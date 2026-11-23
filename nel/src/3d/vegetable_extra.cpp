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


using namespace std;
using namespace NLMISC;

#ifdef DEBUG_NEW
#define new DEBUG_NEW
#endif

namespace NL3D
{


// ***************************************************************************
/*
 * Extended-mode tuning. The five authored sets are stretched from 10m to 20m
 * bands. Each set's generated annulus becomes four times larger, so it is
 * thinned to keep the instance count in check and the survivors are scaled up
 * so the band's outer edge keeps a legible screen size (and stays on a low
 * enough texture mip for the alpha test). Buckets 0..3 share one factor; the
 * far bucket has its own thinner factor. A far-only tileset (all species in
 * the last band) keeps the authored density: thinning it would remove most of
 * the biome's vegetation.
 */
static const float	kVegExtraBandDensity= 0.28f;
static const float	kVegExtraBandScale= 1.40f;
static const float	kVegExtraFarDensity= 0.22f;
static const float	kVegExtraFarScale= 1.45f;


// ***************************************************************************
CVegetableExtra::CVegetableExtra() : _Enabled(false)
{
	// Edge i is the distance after i bands: 0, 20, 40, 60, 80, 100.
	for(uint i= 0; i <= NL3D_VEGETABLE_BLOCK_NUMDIST; ++i)
	{
		_SqrDistLUT[i]= sqr(getBandDist() * i);
	}
}


// ***************************************************************************
float	CVegetableExtra::getBandFarDist(uint distType)
{
	if(distType >= NL3D_VEGETABLE_BLOCK_NUMDIST)
		return getBandDist() * NL3D_VEGETABLE_BLOCK_NUMDIST;

	return getBandDist() * (distType + 1);
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

	const bool	isFarBand= (distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST);
	const float	ownDensity= getOwnDensity(tileVegetDesc, distType);
	const float	ownScale= getOwnScale(distType);

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
			sources.push_back(src);
		}
	}
}


} // NL3D
