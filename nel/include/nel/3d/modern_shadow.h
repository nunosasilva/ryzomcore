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

#ifndef NL_MODERN_SHADOW_H
#define NL_MODERN_SHADOW_H

#include "nel/misc/types_nl.h"
#include "nel/misc/matrix.h"
#include "nel/misc/smart_ptr.h"
#include "nel/3d/material.h"
#include "nel/3d/texture.h"

#include <cstring>
#include <vector>

namespace NL3D {

class CScene;
class CTransform;
class IDriver;

/**
 * Shadow state handed to a driver that implements the modern shadow path.
 *
 * The modern path is Vulkan-only for now: the engine builds cascaded sun
 * depth masks (and, later, point-light cube masks) and the driver samples
 * them in its lighting shader. OpenGL keeps the legacy CShadowMapManager
 * path untouched.
 */
struct CModernShadowState
{
	bool			Enabled;
	uint			NumCascades;
	/// View-space distance where each cascade ends (NumCascades entries).
	float			CascadeSplits[4];
	/// World -> mask UV matrix for each cascade (CMatrix::get() layout).
	float			WorldToMask[4][16];
	/// Depth masks, one render target per cascade.
	ITexture		*Mask[4];
	/// Receiver depth bias, in the mask's normalized light-space depth
	/// (receiverDepth > storedDepth + DepthBias => shadowed). It is the
	/// comparison tolerance in metres divided by the cascade's depth extent
	/// (see CModernShadowMask); with a high-precision mask it is a few
	/// centimetres, with the 8-bit fallback the quantization step dominates.
	float			DepthBias[4];
	/// Width of the smoothstep that turns the depth comparison into a soft
	/// edge, in the same normalized units as DepthBias.
	float			DepthSoftness[4];
	/// One mask texel in UV units (1/mask size): the shader offsets its
	/// penumbra taps by this much.
	float			MaskTexelUv;
	/// How much of the scene's light is direct sun (0..1). The shader uses it
	/// to scale the darkening so shadowed areas keep the sky/ambient part:
	/// at dusk, when the sun is dim, a full shadow would otherwise paint
	/// ambient-lit ground black.
	float			SunStrength;

	CModernShadowState()
		: Enabled(false), NumCascades(0), MaskTexelUv(0.f), SunStrength(0.f)
	{
		for (uint i = 0; i < 4; ++i)
		{
			CascadeSplits[i] = 0.f;
			Mask[i] = NULL;
			DepthBias[i] = 0.f;
			DepthSoftness[i] = 0.f;
			memset(WorldToMask[i], 0, sizeof(WorldToMask[i]));
		}
	}
};

/** Opt-in frame-level trace of the modern shadow state (NLVK_SHADOW_DEBUG=1).
 *
 * Emits one line per event that publishes (or fails to publish) the state the
 * opaque pass samples: the mask pass, the disabled publish of reset(), and the
 * skips in CShadowMapManager/RenderTrav. Each line carries the driver swap
 * counter and the scene's render/frame ids, so a 1-2 frame drop of the sun
 * term can be attributed to the pass that caused it. Off by default (one
 * getenv at startup); nothing is logged when it is unset.
 */
void	modernShadowDebugLog(const char *event, const char *reason,
	CScene *scene, IDriver *driver, const CModernShadowState *state,
	uint casters, ITexture *mask0, const char *extra = NULL);

/**
 * Cascaded sun shadow masks.
 *
 * The engine renders the visible shadow casters (the same list the legacy
 * manager receives) into a small set of cascades fitted around the camera.
 * Each mask texel stores the *nearest* caster's light-space depth; the
 * receiver compares its own light-space depth against it, with a small bias
 * (see CModernShadowState::DepthBias). A texel with no caster holds the far
 * depth (1.0), so it reads lit. The mask is sampled with linear filtering and
 * one smoothstep'd comparison (a soft edge of about one texel), so there are
 * no per-caster maps, no merge of sun/point-light directions and no hard map
 * borders to clip a low-sun shadow.
 *
 * Enablement is gated by CScene::getShadowsEnabled() (the client's Shadows
 * option) and by the driver capability: the modern path runs only when both
 * are true, so `Shadows = 0` disables all shadows and OpenGL keeps the legacy
 * CShadowMapManager path.
 */
class CModernShadowMask
{
public:
	CModernShadowMask();
	~CModernShadowMask();

	/// True when the scene wants shadows and the driver can take the modern path.
	bool					isEnabled(CScene *scene) const;

	/** Stop sampling the masks (the Shadows option was turned off): publish a
	 *	disabled state once, so the shader does not keep using the last masks.
	 */
	void					reset(CScene *scene);

	/** Render the cascades for this pass. Returns true when the modern path
	 *	handled generation and the legacy renderGenerate() must be skipped.
	 */
	bool					render(CScene *scene, const std::vector<CTransform*> &casters);

private:
	bool					ensureMasks(bool highPrecision);
	void					releaseMasks();

	/// Caster material for the depth mask: unlit, double-sided (several Ryzom
	/// shapes are single-layer shells whose only faces point away from the
	/// light). The driver turns the draw into a depth write with a MIN blend
	/// while the shadow depth pass is on.
	CMaterial				_CasterMaterial;
	NLMISC::CSmartPtr<ITexture>	_Mask[2];
	NLMISC::CMatrix			_WorldToMask[2];
	float					_Split[2];
	/// Receiver tolerance and soft-edge width, normalized by the depth extent.
	float					_DepthBias[2];
	float					_DepthSoftness[2];
	/// Published sun share and whether it has a previous value yet (the
	/// per-frame change is limited to NL_MODERN_SHADOW_SUN_SLEW).
	float					_SunStrength;
	bool					_HaveSunStrength;
	/// True when the masks are 32-bit float (driver::hasHighPrecisionShadowMask).
	bool					_HighPrecision;
	uint					_Size;
	bool					_Active;
	uint					_NumCascades;
};

} // NL3D

#endif // NL_MODERN_SHADOW_H
