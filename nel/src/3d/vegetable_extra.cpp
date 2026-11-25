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
 * Union2 tuning (the layout MicroVegetExtra=1 selects). The five authored sets
 * are stretched from 10m to (farEdge/5)m bands and each set stays in its own
 * bucket, so its reach is its own band's far edge: 20/40/60/80/100m. The near
 * bands keep the short reach they were authored for (the tall grass at the feet
 * must not be generated from every tile out to 100m). The farthest bucket also
 * carries a thinned copy of every lower band (getUnionDensity) with the lower
 * band's own seed and scale, so a species keeps existing across its band edge
 * without swapping plants or adding an alpha step.
 *
 * The densities/scales are union2's, not authored 1.0: each band's own list is
 * thinned to keep the larger stretched annulus near the stock instance budget;
 * far-only tilesets (desert floor/canyon: every species is in the last band)
 * keep the authored density, or they would lose most of the biome.
 */
/*
 * Merged-far measurement tuning. Not selected by MicroVegetExtra; only by
 * RYZOM_VEG_EXTRA_LAYOUT=merged-far. Every authored band goes into the farthest
 * bucket at this single density/scale.
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
	_Layout(Union2Layout),
	_Enabled(false),
	_FarEdgeM(resolveFarEdge())
{
	// The merged-far layout is kept only as an A/B measurement knob; the switch
	// in client.cfg always means union2. Read once per process, like the far edge.
	const char	*layoutEnv= getenv("RYZOM_VEG_EXTRA_LAYOUT");
	if(layoutEnv != NULL && stricmp(layoutEnv, "merged-far") == 0)
		_Layout= MergedFarLayout;

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
float	CVegetableExtra::getBandFarDist(uint distType) const
{
	if(distType >= NL3D_VEGETABLE_BLOCK_NUMDIST)
		return _FarEdgeM;

	return getBandDist() * (distType + 1);
}


// ***************************************************************************
float	CVegetableExtra::getMergedDensity()
{
	// Read once: changing it while blocks are alive would leave the alive
	// instance groups at the old density with no way to tell. Measurement only.
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
	// Stock and union2 can put something in every bucket. Only the merged-far
	// measurement layout fills the farthest bucket alone.
	if(!_Enabled || _Layout == Union2Layout)
		return true;
	return distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST;
}


// ***************************************************************************
bool	CVegetableExtra::hasSources(const CTileVegetableDesc &tileVegetDesc, uint distType) const
{
	if(distType >= NL3D_VEGETABLE_BLOCK_NUMDIST)
		return false;

	if(_Layout == MergedFarLayout)
	{
		// Everything lives in the farthest bucket; the others stay empty so
		// crossing their edge cannot change what is on screen.
		return distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST && !tileVegetDesc.empty();
	}

	// Union2: the bucket's own authored list. The farthest bucket can still
	// carry the lower bands when the tileset is far-only or mixed.
	if(!tileVegetDesc.getVegetableList(distType).empty())
		return true;
	if(distType + 1 != NL3D_VEGETABLE_BLOCK_NUMDIST)
		return false;
	for(uint k= 0; k < distType; ++k)
	{
		if(!tileVegetDesc.getVegetableList(k).empty())
			return true;
	}
	return false;
}


// ***************************************************************************
float	CVegetableExtra::getOwnDensity(const CTileVegetableDesc &tileVegetDesc, uint distType) const
{
	if(distType >= NL3D_VEGETABLE_BLOCK_NUMDIST)
		return getFarOnlyDensity();

	if(distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST)
	{
		// Far-only tilesets (desert floor/canyon: all authored species are in the
		// last band) have nothing in the lower bands, so the global far thinning
		// would remove most of the biome's vegetation. Keep the authored density
		// there; the lower-band check is exact and costs one list scan per IG.
		for(uint k= 0; k < distType; ++k)
		{
			if(!tileVegetDesc.getVegetableList(k).empty())
				return getFarDensity();
		}
		return getFarOnlyDensity();
	}

	return getBandDensity();
}


// ***************************************************************************
float	CVegetableExtra::getOwnScale(uint distType) const
{
	return distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST ? getFarScale() : getBandScale();
}


// ***************************************************************************
void	CVegetableExtra::buildSources(const CTileVegetableDesc &tileVegetDesc, uint distType,
	std::vector<CSource> &sources) const
{
	sources.clear();
	if(!_Enabled || distType >= NL3D_VEGETABLE_BLOCK_NUMDIST)
		return;

	if(_Layout == MergedFarLayout)
	{
		if(distType + 1 != NL3D_VEGETABLE_BLOCK_NUMDIST)
			return;
		for(uint k= 0; k < NL3D_VEGETABLE_BLOCK_NUMDIST; ++k)
		{
			const std::vector<CVegetable>	&bandList= tileVegetDesc.getVegetableList(k);
			const uint						bandSeed= tileVegetDesc.getVegetableSeed(k);
			for(uint i= 0; i < bandList.size(); ++i)
			{
				CSource	src;
				src.Veget= &bandList[i];
				src.Seed= i + bandSeed;
				src.Density= getMergedDensity();
				src.Scale= getMergedScale();
				src.Reach= _FarEdgeM;
				src.Band= k;
				sources.push_back(src);
			}
		}
		return;
	}

	// Union2: this bucket's own authored band, reaching its own band's far edge
	// (20/40/60/80/100m). The near bands keep the short reach they were authored
	// for: extending them to 100m made the tall grass vanish at tile boundaries.
	const std::vector<CVegetable>	&ownList= tileVegetDesc.getVegetableList(distType);
	const uint						ownSeed= tileVegetDesc.getVegetableSeed(distType);
	const float						ownDensity= getOwnDensity(tileVegetDesc, distType);
	const float						ownScale= getOwnScale(distType);
	const float						bandReach= getBandFarDist(distType);
	for(uint i= 0; i < ownList.size(); ++i)
	{
		CSource	src;
		src.Veget= &ownList[i];
		src.Seed= i + ownSeed;
		src.Density= ownDensity;
		src.Scale= ownScale;
		src.Reach= bandReach;
		src.Band= distType;
		sources.push_back(src);
	}

	// Farthest bucket only: the lower bands' species at the union density, with
	// their own seed and scale (exact duplicates of a subset of their instances,
	// so a band edge thins the plants instead of swapping them).
	if(distType + 1 != NL3D_VEGETABLE_BLOCK_NUMDIST)
		return;

	for(uint k= 0; k < distType; ++k)
	{
		const std::vector<CVegetable>	&lowerList= tileVegetDesc.getVegetableList(k);
		const uint						lowerSeed= tileVegetDesc.getVegetableSeed(k);
		const float						lowerScale= getOwnScale(k);
		for(uint v= 0; v < lowerList.size(); ++v)
		{
			CSource	src;
			src.Veget= &lowerList[v];
			src.Seed= v + lowerSeed;
			src.Density= getUnionDensity();
			src.Scale= lowerScale;
			src.Reach= _FarEdgeM;
			src.Band= k;
			sources.push_back(src);
		}
	}
}


} // NL3D
