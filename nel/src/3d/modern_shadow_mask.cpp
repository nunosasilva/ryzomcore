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
/// Depth margin kept around the casters' light-space extent (fraction of the
/// cascade box), so a caster whose bbox is slightly off is not clipped.
static const float	NL_MODERN_SHADOW_DEPTH_MARGIN = 0.02f;

namespace {

/** Light-space AABB of a world AABB under an affine map to the cascade box.
  * Fills lo/hi with the box-space bounds of the 8 transformed corners.
  */
void	lightSpaceAABBox(const CMatrix &worldToMask, const CAABBox &world,
	CVector &lo, CVector &hi)
{
	const CVector	center = worldToMask * world.getCenter();
	const CVector	half = world.getHalfSize();
	const CVector	i = worldToMask.getI();
	const CVector	j = worldToMask.getJ();
	const CVector	k = worldToMask.getK();
	CVector	extent;
	extent.x = fabsf(i.x) * half.x + fabsf(j.x) * half.y + fabsf(k.x) * half.z;
	extent.y = fabsf(i.y) * half.x + fabsf(j.y) * half.y + fabsf(k.y) * half.z;
	extent.z = fabsf(i.z) * half.x + fabsf(j.z) * half.y + fabsf(k.z) * half.z;
	lo = center - extent;
	hi = center + extent;
}

} // anonymous namespace

CModernShadowMask::CModernShadowMask(CShadowMapManager *owner)
	: _Owner(owner), _Size(NL_MODERN_SHADOW_SIZE), _Active(false), _NumCascades(0)
{
	for (uint i = 0; i < 2; ++i)
	{
		_Split[i] = 0.f;
		_DepthBias[i] = 0.f;
	}

	_CasterMaterial.initUnlit();
	_CasterMaterial.setColor(CRGBA::White);
	_CasterMaterial.setZWrite(false);
	_CasterMaterial.setZFunc(CMaterial::always);
	// The casters are drawn as they are, without culling: several Ryzom shapes
	// are single-layer shells whose only faces point away from the light, and
	// culling them would leave the mask empty.
	_CasterMaterial.setDoubleSided(true);
	// The mask stores a depth, not a coverage: while the depth pass is on the
	// driver ignores these blend factors and blends with MIN, which is what
	// keeps the nearest caster per texel. They are kept as the plain
	// additive fallback for a driver without the depth pass.
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
		// White = the far end of the light box = no caster = lit, so a mask
		// that is sampled before its first caster pass cannot darken anything.
		memset(mem, 0xFF, bytes);
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

	// The cascades are fitted to the player's camera. The water reflection
	// renders the whole scene again through a mirrored camera *inside* the
	// same opaque pass; rebuilding the masks there would cost a full caster
	// pass and publish matrices that no longer match the main view, which the
	// rest of the pass would then sample. Keep the main view's masks: the
	// mirrored scene is only seen through moving water.
	if (scene->getWaterReflectionManager().isRenderingReflection())
		return _Active;

	// Every opaque pass of the main view rebuilds the cascades around its own
	// camera. The water reflection pass returns early above: it is a mirrored
	// view of the same scene and re-using the main cascades is both cheaper
	// and keeps the published matrices valid for the rest of the pass.
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
		const CVector	iAxis = cameraMatrix.getI();
		const CVector	jAxis = cameraMatrix.getJ();
		const CVector	kAxis = cameraMatrix.getK();
		CAABBox		worldBox;
		worldBox.setCenter(camPos);
		worldBox.setHalfSize(CVector(NL_MODERN_SHADOW_RADIUS[i], NL_MODERN_SHADOW_RADIUS[i], NL_MODERN_SHADOW_RADIUS[i]));
		CAABBox		localBox = CAABBox::transformAABBox(cameraMatrix.inverted(), worldBox);
		const CVector	boxMin = localBox.getMin();
		const CVector	boxSize = localBox.getSize();
		// translate() moves along the camera's own basis, so this puts the
		// light camera at the box corner in light space (see the inverse
		// below: boxToMask maps the box exactly to [0,1]^3).
		cameraMatrix.translate(boxMin);
		cameraMatrix.setRot(iAxis * boxSize.x, jAxis * boxSize.y, kAxis * boxSize.z);

		// The mask only stores 8 bits per channel, so the depth axis is fitted
		// to the casters that actually project into the box (the image axes of
		// the box are left untouched): a smaller depth extent means a smaller
		// depth step, and therefore a tighter shadow that starts closer to its
		// caster.
		//
		// The driver inserts its NeL->GL change basis between the projection
		// and the model-view matrix (mvp = P * _ChangeBasis * MV), so the
		// caster pass rasterises the box's x into the image U, its z into the
		// image V, and its y -- the axis along the sun, i.e. cameraMatrix's J
		// -- into the window depth. That is the same naming the legacy path
		// uses with CShadowMapProjector::_XYZToUWVMatrix.
		CMatrix	boxToMask = cameraMatrix.inverted();
		float	depthLo = 1.f, depthHi = 0.f;
		bool	haveDepth = false;
		for (uint c = 0; c < casters.size(); ++c)
		{
			CAABBox bb;
			if (!casters[c] || !casters[c]->computeWorldBBoxForShadow(bb))
				continue;
			CVector lo, hi;
			lightSpaceAABBox(boxToMask, bb, lo, hi);
			if (hi.x < 0.f || lo.x > 1.f || hi.z < 0.f || lo.z > 1.f)
				continue; // outside the image plane of the box: it draws nothing here
			depthLo = std::min(depthLo, lo.y);
			depthHi = std::max(depthHi, hi.y);
			haveDepth = true;
		}
		float	depthSpan = 1.f;
		if (haveDepth)
		{
			depthLo = std::max(0.f, depthLo - NL_MODERN_SHADOW_DEPTH_MARGIN);
			depthHi = std::min(1.f, depthHi + NL_MODERN_SHADOW_DEPTH_MARGIN);
			depthSpan = depthHi - depthLo;
			if (depthSpan < 0.05f)
			{
				// Degenerate (everything coplanar along the light): keep the
				// whole box so the comparison still has a usable range.
				depthLo = 0.f;
				depthSpan = 1.f;
			}
		}
		if (depthSpan < 1.f)
		{
			// The depth axis is J: put the near plane at depthLo and scale J
			// by the fitted span. J is a column of the camera rotation, so the
			// world offset is simply jAxis * metres, and the position must be
			// set directly (translate() would scale the offset again).
			cameraMatrix.setPos(cameraMatrix.getPos() + jAxis * (depthLo * boxSize.y));
			cameraMatrix.setRot(iAxis * boxSize.x, jAxis * (boxSize.y * depthSpan), kAxis * boxSize.z);
		}
		else
			cameraMatrix.setRot(iAxis * boxSize.x, jAxis * boxSize.y, kAxis * boxSize.z);

		// cameraMatrix is the camera-to-world matrix of the light camera (the
		// same convention as CShadowMap::buildCasterCameraMatrix); the shader
		// needs world -> mask UV, i.e. its inverse -- with the same axis
		// permutation the rasteriser applies: (x,y,z) -> (x,z,y), the legacy
		// CShadowMapProjector::_XYZToUWVMatrix. Then sunP.xy is the mask's
		// image plane and sunP.z is exactly the depth the caster pass wrote
		// with gl_FragCoord.z.
		CMatrix	xyzToUvw;
		xyzToUvw.setRot(CVector::I, CVector::K, CVector::J);
		_WorldToMask[i] = xyzToUvw * cameraMatrix.inverted();
		_Split[i] = NL_MODERN_SHADOW_RADIUS[i];
		// Receiver-side depth bias, in the mask's normalized light-space depth.
		// The stored depth is the nearest caster rounded to the nearest 8-bit
		// step (<= 0.5 step of error), and the receiver's surface can slope
		// about one step over a texel; 1.5 steps covers both. In metres this is
		// 1.5/255 of the fitted depth extent (see NL_MODERN_SHADOW_DEPTH_MARGIN
		// above), so the tighter fit above also tightens the shadow contact.
		_DepthBias[i] = 1.5f / 255.f;
		if (i == 0)
		{
			static float lastLoggedSpan = -1.f;
			if (fabsf(depthSpan - lastLoggedSpan) > 0.05f)
			{
				lastLoggedSpan = depthSpan;
				nlinfo("MODERNSHADOW cascade 0 depth: span=%.3f of the box (%.1f m) bias=%.1f m",
					depthSpan, boxSize.y * depthSpan, _DepthBias[0] * boxSize.y * depthSpan);
			}
		}

		driver->setRenderTarget(_Mask[i], 0, 0, _Size, _Size);
		CScissor	scissor;
		scissor.initFullScreen();
		driver->setupScissor(scissor);
		// TEMP probe: fill cascade 0 with white and cascade 1 with black to
		// tell "the mask is never written" from "the casters write nothing".
		static const bool testFill = getenv("NLVK_SHADOW_TESTFILL") != NULL;
		// Far (white) = no caster in this texel = lit. The caster draws blend
		// with a MIN op, so they only ever replace it with a nearer depth.
		driver->setColorMask(true, true, true, true);
		if (testFill)
			driver->clear2D(i == 0 ? CRGBA(255, 255, 255, 255) : CRGBA(0, 0, 0, 255));
		else
			driver->clear2D(CRGBA(255, 255, 255, 255));

		driver->setModernShadowDepthPass(true);
		for (uint c = 0; c < casters.size(); ++c)
		{
			if (casters[c])
				casters[c]->renderModernShadowCaster(driver, cameraMatrix, castMat);
		}
		driver->setModernShadowDepthPass(false);
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
	if (CFile::isExists("tmp/shadow_cascades"))
		state.DebugMode = 18;
	else if (CFile::isExists("tmp/shadow_masklookup"))
		state.DebugMode = 17;
	else if (CFile::isExists("tmp/shadow_maskamp"))
		state.DebugMode = 16;
	else if (CFile::isExists("tmp/shadow_depthnum"))
		state.DebugMode = 15;
	else if (CFile::isExists("tmp/shadow_depthdiff"))
		state.DebugMode = 12;
	else if (CFile::isExists("tmp/shadow_depthprobe"))
		state.DebugMode = 11;
	else if (CFile::isExists("tmp/shadow_magprobe"))
		state.DebugMode = 10;
	else if (CFile::isExists("tmp/shadow_spaceprobe"))
		state.DebugMode = 9;
	else if (CFile::isExists("tmp/shadow_wprobe"))
		state.DebugMode = 8;
	else if (CFile::isExists("tmp/shadow_uvramp"))
		state.DebugMode = 7;
	else if (CFile::isExists("tmp/shadow_cross"))
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
		state.DepthBias[i] = _DepthBias[i];
	}
	// Development marker: the cascade 0 UV of a caster base that can be seen on
	// screen (the largest registered caster, usually the tent at the camp). The
	// shader draws a cross where it sees that UV, so the published matrix can be
	// compared with the CPU's own projection of an object we can point at.
	{
		CAABBox best;
		float bestArea = 0.f;
		bool haveBest = false;
		for (uint c = 0; c < casters.size(); ++c)
		{
			CAABBox bb;
			if (!casters[c] || !casters[c]->computeWorldBBoxForShadow(bb))
				continue;
			const CVector sz = bb.getSize();
			if (!(sz.x > 0.5f && sz.y > 0.5f && sz.z > 0.2f) ||
				sz.x > 60.f || sz.y > 60.f || sz.z > 60.f)
				continue;
			// Only casters whose base lands inside cascade 0 make a marker we can
			// actually see on screen.
			CVector base = bb.getCenter();
			base.z = bb.getMin().z;
			const CVector baseUv = _WorldToMask[0] * base;
			if (baseUv.x < 0.05f || baseUv.x > 0.95f || baseUv.y < 0.05f || baseUv.y > 0.95f)
				continue;
			const float area = sz.x * sz.y * sz.z;
			if (area > bestArea)
			{
				bestArea = area;
				best = bb;
				haveBest = true;
			}
		}
		CVector ahead = scene->getRenderTrav().CamPos;
		if (haveBest)
		{
			ahead = best.getCenter();
			ahead.z = best.getMin().z;
			const CVector bsz = best.getSize();
			nlinfo("MODERNSHADOW marker caster: base=(%.1f %.1f %.1f) size=(%.1f %.1f %.1f)",
				ahead.x, ahead.y, ahead.z, bsz.x, bsz.y, bsz.z);
		}
		const CVector uv = _WorldToMask[0] * ahead;
		// The shader flips V (the negative-height viewport of the mask pass),
		// so compare against the flipped value.
		state.Screen[2] = uv.x;
		state.Screen[3] = 1.f - uv.y;
		static bool logged = false;
		if (!logged)
		{
			logged = true;
			nlinfo("MODERNSHADOW w2m: |I|=%.5f |J|=%.5f |K|=%.5f camPosUV=(%.3f %.3f) casterBaseUV=(%.3f %.3f) at=(%.1f %.1f %.1f)",
				_WorldToMask[0].getI().norm(), _WorldToMask[0].getJ().norm(), _WorldToMask[0].getK().norm(),
				_WorldToMask[0].get() ? (_WorldToMask[0] * scene->getRenderTrav().CamPos).x : 0.f,
				(_WorldToMask[0] * scene->getRenderTrav().CamPos).y, uv.x, uv.y,
				ahead.x, ahead.y, ahead.z);
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
