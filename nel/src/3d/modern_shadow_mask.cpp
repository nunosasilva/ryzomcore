// NeL - MMORPG Framework <http://dev.ryzom.com/projects/nel/>
// Copyright (C) 2010  Winch Gate Property Limited
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "std3d.h"

#include "nel/3d/modern_shadow.h"
#include "nel/3d/scene.h"
#include "nel/3d/render_trav.h"
#include "nel/3d/driver.h"
#include "nel/3d/shadow_map_manager.h"
#include "nel/3d/texture_mem.h"
#include "nel/3d/transform.h"
#include "nel/misc/aabbox.h"

#include <cstdlib>
#include <cstring>

using namespace NLMISC;

namespace NL3D {

// Cascade world half sizes (around the camera). The first cascade is the
// readable one near the player; the second covers the visible mid distance.
static const float	NL_MODERN_SHADOW_RADIUS[2] = { 40.f, 80.f };
static const uint	NL_MODERN_SHADOW_SIZE = 2048;

CModernShadowMask::CModernShadowMask(CShadowMapManager *owner)
	: _Owner(owner), _Size(NL_MODERN_SHADOW_SIZE), _Active(false), _NumCascades(0)
{
	for (uint i = 0; i < 2; ++i)
		_Split[i] = 0.f;
}

CModernShadowMask::~CModernShadowMask()
{
	releaseMasks();
}

bool CModernShadowMask::isEnabled(CScene *scene) const
{
	if (!scene || !scene->getShadowsEnabled())
		return false;

	IDriver *driver = scene->getRenderTrav().getDriver();
	if (!driver || !driver->supportsModernShadows())
		return false;

	// Development-only opt-in. The user-facing gate is the scene's Shadows
	// flag (client cfg Shadows); this switch will be removed before merge.
	static const bool devOptIn = getenv("NLVK_MODERN_SHADOWS") != NULL;
	return devOptIn;
}

bool CModernShadowMask::ensureMasks()
{
	if (_Mask[0] && _Mask[1])
		return true;

	releaseMasks();
	for (uint i = 0; i < 2; ++i)
	{
		const uint32 bytes = 4 * _Size * _Size;
		uint8 *mem = new uint8[bytes];
		memset(mem, 0, bytes);
		CTextureMem *tex = new CTextureMem(mem, bytes, true, false, _Size, _Size);
		tex->setWrapS(ITexture::Clamp);
		tex->setWrapT(ITexture::Clamp);
		tex->setFilterMode(ITexture::Linear, ITexture::LinearMipMapOff);
		tex->generate();
		tex->setReleasable(false);
		tex->setRenderTarget(true);
		_Mask[i] = tex;
	}
	return true;
}

bool CModernShadowMask::render(CScene *scene, const std::vector<CTransform*> &casters)
{
	_Active = false;
	if (!isEnabled(scene))
		return false;
	if (!ensureMasks())
		return false;

	IDriver *driver = scene->getRenderTrav().getAuxDriver();
	if (!driver)
		return false;

	CMaterial	&castMat = _Owner->getCasterShadowMaterial();
	ITexture	*previousTarget = driver->getRenderTarget();
	CViewport	bkupViewport;
	driver->getViewport(bkupViewport);
	const bool	bkupFog = driver->fogEnabled();

	driver->enableFog(false);
	driver->setColorMask(false, false, false, true);

	const CVector	sun = scene->getSunDirection();
	const CVector	camPos = scene->getRenderTrav().CamPos;

	_NumCascades = 2;
	for (uint i = 0; i < 2; ++i)
	{
		// Build the same kind of world -> [0,1]^3 camera the legacy caster
		// maps use, but fitted to a box around the camera instead of a single
		// caster bbox. The light looks along J; the box is the world AABB
		// transformed into light space.
		CMatrix		cameraMatrix;
		cameraMatrix.setArbitraryRotJ(sun);
		CAABBox		worldBox;
		worldBox.setCenter(camPos);
		worldBox.setHalfSize(CVector(NL_MODERN_SHADOW_RADIUS[i], NL_MODERN_SHADOW_RADIUS[i], NL_MODERN_SHADOW_RADIUS[i]));
		CAABBox		localBox = CAABBox::transformAABBox(cameraMatrix.inverted(), worldBox);
		cameraMatrix.translate(localBox.getMin());
		cameraMatrix.setRot(
			cameraMatrix.getI() * localBox.getSize().x,
			cameraMatrix.getJ() * localBox.getSize().y,
			cameraMatrix.getK() * localBox.getSize().z);

		_WorldToMask[i] = cameraMatrix;
		_Split[i] = NL_MODERN_SHADOW_RADIUS[i];

		driver->setRenderTarget(_Mask[i], 0, 0, _Size, _Size);
		driver->setColorMask(false, false, false, true);
		CScissor	scissor;
		scissor.initFullScreen();
		driver->setupScissor(scissor);
		driver->clear2D(CRGBA(0, 0, 0, 0));

		for (uint c = 0; c < casters.size(); ++c)
		{
			if (casters[c])
				casters[c]->renderModernShadowCaster(driver, cameraMatrix, castMat);
		}
	}

	driver->setRenderTarget(previousTarget);
	driver->setColorMask(true, true, true, true);
	driver->setupViewport(bkupViewport);
	driver->enableFog(bkupFog);
	CScissor	scissor;
	scissor.initFullScreen();
	driver->setupScissor(scissor);
	scene->getRenderTrav().setupDriverCamera();

	// Publish the masks before the opaque pass samples them. The masks are
	// already back in SHADER_READ_ONLY layout (setRenderTarget(previous)).
	CModernShadowState	state;
	state.Enabled = true;
	state.NumCascades = _NumCascades;
	for (uint i = 0; i < _NumCascades; ++i)
	{
		state.CascadeSplits[i] = _Split[i];
		memcpy(state.WorldToMask[i], _WorldToMask[i].get(), sizeof(state.WorldToMask[i]));
		state.Mask[i] = _Mask[i];
		state.TexelWorldSize[i] = 1.f / (float)_Size;
	}
	driver->setModernShadowState(state);

	_Active = true;
	return true;
}

bool CModernShadowMask::apply(CScene *scene)
{
	(void)scene;
	return _Active;
}

void CModernShadowMask::releaseMasks()
{
	for (uint i = 0; i < 2; ++i)
		_Mask[i] = NULL;
}

} // NL3D
