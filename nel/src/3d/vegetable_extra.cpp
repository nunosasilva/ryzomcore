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

#include <cstdlib>


using namespace NLMISC;

#ifdef DEBUG_NEW
#define new DEBUG_NEW
#endif

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

// ***************************************************************************
float	CVegetableExtra::getDensityScale()
{
	// 0.3 is the shipped extra-mode density. The env override exists only for
	// RYZOM_VEG_VB_STATS=1 measurement runs that map the soft-VB threshold.
	static float	scale= -1.f;
	if(scale < 0.f)
	{
		scale= 0.3f;
		if(getenv("RYZOM_VEG_VB_STATS") != NULL)
		{
			const char	*env= getenv("RYZOM_VEG_EXTRA_DENSITY");
			if(env != NULL)
			{
				const float	v= (float)atof(env);
				if(v > 0.f && v <= 1.f)
					scale= v;
			}
		}
	}
	return scale;
}


} // NL3D
