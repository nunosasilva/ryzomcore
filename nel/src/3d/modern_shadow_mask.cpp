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
#include "nel/misc/path.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace NLMISC;

namespace NL3D {

// Cascade world half sizes (around the camera). The first cascade is the
// readable one near the player; the second covers the visible mid distance.
static const float	NL_MODERN_SHADOW_RADIUS[2] = { 40.f, 80.f };
static const uint	NL_MODERN_SHADOW_SIZE = 2048;

CModernShadowMask::CModernShadowMask(CShadowMapManager *owner)
	: _Owner(owner), _Size(NL_MODERN_SHADOW_SIZE), _Active(false), _LastMaskFrame(0xFFFFFFFFFFFFFFFFull), _NumCascades(0)
{
	for (uint i = 0; i < 2; ++i)
		_Split[i] = 0.f;

	_CasterMaterial.initUnlit();
	_CasterMaterial.setColor(CRGBA::White);
	_CasterMaterial.setZWrite(false);
	_CasterMaterial.setZFunc(CMaterial::always);
	_CasterMaterial.setDoubleSided(false);
	_CasterMaterial.setBlend(true);
	_CasterMaterial.setBlendFunc(CMaterial::one, CMaterial::one);
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
	static bool logged = false;
	if (!logged)
	{
		logged = true;
		nlinfo("MODERNSHADOW enabled check: sceneShadows=%d driverSupport=%d env=%d",
			(int)scene->getShadowsEnabled(), (int)driver->supportsModernShadows(), (int)devOptIn);
	}
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
	if (!isEnabled(scene))
	{
		_Active = false;
		return false;
	}

	// The cascades are fitted around the rendertrav camera. The same scene is
	// rendered again within one frame for the water reflection and the water
	// env map cube; those passes must keep the player's masks instead of
	// replacing them with ones fitted to a reflected camera.
	if (scene->getFrameId() == _LastMaskFrame)
		return _Active;

	if (!ensureMasks())
		return false;

	_LastMaskFrame = scene->getFrameId();
	_Active = false;

	IDriver *driver = scene->getRenderTrav().getAuxDriver();
	if (!driver)
		return false;

	static uint lastLoggedCasters = 0xFFFFFFFFu;
	if ((uint)casters.size() != lastLoggedCasters)
	{
		lastLoggedCasters = (uint)casters.size();
		nlinfo("MODERNSHADOW casters now=%u size=%u", (unsigned)casters.size(), (unsigned)_Size);
	}

	// TEMP: the largest casters by ground footprint. A big, flat static mesh
	// (a village platform, for instance) covers its whole footprint in a
	// coverage mask, because the mask stores occlusion without depth.
	{
		static bool loggedBoxes = false;
		if (!loggedBoxes && !casters.empty())
		{
			loggedBoxes = true;
			std::vector<std::pair<float, CAABBox> > boxes;
			boxes.reserve(casters.size());
			for (uint c = 0; c < casters.size(); ++c)
			{
				CAABBox bb;
				if (casters[c] && casters[c]->computeWorldBBoxForShadow(bb))
					boxes.push_back(std::make_pair(bb.getSize().x * bb.getSize().y, bb));
			}
			std::sort(boxes.begin(), boxes.end(),
				[](const std::pair<float, CAABBox> &a, const std::pair<float, CAABBox> &b)
				{ return a.first > b.first; });
			const uint n = std::min<uint>(8, (uint)boxes.size());
			for (uint b = 0; b < n; ++b)
			{
				const CAABBox &bb = boxes[b].second;
				const CVector c = bb.getCenter(), s = bb.getSize();
				nlinfo("MODERNSHADOW caster %u: footprint %.0f m2 center=(%.1f %.1f %.1f) size=(%.1f %.1f %.1f)",
					b, boxes[b].first, c.x, c.y, c.z, s.x, s.y, s.z);
			}
		}
	}

	CMaterial	&castMat = _CasterMaterial;
	ITexture	*previousTarget = driver->getRenderTarget();
	CViewport	bkupViewport;
	driver->getViewport(bkupViewport);
	const bool	bkupFog = driver->fogEnabled();

	driver->enableFog(false);
	driver->setColorMask(false, false, false, true);
	// Cull front faces: the ground planes are front-facing to the sun and
	// must not cover the mask; the far building/character silhouettes remain.
	driver->setCullMode(IDriver::CW);

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
	driver->setCullMode(IDriver::CCW);
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
	// Sun share of the current lighting: the landscape lightmaps hold both the
	// sun and the sky, so darkening them by the full shadow at dusk would
	// remove the ambient light too. Ryzom publishes the sun and ambient colors
	// it uses for the lightmaps, so their ratio is the fraction to remove.
	{
		const CRGBA sun = scene->getRenderTrav().SunDiffuse;
		const CRGBA amb = scene->getRenderTrav().SunAmbient;
		const float sunLum = 0.3f * (float)sun.R + 0.59f * (float)sun.G + 0.11f * (float)sun.B;
		const float ambLum = 0.3f * (float)amb.R + 0.59f * (float)amb.G + 0.11f * (float)amb.B;
		const float share = sunLum / std::max(1.f, sunLum + ambLum);
		state.SunStrength = std::min(0.85f, std::max(0.25f, share));
	}
	// Development view toggle; see CModernShadowState::DebugView.
	state.DebugView = CFile::isExists("tmp/shadow_debug");
	// Development A/B: ./tmp/shadow_off keeps the masks but does not darken
	// anything, so the same view can be compared with and without shading
	// without restarting the client.
	if (CFile::isExists("tmp/shadow_off"))
		state.SunStrength = 0.f;
	for (uint i = 0; i < _NumCascades; ++i)
	{
		state.CascadeSplits[i] = _Split[i];
		memcpy(state.WorldToMask[i], _WorldToMask[i].get(), sizeof(state.WorldToMask[i]));
		state.Mask[i] = _Mask[i];
		state.TexelWorldSize[i] = 1.f / (float)_Size;
	}
	driver->setModernShadowState(state);
	static bool loggedState = false;
	if (!loggedState)
	{
		loggedState = true;
		nlinfo("MODERNSHADOW state published: enabled=1 cascades=%u split0=%.1f texel=%.4f masks=%p,%p",
			(unsigned)state.NumCascades, state.CascadeSplits[0], state.TexelWorldSize[0],
			(void*)state.Mask[0], (void*)state.Mask[1]);
	}

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
