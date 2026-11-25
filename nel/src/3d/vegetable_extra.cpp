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

#include "nel/3d/vegetable_extra.h"
#include "nel/misc/common.h"


using namespace NLMISC;

namespace NL3D
{


// ***************************************************************************
CVegetableExtra::CVegetableExtra() : _Enabled(false)
{
	// Stock edges are i * NL3D_VEGETABLE_BLOCK_ELTDIST; the extra doubles the
	// band width, so the edges become i * 2 * ELTDIST.
	for(uint i= 0; i <= NL3D_VEGETABLE_BLOCK_NUMDIST; ++i)
		_SqrDistLUT[i]= sqr(2.f * NL3D_VEGETABLE_BLOCK_ELTDIST * i);
}


} // NL3D
