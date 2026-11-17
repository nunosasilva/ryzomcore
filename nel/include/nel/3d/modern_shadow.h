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
class CShadowMapManager;

/**
 * Shadow state handed to a driver that implements the modern shadow path.
 *
 * The modern path is Vulkan-only for now: the engine builds cascaded sun
 * coverage masks (and, later, point-light cube masks) and the driver samples
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
	/// Coverage masks, one render target per cascade.
	ITexture		*Mask[4];
	/// World size of a mask texel (for PCF), per cascade.
	float			TexelWorldSize[4];
	/// Development: [0]/[1] are filled by the driver with 1/width, 1/height of
	/// the current target; [2]/[3] carry a mask UV computed on the CPU (see
	/// CModernShadowMask::render) for the marker debug view.
	float			Screen[4];
	/// How much of the scene's light is direct sun (0..1). The shader uses it
	/// to scale the darkening so shadowed areas keep the sky/ambient part:
	/// at dusk, when the sun is dim, a full shadow would otherwise paint
	/// ambient-lit ground black.
	float			SunStrength;
	/// Development views, toggled at runtime by creating files (see
	/// CModernShadowMask::render): 1 paints the sun coverage (black = lit,
	/// white = shadowed), 2 paints the cascade block read by the shader.
	uint			DebugMode;
	/// Development probe: mirror the mask lookup along U, to find the mask's
	/// handedness on this driver.
	bool			FlipU;
	/// Mask row 0 is the top of the light image while the cascade matrix maps
	/// the bottom of the light box to v = 0, so the lookup flips V. Kept as a
	/// runtime toggle for the development A/B.
	bool			FlipV;

	CModernShadowState()
		: Enabled(false), NumCascades(0), SunStrength(0.f), DebugMode(0), FlipU(false), FlipV(true)
	{
		for (uint i = 0; i < 4; ++i)
		{
			Screen[i] = 0.f;
			CascadeSplits[i] = 0.f;
			Mask[i] = NULL;
			TexelWorldSize[i] = 0.f;
			memset(WorldToMask[i], 0, sizeof(WorldToMask[i]));
		}
	}
};

/**
 * Cascaded sun shadow masks.
 *
 * The engine renders the visible shadow casters (the same list the legacy
 * manager receives) as coverage into a small set of cascades fitted around
 * the camera. The receiver shader samples the coverage with PCF, so there
 * are no per-caster maps, no merge of sun/point-light directions and no
 * hard map borders to clip a low-sun shadow.
 *
 * Enablement is gated by CScene::getShadowsEnabled() (the client's Shadows
 * option) and by the driver capability. NLVK_MODERN_SHADOWS is a
 * development-only opt-in while the Vulkan shader path is being built; it
 * must be removed before merge.
 */
class CModernShadowMask
{
public:
	CModernShadowMask(CShadowMapManager *owner);
	~CModernShadowMask();

	/// True when the scene wants shadows and the driver can take the modern path.
	bool					isEnabled(CScene *scene) const;

	/** Render the cascades for this pass. Returns true when the modern path
	 *	handled generation and the legacy renderGenerate() must be skipped.
	 */
	bool					render(CScene *scene, const std::vector<CTransform*> &casters);

	/** Publish the cascade state to the driver. Returns true when the modern
	 *	path handled projection and renderProject() must be skipped.
	 */
	bool					apply(CScene *scene);

private:
	bool					ensureMasks();
	void					releaseMasks();

	CShadowMapManager		*_Owner;
	/// Single-sided coverage material: culling front faces keeps the far
	/// silhouettes of buildings/characters and drops up-facing ground planes
	/// (the village shape contains ground geometry that would otherwise cover
	/// the whole mask).
	CMaterial				_CasterMaterial;
	NLMISC::CSmartPtr<ITexture>	_Mask[2];
	NLMISC::CMatrix			_WorldToMask[2];
	float					_Split[2];
	uint					_Size;
	bool					_Active;
	uint					_NumCascades;
};

/// Development counter: triangles submitted to the modern sun masks since it
/// was last reset. CModernShadowMask::render() resets it once per pass.
extern uint32			NLModernShadowCasterTris;
extern uint32			NLModernShadowCasterMeshes;

} // NL3D

#endif // NL_MODERN_SHADOW_H
