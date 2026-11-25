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
 * Far-only extension (the layout MicroVegetExtra=1 selects).
 *
 * Bands 0..2 are the near bands (the tall grass): they keep their STOCK reach
 * (10/20/30m) and their authored density/scale (1.0). They are not stretched
 * and not copied to the far field, so the near field is bit-for-bit stock.
 *
 * Bands 3 and 4 are the far bands: they are generated into the farthest
 * bucket and reach the far edge (100m), at authored density/scale (1.0).
 * There is no fade, crossfade or gated carrier.
 *
 * The distance LUT is non-uniform: 0/10/20/30 for the near bands, then the
 * farthest bucket covers 30..100m and holds both far bands. Bucket 3
 * (30..60m) is never filled; update() skips it through hasAnySources().
 */

/*
 * Union2 measurement layout tuning, selected only with
 * RYZOM_VEG_EXTRA_LAYOUT=union2.
 */
static const float	kVegBandDensity= 0.28f;
static const float	kVegBandScale= 1.40f;
static const float	kVegFarDensity= 0.22f;
static const float	kVegFarScale= 1.45f;

/*
 * Merged-far measurement layout tuning, selected only with
 * RYZOM_VEG_EXTRA_LAYOUT=merged-far.
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
/// Far edge of the measurement layouts. A constant of NeL; RYZOM_VEG_FAR_EDGE_M
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
	_Layout(FarOnlyLayout),
	_Enabled(false),
	_FarEdgeM(NL3D_VEGETABLE_BLOCK_EXTRA_FAR_DIST)
{
	// The default is far-only. union2/merged-far are measurement layouts only;
	// the switch in client.cfg always selects far-only. Read once per process.
	const char	*layoutEnv= getenv("RYZOM_VEG_EXTRA_LAYOUT");
	if(layoutEnv != NULL)
	{
		if(stricmp(layoutEnv, "union2") == 0)
			_Layout= Union2Layout;
		else if(stricmp(layoutEnv, "merged-far") == 0)
			_Layout= MergedFarLayout;
		else if(stricmp(layoutEnv, "far-only") != 0)
			nlwarning("vegetable: unknown RYZOM_VEG_EXTRA_LAYOUT='%s', using far-only",
				layoutEnv);
	}

	// The far-only LUT is non-uniform (10/20/30/60/100); RYZOM_VEG_FAR_EDGE_M is
	// only meaningful for the uniform measurement layouts.
	if(_Layout != FarOnlyLayout)
		_FarEdgeM= resolveFarEdge();

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
const char	*CVegetableExtra::getLayoutName() const
{
	switch(_Layout)
	{
		case Union2Layout:   return "union2";
		case MergedFarLayout:return "merged-far";
		case FarOnlyLayout:
		default:             return "far-only";
	}
}


// ***************************************************************************
void	CVegetableExtra::buildLUT()
{
	// Far-only: near bands keep the stock 10m edges, the farthest bucket holds
	// bands 3+4 from 30m to the far edge. The other layouts are uniform.
	if(_Layout == FarOnlyLayout)
	{
		const float	edges[NL3D_VEGETABLE_BLOCK_NUMDIST + 1]= { 0.f, 10.f, 20.f, 30.f, 60.f, 100.f };
		for(uint i= 0; i <= NL3D_VEGETABLE_BLOCK_NUMDIST; ++i)
			_SqrDistLUT[i]= sqr(edges[i]);
		return;
	}

	// Edge i is the distance after i buckets: 0, b, 2b, ..., farEdge.
	for(uint i= 0; i <= NL3D_VEGETABLE_BLOCK_NUMDIST; ++i)
	{
		_SqrDistLUT[i]= sqr(getBandDist() * i);
	}
}


// ***************************************************************************
float	CVegetableExtra::getBandFarDist(uint distType) const
{
	if(_Layout == FarOnlyLayout)
	{
		const float	edges[NL3D_VEGETABLE_BLOCK_NUMDIST]= { 10.f, 20.f, 30.f, 60.f, 100.f };
		return distType < NL3D_VEGETABLE_BLOCK_NUMDIST ? edges[distType] : 100.f;
	}

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
	if(!_Enabled)
		return true;

	if(_Layout == FarOnlyLayout)
	{
		// Buckets 0..2 are the near bands, bucket 4 holds bands 3+4; bucket 3
		// (the 30..60m slot) is never filled.
		return distType != 3;
	}

	// Union2 can put something in every bucket; merged-far only in the farthest.
	if(_Layout == Union2Layout)
		return true;
	return distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST;
}


// ***************************************************************************
bool	CVegetableExtra::hasSources(const CTileVegetableDesc &tileVegetDesc, uint distType) const
{
	if(distType >= NL3D_VEGETABLE_BLOCK_NUMDIST)
		return false;

	if(_Layout == FarOnlyLayout)
	{
		if(distType == 3)
			return false;
		if(distType == 4)
		{
			return !tileVegetDesc.getVegetableList(3).empty()
				|| !tileVegetDesc.getVegetableList(4).empty();
		}
		return !tileVegetDesc.getVegetableList(distType).empty();
	}

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
	if(_Layout == FarOnlyLayout)
		return 1.f;
	if(_Layout == MergedFarLayout)
		return getMergedDensity();

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
	if(_Layout == FarOnlyLayout)
		return 1.f;
	if(_Layout == MergedFarLayout)
		return getMergedScale();
	return distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST ? getFarScale() : getBandScale();
}


// ***************************************************************************
void	CVegetableExtra::buildSources(const CTileVegetableDesc &tileVegetDesc, uint distType,
	std::vector<CSource> &sources) const
{
	sources.clear();
	if(!_Enabled || distType >= NL3D_VEGETABLE_BLOCK_NUMDIST)
		return;

	// Far-only: the three near bands in their own buckets with the stock reach
	// and authored density/scale; both far bands in the farthest bucket at the
	// far edge. No copy, no fade.
	if(_Layout == FarOnlyLayout)
	{
		if(distType == 3)
			return;

		if(distType == 4)
		{
			for(uint k= 3; k < NL3D_VEGETABLE_BLOCK_NUMDIST; ++k)
			{
				const std::vector<CVegetable>	&bandList= tileVegetDesc.getVegetableList(k);
				const uint						bandSeed= tileVegetDesc.getVegetableSeed(k);
				for(uint i= 0; i < bandList.size(); ++i)
				{
					CSource	src;
					src.Veget= &bandList[i];
					src.Seed= i + bandSeed;
					src.Density= 1.f;
					src.Scale= 1.f;
					src.Reach= _FarEdgeM;
					src.Band= k;
					sources.push_back(src);
				}
			}
			return;
		}

		const std::vector<CVegetable>	&ownList= tileVegetDesc.getVegetableList(distType);
		const uint						ownSeed= tileVegetDesc.getVegetableSeed(distType);
		const float						reach= getBandFarDist(distType);
		for(uint i= 0; i < ownList.size(); ++i)
		{
			CSource	src;
			src.Veget= &ownList[i];
			src.Seed= i + ownSeed;
			src.Density= 1.f;
			src.Scale= 1.f;
			src.Reach= reach;
			src.Band= distType;
			sources.push_back(src);
		}
		return;
	}

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

	// Union2 measurement layout: this bucket's own authored band, reaching its
	// own band's far edge (20/40/60/80/100m), plus the lower-band copies in the
	// farthest bucket at the union density.
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
