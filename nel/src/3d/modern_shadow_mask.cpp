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

uint32	NLModernShadowCasterTris = 0;
uint32	NLModernShadowCasterMeshes = 0;

// Cascade world half sizes (around the camera). The first cascade is the
// readable one near the player; the second covers the visible mid distance.
static const float	NL_MODERN_SHADOW_RADIUS[2] = { 40.f, 80.f };
static const uint	NL_MODERN_SHADOW_SIZE = 2048;

CModernShadowMask::CModernShadowMask(CShadowMapManager *owner)
	: _Owner(owner), _Size(NL_MODERN_SHADOW_SIZE), _Active(false), _NumCascades(0)
{
	for (uint i = 0; i < 2; ++i)
		_Split[i] = 0.f;

	_CasterMaterial.initUnlit();
	_CasterMaterial.setColor(CRGBA::White);
	_CasterMaterial.setZWrite(false);
	_CasterMaterial.setZFunc(CMaterial::always);
	// The casters are drawn as they are, without culling: several Ryzom shapes
	// are single-layer shells whose only faces point away from the light, and
	// culling them would leave the mask empty.
	_CasterMaterial.setDoubleSided(true);
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

	IDriver *driver = scene->getRenderTrav().getAuxDriver();
	if (!driver)
		return false;

	// Every opaque pass rebuilds the cascades around its own camera. The main
	// pass is what the player sees; the water env map renders the same scene
	// with a reflected camera from time to time, which costs one extra mask
	// render but is immediately replaced by the next frame's main pass.
	if (!ensureMasks())
		return false;

	_Active = false;

	NLModernShadowCasterTris = 0;
	NLModernShadowCasterMeshes = 0;

	static uint lastLoggedCasters = 0xFFFFFFFFu;
	if ((uint)casters.size() != lastLoggedCasters)
	{
		lastLoggedCasters = (uint)casters.size();
		nlinfo("MODERNSHADOW casters now=%u size=%u", (unsigned)casters.size(), (unsigned)_Size);
	}

	// TEMP: what the casters are and where they land in cascade 0.
	{
		static bool loggedBoxes = false;
		if (!loggedBoxes && !casters.empty())
		{
			loggedBoxes = true;
			const CMatrix &w2m = _WorldToMask[0];
			for (uint c = 0; c < casters.size() && c < 6; ++c)
			{
				CAABBox bb;
				if (!casters[c] || !casters[c]->computeWorldBBoxForShadow(bb))
				{
					nlinfo("MODERNSHADOW caster %u: no bbox (no-op caster)", c);
					continue;
				}
				const CVector cc = bb.getCenter(), hs = bb.getHalfSize();
				const CVector p0 = w2m * (cc - hs);
				const CVector p1 = w2m * (cc + hs);
				nlinfo("MODERNSHADOW caster %u: center=(%.1f %.1f %.1f) half=(%.1f %.1f %.1f) uv=(%.3f..%.3f, %.3f..%.3f)",
					c, cc.x, cc.y, cc.z, hs.x, hs.y, hs.z,
					std::min(p0.x, p1.x), std::max(p0.x, p1.x),
					std::min(p0.y, p1.y), std::max(p0.y, p1.y));
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

	// The casters are drawn with the same mega shader as everything else. Turn
	// the shadow state off for the duration of the pass: the mask textures are
	// the render targets here, and a shader must never sample the image it is
	// writing. It would also darken the casters themselves.
	driver->setModernShadowState(CModernShadowState());

	const CVector	sun = scene->getSunDirection();
	// TEMP: which space is the driver working in?
	{
		static bool logged = false;
		if (!logged)
		{
			logged = true;
			const CMatrix view = driver->getViewMatrix();
			const CVector vt = view.getPos();
			const CVector cp = scene->getRenderTrav().CamPos;
			nlinfo("MODERNSHADOW space: camPos=(%.2f %.2f %.2f) viewT=(%.2f %.2f %.2f)",
				cp.x, cp.y, cp.z, vt.x, vt.y, vt.z);
		}
	}
	// TEMP: the mask is only right for the sun direction it was built with.
	{
		static CVector lastSun(0.f, 0.f, 0.f);
		if ((sun - lastSun).norm() > 0.02f)
		{
			lastSun = sun;
			nlinfo("MODERNSHADOW sun now=(%.3f %.3f %.3f)", sun.x, sun.y, sun.z);
		}
	}
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

		// cameraMatrix is the camera-to-world matrix of the light camera (the
		// same convention as CShadowMap::buildCasterCameraMatrix); the shader
		// needs world -> mask UV, i.e. its inverse.
		_WorldToMask[i] = cameraMatrix.inverted();
		_Split[i] = NL_MODERN_SHADOW_RADIUS[i];

		driver->setRenderTarget(_Mask[i], 0, 0, _Size, _Size);
		CScissor	scissor;
		scissor.initFullScreen();
		driver->setupScissor(scissor);
		// TEMP probe: fill cascade 0 with white and cascade 1 with black to
		// tell "the mask is never written" from "the casters write nothing".
		static const bool testFill = getenv("NLVK_SHADOW_TESTFILL") != NULL;
		if (testFill)
		{
			driver->setColorMask(true, true, true, true);
			driver->clear2D(i == 0 ? CRGBA(255, 255, 255, 255) : CRGBA(0, 0, 0, 255));
		}
		driver->setColorMask(false, false, false, true);
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
	// TEMP probe: publish no mask, so the shader binds the white fallback
	// texture. With the coverage view on, the screen must turn white if the
	// mask sampling path works at all.
	if (getenv("NLVK_SHADOW_WHITE"))
	{
		state.Mask[0] = NULL;
		state.Mask[1] = NULL;
	}

	// Development view toggles; see CModernShadowState::DebugMode.
	state.FlipU = CFile::isExists("tmp/shadow_flipu");
	state.FlipV = !CFile::isExists("tmp/shadow_noflipv");
	if (CFile::isExists("tmp/shadow_cross"))
		state.DebugMode = 6;
	else if (CFile::isExists("tmp/shadow_maskview"))
		state.DebugMode = 4;
	else if (CFile::isExists("tmp/shadow_magenta"))
		state.DebugMode = 4;
	else if (CFile::isExists("tmp/shadow_raw"))
		state.DebugMode = 3;
	else if (CFile::isExists("tmp/shadow_ubodbg"))
		state.DebugMode = 2;
	else if (CFile::isExists("tmp/shadow_debug"))
		state.DebugMode = 1;
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
	// Development marker: the cascade 0 UV of a ground point five metres in
	// front of the camera. The shader draws a cross where it sees that UV, so
	// the published matrix can be compared with the CPU's own projection.
	{
		const CVector look = scene->getRenderTrav().CamLook;
		CVector ahead = scene->getRenderTrav().CamPos + look * 5.f;
		ahead.z = 0.f;
		const CVector uv = _WorldToMask[0] * ahead;
		state.Screen[2] = uv.x;
		state.Screen[3] = uv.y;
		static bool logged = false;
		if (!logged)
		{
			logged = true;
			nlinfo("MODERNSHADOW w2m: |I|=%.5f |J|=%.5f |K|=%.5f camPosUV=(%.3f %.3f) aheadUV=(%.3f %.3f)",
				_WorldToMask[0].getI().norm(), _WorldToMask[0].getJ().norm(), _WorldToMask[0].getK().norm(),
				_WorldToMask[0].get() ? (_WorldToMask[0] * scene->getRenderTrav().CamPos).x : 0.f,
				(_WorldToMask[0] * scene->getRenderTrav().CamPos).y, uv.x, uv.y);
		}
	}
	driver->setModernShadowState(state);
	static uint32 lastLoggedTris = 0xFFFFFFFFu;
	if (NLModernShadowCasterTris != lastLoggedTris)
	{
		lastLoggedTris = NLModernShadowCasterTris;
		const CVector uv0 = _WorldToMask[0] * camPos;
		const CVector uv1 = _WorldToMask[1] * camPos;
		nlinfo("MODERNSHADOW pass: meshes=%u tris=%u camUV=(%.3f %.3f) / (%.3f %.3f) sun=(%.2f %.2f %.2f)",
			NLModernShadowCasterMeshes, NLModernShadowCasterTris,
			uv0.x, uv0.y, uv1.x, uv1.y, sun.x, sun.y, sun.z);
	}
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
