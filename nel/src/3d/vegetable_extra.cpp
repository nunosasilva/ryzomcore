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
#include "nel/3d/tile_vegetable_desc.h"
#include "nel/3d/vegetable.h"
#include "nel/misc/common.h"
#include "nel/misc/string_conversion.h"


using namespace std;
using namespace NLMISC;

#ifdef DEBUG_NEW
#define new DEBUG_NEW
#endif

namespace NL3D
{


// ***************************************************************************
/*
 * Extended-mode tuning. The five authored sets are stretched from 10m to
 * (farEdge/5)m bands, and every set is generated into the farthest bucket, so
 * a tile keeps the authored instance count and scale of each band (1.0): the
 * close-up look is stock by construction, and the cost of the extension is the
 * larger number of tiles that stay alive, not more instances per tile.
 *
 * The overrides are for measurement runs (density 0.5 buys frame rate on
 * setups where the far field overflows the hard vegetation VB); they are read
 * once per process, like the far edge itself.
 */
static const float	kVegMergedDensity= 1.0f;
static const float	kVegMergedScale= 1.0f;


// ***************************************************************************
/// Read a positive float from an environment variable, with a default and a
/// clamp. Returns 'def' when the variable is unset, empty or not a number.
static float	readEnvFloat(const char *name, float def, float maxValue)
{
	const char	*env= getenv(name);
	if(env == NULL)
		return def;

	float	value= 0.f;
	if(!fromString(std::string(env), value) || value <= 0.f)
	{
		nlwarning("vegetable: %s='%s' is not a positive number, ignored", name, env);
		return def;
	}
	clamp(value, 0.f, maxValue);
	return value;
}


// ***************************************************************************
/// Far edge of the extended layout. A constant of NeL; RYZOM_VEG_FAR_EDGE_M
/// replaces it for measurement runs (values outside [stock far edge, 100] are
/// clamped, and it never narrows below the five stock 10m bands).
static float	resolveFarEdge()
{
	float	farEdge= NL3D_VEGETABLE_BLOCK_EXTRA_FAR_DIST;

	const char	*env= getenv("RYZOM_VEG_FAR_EDGE_M");
	if(env == NULL)
		return farEdge;

	const float	stockFarEdge= (float)NL3D_VEGETABLE_BLOCK_NUMDIST * NL3D_VEGETABLE_BLOCK_ELTDIST;
	float		requested= 0.f;
	if(!fromString(std::string(env), requested) || requested <= 0.f)
	{
		nlwarning("vegetable: RYZOM_VEG_FAR_EDGE_M='%s' is not a positive number, using %.0f m",
			env, farEdge);
		return farEdge;
	}
	clamp(requested, stockFarEdge, NL3D_VEGETABLE_BLOCK_EXTRA_FAR_DIST);
	if(requested != (float)atof(env))
		nlinfo("vegetable: RYZOM_VEG_FAR_EDGE_M='%s' clamped to %.0f m", env, requested);
	return requested;
}


// ***************************************************************************
CVegetableExtra::CVegetableExtra() :
	_Enabled(false),
	_FarEdgeM(resolveFarEdge())
{
	buildLUT();
}


// ***************************************************************************
bool	CVegetableExtra::setEnabled(bool enabled)
{
	if(_Enabled == enabled)
		return false;

	_Enabled= enabled;
	return true;
}


// ***************************************************************************
void	CVegetableExtra::buildLUT()
{
	// Edge i is the distance after i buckets: 0, b, 2b, ..., farEdge.
	// The edges are monotonically increasing, which is what the two walks of
	// CLandscapeVegetableBlock::update() rely on.
	for(uint i= 0; i <= NL3D_VEGETABLE_BLOCK_NUMDIST; ++i)
	{
		_SqrDistLUT[i]= sqr(getBandDist() * i);
	}
}


// ***************************************************************************
float	CVegetableExtra::getMergedDensity()
{
	// Read once: changing it while blocks are alive would leave the alive
	// instance groups at the old density with no way to tell. This is a
	// measurement knob, not a user setting.
	static const float	sDensity= readEnvFloat("RYZOM_VEG_MERGED_DENSITY", kVegMergedDensity, 1.f);
	return sDensity;
}


// ***************************************************************************
float	CVegetableExtra::getMergedScale()
{
	static const float	sScale= readEnvFloat("RYZOM_VEG_MERGED_SCALE", kVegMergedScale, 4.f);
	return sScale;
}


// ***************************************************************************
bool	CVegetableExtra::hasAnySources(uint distType) const
{
	// Stock: every bucket can hold its authored list. Extended: only the
	// farthest bucket carries anything.
	if(!_Enabled)
		return true;
	return distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST;
}


// ***************************************************************************
bool	CVegetableExtra::hasSources(const CTileVegetableDesc &tileVegetDesc, uint distType) const
{
	if(distType >= NL3D_VEGETABLE_BLOCK_NUMDIST)
		return false;

	// Everything lives in the farthest bucket; the others stay empty so crossing
	// their edge cannot change what is on screen. A tileset with all five lists
	// empty still has nothing to generate.
	return distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST && !tileVegetDesc.empty();
}


// ***************************************************************************
void	CVegetableExtra::buildSources(const CTileVegetableDesc &tileVegetDesc, uint distType,
	std::vector<CSource> &sources) const
{
	sources.clear();
	if(!_Enabled || distType >= NL3D_VEGETABLE_BLOCK_NUMDIST)
		return;

	// The constraint every layout here obeys: CLandscapeVegetableBlock keeps
	// every bucket i >= _CurDistType alive at the same time, so a species placed
	// in several buckets is drawn several times over the same ground, and each
	// bucket crossing adds or removes one of those copies at full alpha (pop-in).
	// So a species must live in exactly one bucket, and its reach must be that
	// bucket's far edge. Here every authored list goes into the farthest bucket,
	// which is the only bucket alive at every distance; a tile is therefore
	// generated once, when its block crosses in past the far edge, and no bucket
	// crossing can create or delete anything any more. The reach is the far edge
	// for all of them, so there is a single alpha fade, at the far edge, exactly
	// where the block stops being alive.
	if(distType + 1 != NL3D_VEGETABLE_BLOCK_NUMDIST)
		return;

	const uint	numBands= NL3D_VEGETABLE_BLOCK_NUMDIST;
	for(uint k= 0; k < numBands; ++k)
	{
		const std::vector<CVegetable>	&bandList= tileVegetDesc.getVegetableList(k);
		const uint						bandSeed= tileVegetDesc.getVegetableSeed(k);
		for(uint i= 0; i < bandList.size(); ++i)
		{
			CSource	src;
			src.Veget= &bandList[i];
			// The band's own seed: the instance positions of a species do not
			// depend on the layout, so the close-up matches stock exactly.
			src.Seed= i + bandSeed;
			src.Density= getMergedDensity();
			src.Scale= getMergedScale();
			src.Reach= _FarEdgeM;
			src.Band= k;
			sources.push_back(src);
		}
	}
}


} // NL3D
