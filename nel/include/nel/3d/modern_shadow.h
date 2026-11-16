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

	CModernShadowState()
		: Enabled(false), NumCascades(0)
	{
		for (uint i = 0; i < 4; ++i)
		{
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
	NLMISC::CSmartPtr<ITexture>	_Mask[2];
	NLMISC::CMatrix			_WorldToMask[2];
	float					_Split[2];
	uint					_Size;
	bool					_Active;
	uint					_NumCascades;
};

} // NL3D

#endif // NL_MODERN_SHADOW_H
