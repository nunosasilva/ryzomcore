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
 * Merged density/scale factors. A tile keeps generating its authored instance
 * count per band; stretching a band only keeps those instances alive from
 * farther away, it does not spread them over more ground. So 1.0 makes the
 * close-up look identical to stock by construction, and the cost of the
 * extension is purely the larger number of tiles that stay alive. Override
 * with RYZOM_VEG_MERGED_DENSITY / RYZOM_VEG_MERGED_SCALE to thin the far field
 * without a rebuild.
 */
static const float	kVegMergedDensity= 1.0f;
static const float	kVegMergedScale= 1.0f;


// ***************************************************************************
CVegetableExtra::CVegetableExtra() :
	_FarEdgeM(0.f),
	_Merged(true),
	_MergedIntoFar(true)
{
	// RYZOM_VEG_EXTRA_MODE=carrier selects the retired layout for A/B runs.
	// Read once per process; it must not change while blocks are alive.
	const char	*modeEnv= getenv("RYZOM_VEG_EXTRA_MODE");
	if(modeEnv != NULL)
		_Merged= (stricmp(modeEnv, "carrier") != 0);

	// RYZOM_VEG_MERGED_INTO=band puts every authored band back into its own
	// bucket instead of all of them into the farthest one. Same rule as above:
	// read once, the buckets already alive would have to be rebuilt.
	const char	*intoEnv= getenv("RYZOM_VEG_MERGED_INTO");
	if(intoEnv != NULL)
		_MergedIntoFar= (stricmp(intoEnv, "band") != 0);

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
float	CVegetableExtra::getMergedScale()
{
	static const float	sScale= []()
	{
		const char	*env= getenv("RYZOM_VEG_MERGED_SCALE");
		if(env == NULL)
			return kVegMergedScale;
		float	v= 0.f;
		fromString(std::string(env), v);
		if(v <= 0.f)
			return kVegMergedScale;
		clamp(v, 0.f, 4.f);
		return v;
	}();
	return sScale;
}


// ***************************************************************************
bool	CVegetableExtra::hasSources(const CTileVegetableDesc &tileVegetDesc, uint distType) const
{
	if(distType >= NL3D_VEGETABLE_BLOCK_NUMDIST)
		return false;

	if(_Merged)
	{
		if(_MergedIntoFar)
		{
			// Everything lives in the farthest bucket; the others stay empty so
			// crossing their edge cannot change what is on screen.
			return (distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST) && !tileVegetDesc.empty();
		}
		// merged-band keeps one authored list per bucket, exactly like stock: an
		// empty band means there is nothing to generate in that bucket.
		return !tileVegetDesc.getVegetableList(distType).empty();
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

	// The constraint every layout here obeys: CLandscapeVegetableBlock keeps
	// every bucket i >= _CurDistType alive at the same time, so a species placed
	// in several buckets is drawn several times over the same ground, and each
	// bucket crossing adds or removes one of those copies at full alpha (pop-in).
	// So a species must live in exactly one bucket. What differs is which bucket,
	// and that decides where its alpha fade sits.
	// merged-far: the whole authored population of the tileset goes into the
	// farthest bucket, every source reaching the far edge. It is the only bucket
	// that is alive at every distance (update() keeps [Cur..4]), so a tile is
	// generated once, when its block crosses in past the far edge, and no bucket
	// crossing can create or delete anything any more: no fade-in ring, no band
	// vanishing, and each species is still generated exactly once. The reach is
	// the far edge for all of them, so there is a single alpha fade, at the far
	// edge, exactly where the block stops being alive.
	if(_Merged && _MergedIntoFar)
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
				// The band's own seed: the instance positions of a species do not
				// depend on the layout, so merged-far close up matches stock exactly.
				src.Seed= i + bandSeed;
				src.Density= getMergedDensity();
				src.Scale= getMergedScale();
				src.Reach= getBandFarDist(NL3D_VEGETABLE_BLOCK_NUMDIST - 1);
				src.Band= k;
				sources.push_back(src);
			}
		}
		return;
	}

	// merged-band / carrier: the bucket's own authored band, faded at that band's
	// far edge. Nothing is duplicated and nothing is generated where it would be
	// invisible, but a species is only alive from its own bucket's near edge outwards:
	// it fades in across the whole band and the whole band is deleted when the block
	// centre crosses back out past that edge.
	const bool	isFarBand= (distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST);
	const float	bandReach= getBandFarDist(distType);
	const float	density= _Merged ? getMergedDensity() : getOwnDensity(tileVegetDesc, distType);
	const float	scale= _Merged ? getMergedScale() : getOwnScale(distType);

	const std::vector<CVegetable>	&ownList= tileVegetDesc.getVegetableList(distType);
	const uint						ownSeed= tileVegetDesc.getVegetableSeed(distType);
	for(uint i= 0; i < ownList.size(); ++i)
	{
		CSource	src;
		src.Veget= &ownList[i];
		src.Seed= i + ownSeed;
		src.Density= density;
		src.Scale= scale;
		src.Reach= bandReach;
		src.Band= distType;
		sources.push_back(src);
	}

	// Carrier only: the farthest bucket additionally copies the lower bands. The
	// copies reuse the lower band's own seed and own scale, so they are duplicates
	// of a subset of that band's instances; crossing a band edge keeps the same
	// plants and only thins them from the authored density to the union density.
	// Kept for A/B against merged, not as a candidate default.
	if(_Merged || !isFarBand)
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
			src.Band= k;
			sources.push_back(src);
		}
	}
}


} // NL3D
