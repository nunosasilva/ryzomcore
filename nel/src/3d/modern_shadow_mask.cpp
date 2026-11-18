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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace NLMISC;

namespace NL3D {

// Cascade world half sizes (around the camera). The first cascade is the
// readable one near the player; the second covers the visible mid distance.
static const float	NL_MODERN_SHADOW_RADIUS[2] = { 40.f, 80.f };
static const uint	NL_MODERN_SHADOW_SIZE = 2048;
/// Depth margin kept around the casters' light-space extent (fraction of the
/// cascade box), so a caster whose bbox is slightly off is not clipped.
static const float	NL_MODERN_SHADOW_DEPTH_MARGIN = 0.02f;
/// Caster-side depth offset in metres along the sun (the equivalent of a
/// polygon offset for the depth mask): the casters are drawn with the light
/// camera's near plane pushed back by this much, so a caster's stored depth is
/// that much farther from the sun than the surface it stands on.
///
/// This is *not* the cause of the "fence shadow starts slightly towards the
/// sun" report: the reference does it too. The user reproduced the same sunward
/// start on two machines (Radeon and Intel) with the official upstream
/// `ryzom_client`; it is only less visible there because the reference shadows
/// are fuzzier and lighter. So the offset is not tuned to cancel it, it is kept
/// at the value that is safest against contact acne.
///
/// One centimetre is that value. With the second-depth (farthest, MAX) mask the
/// offset only ever pushes the stored depth away from the sun, which can add
/// shadow but can never leave a lit speckle inside a shadow: acne-safe by
/// construction. Its cost is a start 1.7 cm earlier along the ground at the
/// 36 degree sun, i.e. in the direction the reference already shows. Zero (the
/// contact is exact on paper) is a plausible alternative but leaves no margin
/// beyond the half-texel receiver tolerance; a negative value eats that
/// tolerance and brings acne back at silhouette edges.
static const float	NL_MODERN_SHADOW_CASTER_OFFSET = 0.01f;
/// Receiver tolerance, in mask texels of surface slope along the sun. A
/// receiver at the same depth as the caster must read lit, and the stored
/// depth is bilinearly filtered, so the comparison needs a little slack; half
/// a texel is enough now that the stored depth itself is exact.
static const float	NL_MODERN_SHADOW_BIAS_TEXELS = 0.5f;
/// Penumbra width, in mask texels. The depth comparison alone has a hard edge
/// at the receiver's own tolerance, which reads as a staircase along the mask
/// lattice at close range; stretching the smoothstep over several texels makes
/// the shadow diffuse. Only the softness grows: the bias above stays at half a
/// texel, so the shadow stays attached at the base.
///
/// Chosen from an A/B at the Loria camp (the hitching rail's beam and its
/// posts, the Mektoub's legs, same camera/hour): 4 leaves the edge crisp, 8 is
/// hazier but washes the *thin* shadows out (the rail's band and a post's
/// shadow lose most of their contrast), 6 is visibly softer than 4 while a
/// thin caster still reads and the contact stays attached. Raise this one
/// number for an even softer look (the strength and the bias are separate).
static const float	NL_MODERN_SHADOW_SOFTNESS_TEXELS = 6.f;
/// Largest change of the published sun share per frame. The share comes from
/// the light cycle's sun and ambient colours (weather + day cycle). If those
/// colours step - a weather change, or a frame where they are not settled yet
/// - the darkening of every shadowed surface changes with them, which reads as
/// "the shadows brightened for a frame". With this limit the strength can only
/// fade: at 60 fps a 0.25 step takes 0.2 s instead of one frame.
static const float	NL_MODERN_SHADOW_SUN_SLEW = 0.02f;

namespace {

/** NLVK_SHADOW_DEBUG: opt-in per-event trace of the published state.
  */
const char	*shadowDebugEnv()
{
	static const char *env = getenv("NLVK_SHADOW_DEBUG");
	return env ? env : "";
}

bool	shadowDebugEnabled()
{
	return *shadowDebugEnv() != 0;
}

} // anonymous namespace

void	modernShadowDebugLog(const char *event, const char *reason,
	CScene *scene, IDriver *driver, const CModernShadowState *state,
	uint casters, ITexture *mask0, const char *extra)
{
	if (!shadowDebugEnabled())
		return;
	nlinfo("SHADOWFRAME f=%llu scene=%p nrender=%llu frame=%llu event=%s reason=%s "
		"enabled=%d sun=%.3f cascades=%u casters=%u mask0=%p%s%s",
		(unsigned long long)(driver ? driver->getSwapBufferCounter() : 0),
		(void *)scene,
		(unsigned long long)(scene ? scene->getNumRender() : 0),
		(unsigned long long)(scene ? scene->getFrameId() : 0),
		event, reason,
		state ? (state->Enabled ? 1 : 0) : 0,
		state ? state->SunStrength : 0.f,
		state ? state->NumCascades : 0u,
		casters, (void *)mask0,
		(extra && *extra) ? " " : "", extra ? extra : "");
}

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

CModernShadowMask::CModernShadowMask()
	: _HighPrecision(false), _Size(NL_MODERN_SHADOW_SIZE), _Active(false), _NumCascades(0),
	  _SunStrength(0.f), _HaveSunStrength(false), _Driver(NULL)
{
	for (uint i = 0; i < 2; ++i)
	{
		_Split[i] = 0.f;
		_DepthBias[i] = 0.f;
		_DepthSoftness[i] = 0.f;
	}

	_CasterMaterial.initUnlit();
	_CasterMaterial.setColor(CRGBA::White);
	_CasterMaterial.setZWrite(false);
	_CasterMaterial.setZFunc(CMaterial::always);
	// Both faces are drawn into the mask. Culling the caster's lit side would
	// stop it shadowing itself, but it also retracts every silhouette by the
	// object's thickness along the light (fence shadows started beside their
	// post) and drops single-layer shells entirely (a sun-facing plank wall
	// stopped casting, so a shadow behind it showed through it). Self-shadowing
	// is prevented by the second-depth (MAX) mask itself: the farthest surface
	// along the light is stored, and a fragment of the caster cannot be behind
	// its own far side within a texel.
	_CasterMaterial.setDoubleSided(true);
	// The mask stores a depth, not a coverage: while the depth pass is on the
	// driver ignores these blend factors and blends with MAX, which is what
	// keeps the farthest caster per texel (the second-depth convention). They
	// are kept as the plain additive fallback for a driver without the depth
	// pass.
	_CasterMaterial.setBlend(true);
	_CasterMaterial.setBlendFunc(CMaterial::one, CMaterial::one);
}

CModernShadowMask::~CModernShadowMask()
{
	// The client destroys scenes (logout, character selection, level reload)
	// while the driver lives on, and the driver samples the published state on
	// every draw. Disable it before the mask textures are freed: otherwise the
	// next draw would dereference a dangling ITexture*.
	publishDisabled();
	releaseMasks();
}

void CModernShadowMask::publishDisabled()
{
	if (!_Active || !_Driver)
		return;
	_Active = false;
	const CModernShadowState	disabled;
	_Driver->setModernShadowState(disabled);
	modernShadowDebugLog("publish", "destroy", NULL, _Driver, &disabled, 0, NULL);
}


bool CModernShadowMask::isEnabled(CScene *scene) const
{
	if (!scene || !scene->getShadowsEnabled() || !scene->getModernShadowsEnabled())
		return false;

	IDriver *driver = scene->getRenderTrav().getDriver();
	return driver && driver->supportsModernShadows();
}

void CModernShadowMask::reset(CScene *scene)
{
	if (!_Active)
		return;
	_Active = false;
	IDriver *driver = scene ? scene->getRenderTrav().getAuxDriver() : NULL;
	if (driver)
	{
		const CModernShadowState	disabled;
		driver->setModernShadowState(disabled);
		modernShadowDebugLog("publish", "reset", scene, driver, &disabled, 0, _Mask[0]);
	}
}

bool CModernShadowMask::ensureMasks(bool highPrecision)
{
	if (_Mask[0] && _Mask[1])
		return true;

	releaseMasks();
	_HighPrecision = highPrecision;
	for (uint i = 0; i < 2; ++i)
	{
		const uint32 bytes = 4 * _Size * _Size;
		uint8 *mem = new uint8[bytes];
		// 0 = the minimum depth = "no caster" = lit, the empty value of the
		// second-depth (MAX) mask. The buffer here is only the CPU-side
		// placeholder, a render target is never uploaded.
		memset(mem, 0, bytes);
		CTextureMem *tex = new CTextureMem(mem, bytes, true, false, _Size, _Size);
		tex->setWrapS(ITexture::Clamp);
		tex->setWrapT(ITexture::Clamp);
		tex->setFilterMode(ITexture::Linear, ITexture::LinearMipMapOff);
		tex->generate();
		tex->setReleasable(false);
		tex->setRenderTarget(true);
		// A depth mask needs far more than 8 bits per texel: over a ~100 m
		// cascade an 8-bit channel steps by ~0.4 m, which forces a half-metre
		// receiver bias (shadows lifted off their contact) and paints visible
		// depth steps inside the bands. Drivers that cannot render into a
		// float format ignore this and report hasHighPrecisionShadowMask()
		// false, and the bias below stays at the old 8-bit value.
		tex->setFloatRenderTarget(highPrecision);
		_Mask[i] = tex;
	}
	return true;
}

bool CModernShadowMask::render(CScene *scene, const std::vector<CTransform*> &casters)
{
	if (!isEnabled(scene))
	{
		reset(scene);
		return false;
	}

	IDriver *driver = scene->getRenderTrav().getAuxDriver();
	if (!driver)
	{
		modernShadowDebugLog("skip", "nodriver", scene, NULL, NULL, 0, _Mask[0]);
		return false;
	}
	// Remember the publisher: the destructor has no scene to ask, and the
	// client destroys scenes while the driver (which samples the state on
	// every draw) keeps living.
	_Driver = driver;

	// The cascades are fitted to the player's camera. The water reflection
	// renders the whole scene again through a mirrored camera *inside* the
	// same opaque pass; rebuilding the masks there would cost a full caster
	// pass and publish matrices that no longer match the main view, which the
	// rest of the pass would then sample. Keep the main view's masks: the
	// mirrored scene is only seen through moving water.
	if (scene->getWaterReflectionManager().isRenderingReflection())
	{
		// Keep the main view's masks: the reflection is drawn with the state
		// the last main pass published, so nothing is published here.
		modernShadowDebugLog("skip", "reflection", scene, driver, NULL, 0, _Mask[0]);
		return _Active;
	}

	// Every opaque pass of the main view rebuilds the cascades around its own
	// camera. The water reflection pass returns early above: it is a mirrored
	// view of the same scene and re-using the main cascades is both cheaper
	// and keeps the published matrices valid for the rest of the pass.
	if (!ensureMasks(driver->hasHighPrecisionShadowMask()))
	{
		modernShadowDebugLog("skip", "nomask", scene, driver, NULL, 0, _Mask[0]);
		return false;
	}

	_Active = false;

	CMaterial	&castMat = _CasterMaterial;
	ITexture	*previousTarget = driver->getRenderTarget();
	CViewport	bkupViewport;
	driver->getViewport(bkupViewport);
	const bool	bkupFog = driver->fogEnabled();
	// The casters borrow the driver camera and the model matrix: each of them
	// calls setFrustum()/setupViewMatrix() and then setupModelMatrix() with its
	// own world matrix (see renderModernShadowCaster). setupDriverCamera() puts
	// the camera back below, but the model matrix has no such restore point, so
	// the last caster's world matrix stayed active for the rest of the pass and
	// any later draw that relies on the current matrix inherited it. Save the
	// caller's matrix here and restore it once the camera is back. The cull mode
	// is borrowed the same way: the caster material is double sided, and the
	// next draw must not inherit that either.
	const CMatrix	bkupModelMatrix = driver->getModelMatrix();
	const IDriver::TCullMode	bkupCullMode = driver->getCullMode();

	driver->enableFog(false);
	driver->setColorMask(false, false, false, true);

	// The casters are drawn with the same mega shader as everything else. Turn
	// the shadow state off for the duration of the pass: the mask textures are
	// the render targets here, and a shader must never sample the image it is
	// writing. It would also darken the casters themselves. The mask texel
	// size is still published because the driver stages it per draw.
	{
		CModernShadowState	disabled;
		disabled.MaskTexelUv = 1.f / (float)_Size;
		driver->setModernShadowState(disabled);
	}

	const CVector	sun = scene->getSunDirection();
	const CVector	camPos = scene->getRenderTrav().CamPos;

	// A zero-length or non-finite sun direction makes setArbitraryRotJ()
	// produce a NaN camera, hence a NaN world -> mask matrix. Every bbox then
	// compares false (all casters pass), and the receiver samples undefined
	// UVs. Keep the last published masks for a frame instead: no visual change
	// on a normal frame, no NaN anywhere on the odd one.
	if (!(sun.x == sun.x) || !(sun.y == sun.y) || !(sun.z == sun.z) ||
		sun.sqrnorm() < 1e-6f)
	{
		modernShadowDebugLog("skip", "badsun", scene, driver, NULL, 0, _Mask[0]);
		return _Active;
	}

	_NumCascades = 2;
	float	biasMillimetres[2] = { 0.f, 0.f };
	// Debug trace for cascade 0: how many casters project into the box, the
	// light-space span the range actually used and whether a caster bbox had
	// to be rejected as degenerate.
	uint	inBox0 = 0;
	float	span0 = 0.f;
	float	sunShareRaw = 0.f;
	bool	bad0 = false;
	// Reused per-cascade caster set (see the bbox rejection in the cascade loop).
	std::vector<CTransform*>	cascadeCasters;
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

		// The depth axis must contain every receiver the shader will test,
		// not just the casters: it is the same box the receiver matrix maps to
		// [0,1], so the whole box is the range by default. Fitting it to the
		// casters (as this pass used to) left every receiver beyond the
		// casters' light-space extent with a depth > 1, which compares greater
		// than the empty clear value (0 = lit) and turned whole stretches
		// of desert into one giant shadow whose extent moved with the camera,
		// as casters entered and left the fit.
		//
		// A caster whose bbox sticks out of the box (a tall cliff above the
		// cascade) only *extends* the range, so it is not clipped. The image
		// axes of the box are never touched.
		//
		// The driver inserts its NeL->GL change basis between the projection
		// and the model-view matrix (mvp = P * _ChangeBasis * MV), so the
		// caster pass rasterises the box's x into the image U, its z into the
		// image V, and its y -- the axis along the sun, i.e. cameraMatrix's J
		// -- into the window depth. That is the same naming the legacy path
		// uses with CShadowMapProjector::_XYZToUWVMatrix.
		CMatrix	boxToMask = cameraMatrix.inverted();
		float	rawLo = 0.f, rawHi = 1.f;
		bool	badCaster = false;
		const float	texelUv = 1.f / (float)_Size;
		// Casters that can contribute to *this* cascade: their light-space bbox
		// must overlap the box in the image plane and in depth, and it must be
		// large enough to cover a mask texel (a sub-texel prop at distance
		// cannot paint a readable shadow). This is the main cost saver: the
		// full set contains every registered caster of the scene, most of them
		// far outside the near cascade.
		cascadeCasters.clear();
		for (uint c = 0; c < casters.size(); ++c)
		{
			CAABBox bb;
			if (!casters[c] || !casters[c]->computeWorldBBoxForShadow(bb))
				continue;
			// A degenerate or half-initialised caster bbox must never reach the
			// range: an uninitialised bone sphere can report extents around
			// 1e20 (seen in a caster dump), which turns the normalized depth
			// values into noise and clips nothing that a real caster needs.
			const CVector	mn = bb.getMin();
			const CVector	mx = bb.getMax();
			if (!(mx.x >= mn.x && mx.y >= mn.y && mx.z >= mn.z) ||
				fabsf(mn.x) > 1e6f || fabsf(mn.y) > 1e6f || fabsf(mn.z) > 1e6f ||
				fabsf(mx.x) > 1e6f || fabsf(mx.y) > 1e6f || fabsf(mx.z) > 1e6f)
			{
				char	extra[160];
				snprintf(extra, sizeof(extra), "bbox=(%.3g %.3g %.3g)-(%.3g %.3g %.3g)",
					mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
				modernShadowDebugLog("skip", "badcaster", scene, driver, NULL,
					0, _Mask[0], extra);
				badCaster = true;
				continue;
			}
			CVector lo, hi;
			lightSpaceAABBox(boxToMask, bb, lo, hi);
			if (hi.x < 0.f || lo.x > 1.f || hi.z < 0.f || lo.z > 1.f)
				continue; // outside the image plane of the box: it draws nothing here
			if (hi.y < 0.f || lo.y > 1.f)
				continue; // entirely in front of or behind the box along the sun
			if ((hi.x - lo.x) < texelUv && (hi.z - lo.z) < texelUv)
				continue; // smaller than one mask texel: no readable shadow
			cascadeCasters.push_back(casters[c]);
			rawLo = std::min(rawLo, lo.y - NL_MODERN_SHADOW_DEPTH_MARGIN);
			rawHi = std::max(rawHi, hi.y + NL_MODERN_SHADOW_DEPTH_MARGIN);
			if (i == 0)
				++inBox0;
		}
		const float	depthLo = rawLo;
		const float	depthHi = rawHi;
		const float	depthSpan = depthHi - depthLo;
		if (i == 0)
		{
			span0 = depthSpan;
			bad0 = badCaster;
		}
		if (depthSpan <= 1.f)
		{
			// The common case: nothing sticks out, so the range stays exactly
			// the box (depthLo == 0, depthSpan == 1) and no fit is applied.
			cameraMatrix.setRot(iAxis * boxSize.x, jAxis * boxSize.y, kAxis * boxSize.z);
		}
		else
		{
			// The depth axis is J: put the near plane at depthLo and scale J
			// by the extended span. J is a column of the camera rotation, so
			// the world offset is simply jAxis * metres, and the position must
			// be set directly (translate() would scale the offset again).
			cameraMatrix.setPos(cameraMatrix.getPos() + jAxis * (depthLo * boxSize.y));
			cameraMatrix.setRot(iAxis * boxSize.x, jAxis * (boxSize.y * depthSpan), kAxis * boxSize.z);
		}

		// The casters are rendered through a camera whose near plane is pushed
		// back by NL_MODERN_SHADOW_CASTER_OFFSET, while the receivers keep
		// cameraMatrix. Both normalise their own depth range, so this adds
		// offset/extent to every stored depth without touching the receiver:
		// the depth-mask equivalent of glPolygonOffset. The offset is in metres
		// along the sun, the unit the masks are built in.
		const float	depthMetres = boxSize.y * depthSpan;
		const float	casterOffset = NL_MODERN_SHADOW_CASTER_OFFSET;
		CMatrix		casterMatrix = cameraMatrix;
		casterMatrix.setPos(cameraMatrix.getPos() - jAxis * casterOffset);
		casterMatrix.setRot(iAxis * boxSize.x,
			jAxis * (depthMetres + casterOffset), kAxis * boxSize.z);

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
		// Receiver tolerance and soft-edge width, both in metres converted to
		// the mask's normalized light-space depth. The texel size is the one of
		// the box's image plane, which is what the bilinear filter averages.
		const float	texelMetres = std::max(boxSize.x, boxSize.z) / (float)_Size;
		if (_HighPrecision)
		{
			_DepthBias[i] = (NL_MODERN_SHADOW_BIAS_TEXELS * texelMetres) / depthMetres;
			_DepthSoftness[i] = (NL_MODERN_SHADOW_SOFTNESS_TEXELS * texelMetres) / depthMetres;
		}
		else
		{
			// 8-bit mask: the stored depth is rounded to a step of
			// depthMetres/255 (0.3-0.5 m here), so the receiver needs the old
			// one-and-a-half-step tolerance, and the edge has to be soft enough
			// to hide the quantisation itself.
			_DepthBias[i] = 1.5f / 255.f;
			_DepthSoftness[i] = 2.f / 255.f;
		}
		biasMillimetres[i] = _DepthBias[i] * depthMetres * 1000.f;

		driver->setRenderTarget(_Mask[i], 0, 0, _Size, _Size);
		CScissor	scissor;
		scissor.initFullScreen();
		driver->setupScissor(scissor);
		// 0 = no caster in this texel = lit (the empty value of a farthest-depth
		// mask). The caster draws blend with a MAX op, so they only ever raise
		// it with a farther depth.
		driver->setColorMask(true, true, true, true);
		driver->clear2D(CRGBA(0, 0, 0, 0));

		driver->setModernShadowDepthPass(true);
		for (uint c = 0; c < cascadeCasters.size(); ++c)
			cascadeCasters[c]->renderModernShadowCaster(driver, casterMatrix, castMat);
		driver->setModernShadowDepthPass(false);

	}

	driver->setRenderTarget(previousTarget);
	driver->setColorMask(true, true, true, true);
	driver->setupViewport(bkupViewport);
	driver->enableFog(bkupFog);
	driver->setCullMode(bkupCullMode);
	CScissor	scissor;
	scissor.initFullScreen();
	driver->setupScissor(scissor);
	scene->getRenderTrav().setupDriverCamera();
	// setupDriverCamera() re-set the view matrix, so the model matrix has to be
	// re-set up after it (setupModelMatrix() must follow the camera).
	driver->setupModelMatrix(bkupModelMatrix);

	// Publish the masks before the opaque pass samples them. The masks are
	// already back in SHADER_READ_ONLY layout (setRenderTarget(previous)).
	CModernShadowState	state;
	state.Enabled = true;
	state.NumCascades = _NumCascades;
	state.MaskTexelUv = 1.f / (float)_Size;
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
		float strength = std::min(0.50f, std::max(0.25f, share));
		// Never let the strength pop between two frames (see
		// NL_MODERN_SHADOW_SUN_SLEW): a one-frame step would brighten every
		// shadowed surface at once.
		if (_HaveSunStrength)
			strength = std::max(_SunStrength - NL_MODERN_SHADOW_SUN_SLEW,
				std::min(_SunStrength + NL_MODERN_SHADOW_SUN_SLEW, strength));
		_SunStrength = strength;
		_HaveSunStrength = true;
		state.SunStrength = strength;
		sunShareRaw = share;
	}
	for (uint i = 0; i < _NumCascades; ++i)
	{
		state.CascadeSplits[i] = _Split[i];
		memcpy(state.WorldToMask[i], _WorldToMask[i].get(), sizeof(state.WorldToMask[i]));
		state.Mask[i] = _Mask[i];
		state.DepthBias[i] = _DepthBias[i];
		state.DepthSoftness[i] = _DepthSoftness[i];
	}
	driver->setModernShadowState(state);
	{
		char	extra[240];
		snprintf(extra, sizeof(extra),
			"cam=(%.1f %.1f %.1f) inbox=%u span=%.4g bad=%d sunraw=%.3f",
			camPos.x, camPos.y, camPos.z, inBox0, span0,
			bad0 ? 1 : 0, sunShareRaw);
		modernShadowDebugLog("publish", "ok", scene, driver, &state,
			(uint)casters.size(), _Mask[0], extra);
	}

	// One line when the modern path first runs, so a support log shows which
	// shadow implementation produced the frame. Nothing is logged per frame.
	static bool logged = false;
	if (!logged)
	{
		logged = true;
		nlinfo("MODERNSHADOW: modern sun shadows on: %u cascades, %ux%u %s masks, split %.1f m, sun=(%.2f %.2f %.2f), bias %.1f mm",
			(unsigned)state.NumCascades, _Size, _Size, _HighPrecision ? "R32F" : "RGBA8",
			state.CascadeSplits[0], sun.x, sun.y, sun.z, (double)biasMillimetres[0]);
	}

	_Active = true;
	return true;
}

void CModernShadowMask::releaseMasks()
{
	// Dropping the textures while the driver still holds the state would leave
	// it sampling freed memory. publishDisabled() is a no-op when there is
	// nothing published, so it is safe both from the destructor and from a
	// future re-creation path.
	publishDisabled();
	for (uint i = 0; i < 2; ++i)
	{
		_Mask[i] = NULL;
	}
}

} // NL3D
