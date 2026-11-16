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

#include <cstdlib>

using namespace NLMISC;

namespace NL3D {

CModernShadowMask::CModernShadowMask(CShadowMapManager *owner)
	: _Owner(owner), _Size(2048), _Active(false), _NumCascades(0)
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

bool CModernShadowMask::render(CScene *scene, const std::vector<CTransform*> &casters)
{
	_Active = false;
	if (!isEnabled(scene))
		return false;

	// TODO: fit the cascades, render the casters as coverage masks and bind
	// them to the driver. Until that path is implemented the legacy manager
	// keeps generating the shadows.
	(void)casters;
	return false;
}

bool CModernShadowMask::apply(CScene *scene)
{
	if (!_Active)
		return false;

	(void)scene;
	return false;
}

bool CModernShadowMask::ensureMasks()
{
	return false;
}

void CModernShadowMask::releaseMasks()
{
	for (uint i = 0; i < 2; ++i)
		_Mask[i] = NULL;
}

} // NL3D
