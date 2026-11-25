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

#ifndef NL_VEGETABLE_EXTRA_H
#define NL_VEGETABLE_EXTRA_H

#include "nel/misc/types_nl.h"
#include "nel/3d/vegetable_def.h"


namespace NL3D
{


// ***************************************************************************
/**
 * Opt-in doubled microvegetation distance (client.cfg MicroVegetExtra).
 *
 * Off: the stock layout, five 10m bands from 0 to 50m.
 * On:  the same five authored bands with twice the width, 20m each, from
 *      0 to 100m. Species lists, densities, scales, seeds and tile generation
 *      are unchanged; only the distance at which a band is displayed changes.
 *
 * The switch must be set before the first vegetable block is created.
 */
class CVegetableExtra
{
public:
	CVegetableExtra();

	bool			isEnabled() const { return _Enabled; }
	void			setEnabled(bool enabled) { _Enabled= enabled; }

	/// Squared extra-band edges: 0, 20^2, 40^2, 60^2, 80^2, 100^2.
	const float		*getSqrDistLUT() const { return _SqrDistLUT; }

private:
	bool	_Enabled;
	float	_SqrDistLUT[NL3D_VEGETABLE_BLOCK_NUMDIST + 1];
};


} // NL3D


#endif // NL_VEGETABLE_EXTRA_H

/* End of vegetable_extra.h */
