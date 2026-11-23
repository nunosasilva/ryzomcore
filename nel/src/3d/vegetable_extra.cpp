// NeL - MMORPG Framework <http://dev.ryzom.com/projects/nel/>
// Copyright (C) 2010  Winch Gate Property Limited
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "std3d.h"

#include "nel/3d/vegetable_extra.h"
#include "nel/3d/tile_vegetable_desc.h"
#include "nel/3d/vegetable.h"
#include "nel/misc/common.h"


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
 * (farEdge/5)m bands. Each set's generated annulus becomes (farEdge/50)^2
 * larger, so it is thinned to keep the instance count near the stock budget,
 * and the survivors are scaled up so the band's outer edge keeps a legible
 * screen size (and stays on a low enough texture mip for the alpha test).
 *
 * merged (default): one density factor for every source, and the far band has
 * its own slightly different scale. There is no per-bucket density difference,
 * so nothing thins or swaps when the view center crosses a bucket edge.
 *
 * carrier: buckets 0..3 share one factor, the far bucket has its own thinner
 * factor, and the lower-band copies use getUnionDensity(). A far-only tileset
 * (desert floor/canyon: all authored species are in the last band) keeps the
 * authored density there: thinning it would remove most of the biome's
 * vegetation.
 */
static const float	kVegExtraBandDensity= 0.28f;
static const float	kVegExtraBandScale= 1.40f;
static const float	kVegExtraFarDensity= 0.22f;
static const float	kVegExtraFarScale= 1.45f;
/*
 * Merged thinning factor. A band's annulus grows by (farEdge/50)^2 = 4 at
 * 100m, so 0.35 keeps the total instance count of the whole 0..100m disc in
 * the stock 0..50m budget while still doubling the near field density.
 * RYZOM_VEG_MERGED_DENSITY overrides it without a rebuild.
 */
static const float	kVegMergedDensity= 0.35f;


// ***************************************************************************
CVegetableExtra::CVegetableExtra() :
	_FarEdgeM(0.f),
	_Merged(true)
{
	// RYZOM_VEG_EXTRA_MODE=carrier selects the retired layout for A/B runs.
	// Read once per process; it must not change while blocks are alive.
	const char	*modeEnv= getenv("RYZOM_VEG_EXTRA_MODE");
	if(modeEnv != NULL)
		_Merged= (stricmp(modeEnv, "carrier") != 0);

	buildLUT();
}


// ***************************************************************************
bool	CVegetableExtra::setFarEdgeM(float metres)
{
	clamp(metres, 0.f, 100.f);
	// A value between 0 and the stock far edge would give bands narrower than
	// the stock 10m ones (a degenerate layout). The key used to be a boolean,
	// so treat any 0<x as "on, minimum extension" and land on the stock far
	// edge; the resolved layout is logged by CVegetableManager anyway.
	if(metres > 0.f && metres < (float)NL3D_VEGETABLE_BLOCK_NUMDIST * NL3D_VEGETABLE_BLOCK_ELTDIST)
		metres= (float)NL3D_VEGETABLE_BLOCK_NUMDIST * NL3D_VEGETABLE_BLOCK_ELTDIST;

	if(_FarEdgeM == metres)
		return false;

	_FarEdgeM= metres;
	buildLUT();
	return true;
}


// ***************************************************************************
void	CVegetableExtra::buildLUT()
{
	// Edge i is the distance after i bands: 0, b, 2b, ..., farEdge.
	// With the stock layout this class is never used, and when it is, the
	// edges are monotonically increasing, which is what the two walks of
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
float	CVegetableExtra::getBandNearDist(uint distType) const
{
	if(distType >= NL3D_VEGETABLE_BLOCK_NUMDIST)
		return _FarEdgeM;

	return getBandDist() * distType;
}


// ***************************************************************************
float	CVegetableExtra::getMergedDensity()
{
	static const float	sDensity= []()
	{
		const char	*env= getenv("RYZOM_VEG_MERGED_DENSITY");
		if(env == NULL)
			return kVegMergedDensity;
		float	v= 0.f;
		fromString(std::string(env), v);
		if(v <= 0.f)
			return kVegMergedDensity;
		clamp(v, 0.f, 1.f);
		return v;
	}();
	return sDensity;
}


// ***************************************************************************
float	CVegetableExtra::getMergedScale(uint band) const
{
	return band + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST ? kVegExtraFarScale : kVegExtraBandScale;
}


// ***************************************************************************
bool	CVegetableExtra::hasSources(const CTileVegetableDesc &tileVegetDesc, uint distType) const
{
	if(distType >= NL3D_VEGETABLE_BLOCK_NUMDIST)
		return false;

	if(_Merged)
	{
		// Bucket distType carries every band from distType to the last one,
		// so an empty authored band is not a reason to skip the bucket.
		for(uint k= distType; k < NL3D_VEGETABLE_BLOCK_NUMDIST; ++k)
		{
			if(!tileVegetDesc.getVegetableList(k).empty())
				return true;
		}
		return false;
	}

	// Carrier: only the farthest bucket can copy the lower bands.
	if(distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST)
		return true;

	return !tileVegetDesc.getVegetableList(distType).empty();
}


// ***************************************************************************
float	CVegetableExtra::getOwnDensity(const CTileVegetableDesc &tileVegetDesc, uint distType) const
{
	// Far-only tilesets (desert floor/canyon: all authored species are in the
	// last band) have nothing in the lower bands, so the global far thinning
	// would remove most of the biome's vegetation. Keep the authored density
	// there. For every other tileset the lower-band check is exact and costs
	// one list scan per generated IG.
	if(distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST)
	{
		for(uint k= 0; k < distType; ++k)
		{
			if(!tileVegetDesc.getVegetableList(k).empty())
				return kVegExtraFarDensity;
		}
		return getFarOnlyDensity();
	}

	return kVegExtraBandDensity;
}


// ***************************************************************************
float	CVegetableExtra::getOwnScale(uint distType) const
{
	return distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST ? kVegExtraFarScale : kVegExtraBandScale;
}


// ***************************************************************************
void	CVegetableExtra::buildSources(const CTileVegetableDesc &tileVegetDesc, uint distType,
	std::vector<CSource> &sources) const
{
	sources.clear();
	if(distType >= NL3D_VEGETABLE_BLOCK_NUMDIST)
		return;

	if(_Merged)
	{
		// Merged: this bucket holds bands distType..last. A band is skipped only
		// where its reach is already inside the bucket's near edge, which is
		// exactly the bands below distType: those species have alpha 0 for the
		// whole time the block sits in this bucket or farther out, and they come
		// back at their own reach edge, at alpha 0, when the camera closes in.
		// Every source keeps its authored band's seed, scale and reach, so the
		// instances of a band are bit-identical in all the buckets that hold it.
		const float	density= getMergedDensity();
		for(uint k= distType; k < NL3D_VEGETABLE_BLOCK_NUMDIST; ++k)
		{
			const std::vector<CVegetable>	&list= tileVegetDesc.getVegetableList(k);
			const uint					bandSeed= tileVegetDesc.getVegetableSeed(k);
			const float					bandScale= getMergedScale(k);
			const float					bandReach= getBandFarDist(k);
			for(uint v= 0; v < list.size(); ++v)
			{
				CSource	src;
				src.Veget= &list[v];
				src.Seed= v + bandSeed;
				src.Density= density;
				src.Scale= bandScale;
				src.Reach= bandReach;
				sources.push_back(src);
			}
		}
		return;
	}

	const bool	isFarBand= (distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST);
	const float	ownDensity= getOwnDensity(tileVegetDesc, distType);
	const float	ownScale= getOwnScale(distType);
	// Carrier: every source of the bucket fades at the bucket's own far edge.
	const float	bandReach= getBandFarDist(distType);

	// Primary sources: this band's own species list, with its own seed.
	const std::vector<CVegetable>	&ownList= tileVegetDesc.getVegetableList(distType);
	const uint						ownSeed= tileVegetDesc.getVegetableSeed(distType);
	for(uint i= 0; i < ownList.size(); ++i)
	{
		CSource	src;
		src.Veget= &ownList[i];
		src.Seed= i + ownSeed;
		src.Density= ownDensity;
		src.Scale= ownScale;
		src.Reach= bandReach;
		sources.push_back(src);
	}

	// Union copies: only the farthest bucket carries them. The copies reuse the
	// lower band's own seed and own scale, so they are duplicates of a subset
	// of that band's instances; crossing a band edge keeps the same plants and
	// only thins them from the authored density to the union density.
	if(!isFarBand)
		return;

	for(uint k= 0; k < distType; ++k)
	{
		const std::vector<CVegetable>	&lowerList= tileVegetDesc.getVegetableList(k);
		const uint						lowerSeed= tileVegetDesc.getVegetableSeed(k);
		const float	lowerScale= (k + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST) ? kVegExtraFarScale : kVegExtraBandScale;
		for(uint v= 0; v < lowerList.size(); ++v)
		{
			CSource	src;
			src.Veget= &lowerList[v];
			src.Seed= v + lowerSeed;
			src.Density= getUnionDensity();
			src.Scale= lowerScale;
			src.Reach= bandReach;
			sources.push_back(src);
		}
	}
}


} // NL3D
