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
#include "nel/3d/transform_shape.h"
#include "nel/misc/aabbox.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

using namespace NLMISC;

namespace NL3D {

// Modern sun-shadow quality levels, selected by the client's ShadowsQuality
// option (0 = very low, 1 = low, 2 = medium, 3 = ultra). Each level names its
// cascades, their world half size around the camera, their mask texture sizes
// and how often each cascade is rebuilt. The shader reads the cascade count,
// the splits and one mask texel size per cascade from the published state, so
// a level can change the mask sizes freely; only a skipped cascade needs the
// last published matrix to be kept (see _CascadeValid).
//
// Every level keeps the same *area* (or a bigger one at the top); a level
// trades resolution and refresh rate inside that area. Shrinking the boxes
// (the old 48/70/80 m levels) put the edge of the dynamic shadows inside the
// play area, which read as a hard boundary in the landscape and made the
// cheap levels not worth using.
namespace {

struct CModernShadowLevel
{
	/// Number of cascades built and sampled.
	uint	NumCascades;
	/// World-space half size of each cascade's box around the camera.
	float	Radius[3];
	/// Mask texture size of each cascade.
	uint	Size[3];
	/// Rebuild period of each cascade, in frames (1 = every frame).
	uint	Period[3];
	/// Drop a caster whose light-space image is smaller than this many mask
	/// texels of the cascade: it cannot paint a readable shadow.
	float	MinTexels[3];
};

const CModernShadowLevel	NL_MODERN_SHADOW_LEVELS[] =
{
	// 0 = very low: the full 40 + 90 + 160 m area with every mask at 1024²,
	// and both far cascades rebuilt every other frame. The masks are half as
	// fine as the low level's near cascade, so the shadows are softer and
	// coarser everywhere, but they cover as much ground as the top level.
	// The two half-rate cascades are staggered (see the frame counter below),
	// so a frame builds the near box plus one of the far boxes, never both.
	{ 3, { 40.f, 90.f, 160.f }, { 1024, 1024, 1024 }, { 1, 2, 2 }, { 1.f, 1.5f, 2.f } },
	// 1 = low: the near 40 m box at 2048², the mid 90 m box at 1024² every
	// frame and the far 160 m box at 1024² rebuilt every other frame. The
	// far box is camera-centred, so a one-frame-old far matrix and mask stay
	// valid at 90-160 m; the far mask's coarser texels also drop the
	// sub-2-texel casters that cost draws and paint nothing readable there.
	{ 3, { 40.f, 90.f, 160.f }, { 2048, 1024, 1024 }, { 1, 1, 2 }, { 1.f, 1.5f, 2.f } },
	// 2 = medium: a larger area (40 + 110 + 200 m), the near and mid boxes at
	// 2048² every frame, the far box at 1024² rebuilt every other frame. The
	// far box is twice as large as the low level's, so its 1024² mask is
	// coarse and its caster set is big; a 30 Hz refresh is invisible at
	// 110-200 m and keeps this level between low and ultra.
	{ 3, { 40.f, 110.f, 200.f }, { 2048, 2048, 1024 }, { 1, 1, 2 }, { 1.f, 1.5f, 2.f } },
	// 3 = ultra: the reference level with the area pushed out to 40 + 120 +
	// 240 m, all three masks at 2048² and rebuilt every frame. Dynamic
	// shadows then reach well past the play area, and the baked/dynamic
	// boundary in the landscape is far away. The far mask's texel is ~0.32 m
	// (about 1.5x the old 160 m box's), so the far shadows are soft; the
	// measured far cost is draw-bound (the extra area holds more casters),
	// so the 1.5-texel cull is kept permissive to preserve the reference
	// quality.
	{ 3, { 40.f, 120.f, 240.f }, { 2048, 2048, 2048 }, { 1, 1, 1 }, { 1.f, 1.5f, 1.5f } },
};
const uint	NL_MODERN_SHADOW_LEVEL_COUNT =
	sizeof(NL_MODERN_SHADOW_LEVELS) / sizeof(NL_MODERN_SHADOW_LEVELS[0]);

} // anonymous namespace

/// Depth margin kept around the casters' light-space extent (fraction of the
/// cascade box), so a caster whose bbox is slightly off is not clipped.
static const float	NL_MODERN_SHADOW_DEPTH_MARGIN = 0.02f;
/// Sun-side extension of a cascade's depth range, in metres along the sun.
/// The range is the box plus this much *toward the sun*, and it is a constant
/// of the engine, not a fit to the casters of the frame:
///
/// - A directional light projects every caster that lies up-sun of the box
///   into it, however far away it is: distance along the sun does not matter,
///   only the footprint (the image plane) does. The client's sun direction is
///   the continent's day setup and does not change with the hour (the light
///   cycle reports _LightLevel 0 all day: only the colours follow the clock),
///   so its elevation is ~12 degrees and every shadow is long at every hour.
///   At the mats004 spot the casters whose shadows reach the box sat 171 m,
///   335 m and 387 m up-sun in the three cascades (light-space depth -1.36,
///   -0.88 and -0.51 box units). The previous half-box bound (44-250 m) culled
///   them, and since the box is camera centred, a few metres of walking moved
///   the bound across a caster and its big shadow popped in or out.
/// - The previous behaviour, an exact fit to this frame's casters
///   (rawLo/rawHi), made the published depth scale and the normalised receiver
///   tolerance of the *whole* cascade depend on which casters happened to be in
///   the set: a 50 ms A tap could flip every shadowed fragment. A constant is
///   stable by construction; making it a world distance rather than a box
///   fraction keeps it independent of the sun elevation too.
///
/// The far side needs no extension: a caster beyond the box away from the sun
/// cannot shadow anything inside it. The bound is also the cull: a caster
/// farther up-sun than this is not drawn (it would be clipped by the range
/// anyway). 450 m covers the 387 m the far cascade of the mats004 spot needs,
/// with margin for a taller caster; the debug line's `cut`/`cutat` fields show
/// whether anything is still dropped beyond it.
static const float	NL_MODERN_SHADOW_DEPTH_SUN_EXTEND = 450.f;
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

/** NLVK_SHADOW_EXACT_RANGE: opt-in reproduction of the pre-2026-11-19 depth
  *	range (fit to this frame's casters, no up-sun casters). Kept so the A/D
  *	shade flip can be A/B'd on one machine with one binary; unset in normal
  *	runs, where the range is the constant box extension.
  */
bool	modernShadowExactRange()
{
	static const bool	enabled = getenv("NLVK_SHADOW_EXACT_RANGE") != NULL;
	return enabled;
}

} // anonymous namespace

// ***************************************************************************
bool	isValidModernShadowCasterBBox(const CAABBox &bb)
{
	const CVector	mn = bb.getMin();
	const CVector	mx = bb.getMax();
	// Non-finite, inverted or absurdly large: an uninitialised bone sphere or
	// a shape whose bounds were never built. A zero-span box is left to the
	// per-cascade test, which rejects it as smaller than a mask texel.
	if (!(mx.x >= mn.x && mx.y >= mn.y && mx.z >= mn.z))
		return false;
	return fabsf(mn.x) <= 1e6f && fabsf(mn.y) <= 1e6f && fabsf(mn.z) <= 1e6f &&
		fabsf(mx.x) <= 1e6f && fabsf(mx.y) <= 1e6f && fabsf(mx.z) <= 1e6f;
}

namespace {

/** NLVK_SHADOW_CASTER_CENSUS: once, a couple of seconds after a scene starts
 *	rendering (so the zone IGs have streamed in), log how many of the casters
 *	that pass the near cascade's filter belong to each shape class. It answers
 *	the coverage question "are the walls / fences / props around here plain
 *	CMesh, MRM or multi-LOD?" from the installed binary, without touching the
 *	render path when the variable is unset.
 */
void	casterClassCensus(CScene *scene, const std::vector<CTransform*> &cascadeCasters)
{
	static const char	*env = getenv("NLVK_SHADOW_CASTER_CENSUS");
	if (!env || !*env)
		return;
	static std::map<const CScene*, uint>	frames;
	uint	&frameCount = frames[scene];
	// 120 rendered frames is about two seconds: the first frames of a login
	// are still streaming the packed zone IGs in.
	if (frameCount > 120)
		return;
	if (++frameCount != 120)
		return;

	std::map<std::string, uint>	counts;
	for (uint c = 0; c < cascadeCasters.size(); ++c)
	{
		CTransform	*model = cascadeCasters[c];
		if (!model || !model->isTransformShape())
			continue;
		CTransformShape	*shapeModel = static_cast<CTransformShape *>(model);
		const std::string	className = shapeModel->Shape ? shapeModel->Shape->getClassName() : std::string("<noshape>");
		++counts[className];
	}
	std::string	line;
	for (std::map<std::string, uint>::const_iterator it = counts.begin(); it != counts.end(); ++it)
	{
		char	item[128];
		snprintf(item, sizeof(item), "%s%s=%u", line.empty() ? "" : " ", it->first.c_str(), it->second);
		line += item;
	}
	nlinfo("SHADOWCASTER census cascade0 casters=%u classes: %s",
		(uint)cascadeCasters.size(), line.empty() ? "<none>" : line.c_str());
}

} // anonymous namespace

// ***************************************************************************
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
	: _SunStrength(0.f), _HaveSunStrength(false), _HighPrecision(false),
	  _Active(false), _NumCascades(0), _Quality(1), _FrameCounter(0),
	  _Driver(NULL)
{
	for (uint i = 0; i < 3; ++i)
	{
		_Size[i] = 0;
		_Split[i] = 0.f;
		_DepthBias[i] = 0.f;
		_DepthSoftness[i] = 0.f;
		_CascadeValid[i] = false;
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
	// A cascade with a rebuild period may be skipped on the next frame; make
	// every one rebuild after the masks were not published for a while.
	for (uint i = 0; i < 3; ++i)
		_CascadeValid[i] = false;
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
	const CModernShadowLevel	&level = NL_MODERN_SHADOW_LEVELS[_Quality];
	// The level fixes the cascade count and the mask sizes. A level change
	// (ShadowsQuality edited at runtime) drops the old textures and creates the
	// new ones; the driver's published state is disabled first so it never
	// samples or rebinds a freed texture.
	bool	sizesMatch = true;
	for (uint i = 0; i < 3; ++i)
	{
		if (i < level.NumCascades)
			sizesMatch = sizesMatch && _Mask[i] != NULL && _Size[i] == level.Size[i];
		else
			sizesMatch = sizesMatch && _Mask[i] == NULL;
	}
	if (sizesMatch)
		return true;

	releaseMasks();
	_HighPrecision = highPrecision;
	for (uint i = 0; i < level.NumCascades; ++i)
	{
		_Size[i] = level.Size[i];
		const uint32 bytes = 4 * _Size[i] * _Size[i];
		uint8 *mem = new uint8[bytes];
		// 0 = the minimum depth = "no caster" = lit, the empty value of the
		// second-depth (MAX) mask. The buffer here is only the CPU-side
		// placeholder, a render target is never uploaded.
		memset(mem, 0, bytes);
		CTextureMem *tex = new CTextureMem(mem, bytes, true, false, _Size[i], _Size[i]);
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
	for (uint i = level.NumCascades; i < 3; ++i)
		_Size[i] = 0;
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
	//
	// The quality level comes from the client's ShadowsQuality option. It is
	// clamped here so a hand-edited cfg cannot index outside the table, and a
	// change drops the old masks first: the driver must not keep sampling a
	// texture that is about to be freed, and every cascade has to be rebuilt
	// before it is published with the new matrix.
	const sint	requestedQuality = scene->getModernShadowsQuality();
	const uint	quality = requestedQuality <= 0 ? 0 :
		(uint)std::min<sint>(requestedQuality, (sint)NL_MODERN_SHADOW_LEVEL_COUNT - 1);
	if (quality != _Quality)
	{
		_Quality = quality;
		for (uint i = 0; i < 3; ++i)
			_CascadeValid[i] = false;
		releaseMasks();
	}
	const CModernShadowLevel	&level = NL_MODERN_SHADOW_LEVELS[_Quality];

	if (!ensureMasks(driver->hasHighPrecisionShadowMask()))
	{
		modernShadowDebugLog("skip", "nomask", scene, driver, NULL, 0, _Mask[0]);
		return false;
	}

	// Snapshot of the last published cascade parameters. A mask pass that
	// fails half way (a render target that cannot be bound) must not leave a
	// new matrix, split or bias paired with the previous mask content: the
	// driver keeps the last published state, and the next frame publishes
	// these members, so they have to describe the last *complete* pass.
	CMatrix	bkupWorldToMask[3];
	float	bkupSplit[3], bkupBias[3], bkupSoft[3];
	bool	bkupCascadeValid[3];
	for (uint i = 0; i < 3; ++i)
	{
		bkupWorldToMask[i] = _WorldToMask[i];
		bkupSplit[i] = _Split[i];
		bkupBias[i] = _DepthBias[i];
		bkupSoft[i] = _DepthSoftness[i];
		bkupCascadeValid[i] = _CascadeValid[i];
	}

	_Active = false;

	// A zero-length or non-finite sun direction makes setArbitraryRotJ()
	// produce a NaN camera, hence a NaN world -> mask matrix. Every bbox then
	// compares false (all casters pass), and the receiver samples undefined
	// UVs. Keep the last published masks for a frame instead: no visual change
	// on a normal frame, no NaN anywhere on the odd one. Checked *before* any
	// driver state is borrowed below, so an early-out cannot leave the scene
	// with fog off, colour writes off or the mask's depth range.
	const CVector	sun = scene->getSunDirection();
	const CVector	camPos = scene->getRenderTrav().CamPos;
	if (!(sun.x == sun.x) || !(sun.y == sun.y) || !(sun.z == sun.z) ||
		sun.sqrnorm() < 1e-6f)
	{
		modernShadowDebugLog("skip", "badsun", scene, driver, NULL, 0, _Mask[0]);
		return _Active;
	}

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
	// The mask stores gl_FragCoord.z, and the driver's viewport maps NDC depth
	// through the *engine's* window depth range. The client renders the main
	// scene with [0, CANOPY_DEPTH_RANGE_START] = [0, 0.95], which would store
	// 0.95 * box depth, while the receiver compares its own true box depth
	// (the cascade matrix maps the whole box to [0, 1]). The 5 % difference is
	// 2-5 m of light-space depth: every caster then compares behind its own
	// far side and self-shadows (a character loses half its sun), and a thin
	// casting wall self-shadows as a whole, so an entity *behind* it (which
	// MAX stores as the farther depth) is painted as a lit, full silhouette.
	// The mask pass owns its depth convention, so it always writes [0, 1] and
	// restores the caller's range.
	float	bkupDepthNear = 0.f, bkupDepthFar = 1.f;
	driver->getDepthRange(bkupDepthNear, bkupDepthFar);
	driver->setDepthRange(0.f, 1.f);

	driver->enableFog(false);
	driver->setColorMask(false, false, false, true);

	// The casters are drawn with the same mega shader as everything else. Turn
	// the shadow state off for the duration of the pass: the mask textures are
	// the render targets here, and a shader must never sample the image it is
	// writing. It would also darken the casters themselves. The mask texel
	// sizes are still published because the driver stages them per draw.
	{
		CModernShadowState	disabled;
		for (uint i = 0; i < level.NumCascades; ++i)
			disabled.MaskTexelUv[i] = 1.f / (float)_Size[i];
		driver->setModernShadowState(disabled);
	}

	// One world-space bbox per caster for the whole frame. The box is the same
	// for both cascades, and computing it is the expensive part (a virtual
	// call plus the shape bounds transform), so it is done here once instead
	// of once per cascade. A caster whose box is missing (hidden in the HRC,
	// skeleton-bound skin) or degenerate (an uninitialised bone sphere was
	// seen with ~1e20 extents) is dropped once, with one debug line, instead
	// of being rejected again in each cascade.
	struct CCasterBox
	{
		CTransform	*Model;
		CAABBox		Box;
	};
	std::vector<CCasterBox>	casterBoxes;
	casterBoxes.reserve(casters.size());
	uint	badCasters = 0;
	std::string	badCasterInfo;
	for (uint c = 0; c < casters.size(); ++c)
	{
		CTransform	*model = casters[c];
		CAABBox		bb;
		if (!model || !model->computeWorldBBoxForShadow(bb))
			continue;
		if (!isValidModernShadowCasterBBox(bb))
		{
			++badCasters;
			if (badCasterInfo.empty())
			{
				const CVector	mn = bb.getMin();
				const CVector	mx = bb.getMax();
				char	extra[160];
				snprintf(extra, sizeof(extra), "bbox=(%.3g %.3g %.3g)-(%.3g %.3g %.3g)",
					mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
				badCasterInfo = extra;
			}
			continue;
		}
		CCasterBox	entry;
		entry.Model = model;
		entry.Box = bb;
		casterBoxes.push_back(entry);
	}
	if (badCasters)
	{
		char	extra[200];
		snprintf(extra, sizeof(extra), "bad=%u first %s", badCasters,
			badCasterInfo.empty() ? "" : badCasterInfo.c_str());
		modernShadowDebugLog("skip", "badcaster", scene, driver, NULL, 0, _Mask[0], extra);
	}

	_NumCascades = level.NumCascades;
	// A cascade whose level.Period is greater than one is rebuilt on every
	// Period-th frame. On a skipped frame nothing of that cascade is touched:
	// its last published matrix, split, bias and mask content stay paired,
	// which is what keeps its shadows from swimming when the camera moves
	// between two rebuilds. The phase is offset by the cascade index, so two
	// half-rate cascades do not rebuild on the same frame: the work per frame
	// stays flat instead of alternating between a heavy and a light frame.
	++_FrameCounter;
	bool	rebuildCascade[3] = { true, true, true };
	for (uint i = 0; i < _NumCascades; ++i)
		rebuildCascade[i] = level.Period[i] <= 1 || !_CascadeValid[i] ||
			((_FrameCounter + i) % level.Period[i]) == 0;
	float	biasMillimetres[3] = { 0.f, 0.f, 0.f };
	// Debug trace for cascade 0: how many casters project into the box, the
	// light-space span the range actually used and whether a caster bbox had
	// to be rejected as degenerate.
	uint	inBox0 = 0;
	float	span0 = 0.f;
	float	sunShareRaw = 0.f;
	bool	bad0 = badCasters != 0;
	// Reused per-cascade caster set (see the bbox rejection in the cascade loop).
	std::vector<CTransform*>	cascadeCasters;
	bool	maskPassOk = true;
	for (uint i = 0; i < _NumCascades; ++i)
	{
		if (!rebuildCascade[i])
			continue;
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
		worldBox.setHalfSize(CVector(level.Radius[i], level.Radius[i], level.Radius[i]));
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
		const float	texelUv = 1.f / (float)_Size[i];
		const float	minTexels = level.MinTexels[i];
		// Casters that can contribute to *this* cascade: their light-space bbox
		// must overlap the box in the image plane and in the cascade's depth
		// range, and it must be large enough to cover a readable shadow (a
		// sub-texel prop at distance cannot paint one; the coarser far cascade
		// of the balanced level asks for two texels). This is the main cost
		// saver: the full set contains every caster of the scene, most of them
		// far outside the near cascade.
		//
		// The depth range is constant (see NL_MODERN_SHADOW_DEPTH_SUN_EXTEND):
		// its sun side contains the casters that stand between the sun and the
		// box and therefore project into it, up to that constant extension; the
		// far side stops at the box, beyond which nothing can shadow it.
		const bool	exactRange = modernShadowExactRange();
		// The sun-side bound in box units: a world distance, so it does not
		// move with the cascade size or the sun elevation.
		const float	sunExtendBox = NL_MODERN_SHADOW_DEPTH_SUN_EXTEND / boxSize.y;
		const float	castDepthLo = exactRange ? 0.f : -sunExtendBox;
		const float	castDepthHi = 1.f;
		cascadeCasters.clear();
		uint	upSunCasters = 0;
		uint	beyondUpSun = 0;
		float	farthestCut = 0.f;
		float	fittedLo = 0.f, fittedHi = 1.f;
		for (uint c = 0; c < casterBoxes.size(); ++c)
		{
			CVector lo, hi;
			lightSpaceAABBox(boxToMask, casterBoxes[c].Box, lo, hi);
			if (hi.x < 0.f || lo.x > 1.f || hi.z < 0.f || lo.z > 1.f)
				continue; // outside the image plane of the box: it draws nothing here
			if (lo.y > castDepthHi)
				continue; // beyond the box away from the sun: cannot shadow it
			if (hi.y < castDepthLo)
			{
				// Up-sun of the constant bound: dropped by the range, counted
				// so the debug line shows how much the bound leaves out.
				++beyondUpSun;
				farthestCut = std::min(farthestCut, lo.y);
				continue;
			}
			if ((hi.x - lo.x) < texelUv * minTexels && (hi.z - lo.z) < texelUv * minTexels)
				continue; // smaller than one mask texel: no readable shadow
			cascadeCasters.push_back(casterBoxes[c].Model);
			// Fit the survivors, for the diagnostic line and for the opt-in
			// NLVK_SHADOW_EXACT_RANGE path. The exact path is what used to be
			// the only behaviour: the published depth scale then depended on
			// which casters happened to be in the set this frame, so a caster
			// crossing the box with the camera could change the receiver
			// tolerance of the whole cascade.
			fittedLo = std::min(fittedLo, lo.y - NL_MODERN_SHADOW_DEPTH_MARGIN);
			fittedHi = std::max(fittedHi, hi.y + NL_MODERN_SHADOW_DEPTH_MARGIN);
			if (hi.y < 0.f)
				++upSunCasters;
			if (i == 0)
				++inBox0;
		}
		float	depthLo = castDepthLo - NL_MODERN_SHADOW_DEPTH_MARGIN;
		float	depthHi = castDepthHi;
		if (exactRange)
		{
			// Pre-fix behaviour, for the A/B only: fit the range to the casters
			// of this frame, or keep the box when they all fit inside it.
			const bool	fits = fittedHi - fittedLo <= 1.f;
			depthLo = fits ? 0.f : fittedLo;
			depthHi = fits ? 1.f : fittedHi;
		}
		const float	depthSpan = depthHi - depthLo;
		if (i == 0)
			span0 = depthSpan;
		if (i == 0)
			casterClassCensus(scene, cascadeCasters);
		if (shadowDebugEnabled())
		{
			nlinfo("MODERNSHADOWCASCADE i=%u casters=%u upsun=%u cut=%u cutat=%.3f range=[%.3f %.3f] span=%.3f exact=[%.3f %.3f] span=%.3f%s",
				i, (unsigned)cascadeCasters.size(), upSunCasters, beyondUpSun, farthestCut,
				depthLo, depthHi, depthSpan, fittedLo, fittedHi, fittedHi - fittedLo,
				exactRange ? " exactrange" : "");
		}
		// The depth axis is J: put the near plane at depthLo and scale J by the
		// span. J is a column of the camera rotation, so the world offset is
		// simply jAxis * metres, and the position must be set directly
		// (translate() would scale the offset again).
		cameraMatrix.setPos(cameraMatrix.getPos() + jAxis * (depthLo * boxSize.y));
		cameraMatrix.setRot(iAxis * boxSize.x, jAxis * (boxSize.y * depthSpan), kAxis * boxSize.z);

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
		_Split[i] = level.Radius[i];
		// Receiver tolerance and soft-edge width, both in metres converted to
		// the mask's normalized light-space depth. The texel size is the one of
		// the box's image plane, which is what the bilinear filter averages.
		const float	texelMetres = std::max(boxSize.x, boxSize.z) / (float)_Size[i];
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

		// A failed target bind would leave the scene target bound, and the
		// casters would then be drawn into it as unlit white silhouettes (the
		// caster material writes depth through the mega program). Abort the
		// whole mask pass instead and keep the masks published last frame.
		if (!driver->setRenderTarget(_Mask[i], 0, 0, _Size[i], _Size[i]))
		{
			nlwarning("MODERNSHADOW: cannot bind the %ux%u mask target of cascade %u",
				_Size[i], _Size[i], i);
			maskPassOk = false;
			break;
		}
		CScissor	scissor;
		scissor.initFullScreen();
		driver->setupScissor(scissor);
		// 0 = no caster in this texel = lit (the empty value of a farthest-depth
		// mask). The caster draws blend with a MAX op, so they only ever raise
		// it with a farther depth.
		driver->setColorMask(true, true, true, true);
		driver->clear2D(CRGBA(0, 0, 0, 0));

		// The cascade camera, once for the whole cascade. Every caster used to
		// call setFrustum() and setupViewMatrix(cameraMatrix.inverted()) itself:
		// that repeated the same 4x4 inversion per draw and, worse, marked the
		// mega camera UBO dirty per draw, so bindMegaDescriptors() rebuilt it
		// (three more inversions + a 352-byte block) for every caster. The
		// casters only set their model matrix now, and the camera block is
		// staged once per cascade.
		driver->setFrustum(0, 1, 0, 1, 0, 1, false);
		driver->setupViewMatrix(casterMatrix.inverted());
		driver->setModernShadowDepthPass(true);
		for (uint c = 0; c < cascadeCasters.size(); ++c)
			cascadeCasters[c]->renderModernShadowCaster(driver, castMat);
		driver->setModernShadowDepthPass(false);

	}

	driver->setRenderTarget(previousTarget);
	driver->setColorMask(true, true, true, true);
	driver->setDepthRange(bkupDepthNear, bkupDepthFar);
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

	// A half-written set of cascades must not be published: the driver keeps
	// last frame's state, which still points at the masks that state was built
	// with. The cascade parameters go back to the ones it was built with.
	if (!maskPassOk)
	{
		for (uint i = 0; i < 3; ++i)
		{
			_WorldToMask[i] = bkupWorldToMask[i];
			_Split[i] = bkupSplit[i];
			_DepthBias[i] = bkupBias[i];
			_DepthSoftness[i] = bkupSoft[i];
			_CascadeValid[i] = bkupCascadeValid[i];
		}
		return false;
	}
	for (uint i = 0; i < _NumCascades; ++i)
	{
		if (rebuildCascade[i])
			_CascadeValid[i] = true;
	}

	// Publish the masks before the opaque pass samples them. The masks are
	// already back in SHADER_READ_ONLY layout (setRenderTarget(previous)).
	CModernShadowState	state;
	state.Enabled = true;
	state.NumCascades = _NumCascades;
	for (uint i = 0; i < _NumCascades; ++i)
		state.MaskTexelUv[i] = 1.f / (float)_Size[i];
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
			"cam=(%.1f %.1f %.1f) c0=%u span=%.4g bad=%d sunraw=%.3f sun=%.3f bias0=%.1fmm",
			camPos.x, camPos.y, camPos.z, inBox0, span0,
			bad0 ? 1 : 0, sunShareRaw, state.SunStrength, biasMillimetres[0]);
		modernShadowDebugLog("publish", "ok", scene, driver, &state,
			(uint)casters.size(), _Mask[0], extra);
	}

	// One line per quality level the modern path runs at, so a support log
	// shows which shadow implementation produced the frame. Nothing is logged
	// per frame.
	static sint loggedQuality = -1;
	if (loggedQuality != (sint)_Quality)
	{
		loggedQuality = (sint)_Quality;
		// "40+90+160 m, 2048x2048 x3", built from the level so it cannot
		// describe a different configuration than the one running.
		char	cascades[128];
		char	radii[48], sizes[64];
		radii[0] = sizes[0] = 0;
		for (uint i = 0; i < state.NumCascades; ++i)
		{
			char	item[32];
			snprintf(item, sizeof(item), "%s%.0f", i ? "+" : "", level.Radius[i]);
			strncat(radii, item, sizeof(radii) - strlen(radii) - 1);
			snprintf(item, sizeof(item), "%s%ux%u", i ? " " : "", _Size[i], _Size[i]);
			strncat(sizes, item, sizeof(sizes) - strlen(sizes) - 1);
		}
		snprintf(cascades, sizeof(cascades), "%u cascade%s %s m, %s",
			(unsigned)state.NumCascades, state.NumCascades > 1 ? "s" : "",
			radii, sizes);
		nlinfo("MODERNSHADOW: modern sun shadows on: quality %u, %s (%s masks, rebuild every %u/%u/%u frame(s)), split %.1f m, sun=(%.2f %.2f %.2f), bias %.1f mm",
			(unsigned)_Quality, cascades, _HighPrecision ? "R32F" : "RGBA8",
			level.Period[0], level.Period[1], level.Period[2],
			state.CascadeSplits[0], sun.x, sun.y, sun.z,
			(double)biasMillimetres[0]);
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
	for (uint i = 0; i < 3; ++i)
	{
		_Mask[i] = NULL;
	}
}

} // NL3D
