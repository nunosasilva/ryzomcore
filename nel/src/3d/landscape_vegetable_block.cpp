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

#include "nel/3d/landscape_vegetable_block.h"
#include "nel/3d/vegetable_manager.h"
#include "nel/3d/vegetable_clip_block.h"
#include "nel/3d/vegetable_instance_group.h"
#include "nel/3d/patch.h"
#include "nel/3d/bezier_patch.h"


using namespace std;
using namespace NLMISC;

#ifdef DEBUG_NEW
#define new DEBUG_NEW
#endif

namespace NL3D
{


// ***************************************************************************
/*
	Distances type squared.
*/
class	CLVBSqrDistLUT
{
public:
	static	float		Array[NL3D_VEGETABLE_BLOCK_NUMDIST+1];

	CLVBSqrDistLUT()
	{
		// Stock layout: 0, 10, 20, 30, 40, 50.
		for(uint i=0;i<NL3D_VEGETABLE_BLOCK_NUMDIST+1;i++)
		{
			Array[i]= i*NL3D_VEGETABLE_BLOCK_ELTDIST;
			Array[i]= sqr(Array[i]);
		}
	}

};


CLVBSqrDistLUT	NL3D_InitSqrDistLUT;
float		CLVBSqrDistLUT::Array[NL3D_VEGETABLE_BLOCK_NUMDIST+1];


// ***************************************************************************
CLandscapeVegetableBlock::CLandscapeVegetableBlock()
{
	_VegetableClipBlock = nullptr;
	_CurDistType= NL3D_VEGETABLE_BLOCK_NUMDIST;
	// No instance group created yet. init() caches the far bound of the active
	// distance layout (stock 50m or extra 100m) so isInCurrentDistanceType()
	// cannot skip the first update().
	_SqrDistMin= CLVBSqrDistLUT::Array[NL3D_VEGETABLE_BLOCK_NUMDIST];
	_SqrDistMax= FLT_MAX;

	for(uint j=0;j<NL3D_TESSBLOCK_TILESIZE;j++)
	{
		_VegetableSortBlock[j] = nullptr;
		for(uint i=0;i<NL3D_VEGETABLE_BLOCK_NUMDIST;i++)
		{
			_VegetableIG[j][i] = nullptr;
		}
	}
}



// ***************************************************************************
void			CLandscapeVegetableBlock::init(const CVector &center, CVegetableManager *vegetManager,
	CVegetableClipBlock *vegetableClipBlock, CPatch *patch, uint ts, uint tt, CTessList<CLandscapeVegetableBlock> &vblist)
{
	nlassert(patch);
	_Center= center;
	_VegetableClipBlock= vegetableClipBlock;
	_Patch= patch;
	_Ts= uint8(ts);
	_Tt= uint8(tt);

	// Create the Vegetable SortBlocks
	sint	tms,tmt;
	// for all tiles
	nlassert(NL3D_TESSBLOCK_TILESIZE==4);
	for(tmt= _Tt; tmt<_Tt+2; tmt++)
	{
		for(tms= _Ts; tms<_Ts+2; tms++)
		{
			// compute approximate center of the tile.
			float	s= (tms + 0.5f) / _Patch->getOrderS();
			float	t= (tmt + 0.5f) / _Patch->getOrderT();
			CBezierPatch	*bpatch= _Patch->unpackIntoCache();
			CVector	center= bpatch->eval(s, t);

			// create the sortBlock. NB: very approximate SortBlock radius....
			_VegetableSortBlock[(tmt-_Tt)*2 + (tms-_Ts)]= vegetManager->createSortBlock(_VegetableClipBlock, center, NL3D_PATCH_TILE_RADIUS);
		}
	}

	// No IG created yet: cache the far bound of the active distance layout so
	// isInCurrentDistanceType() cannot skip the first update().
	const CVegetableExtra	&extra= vegetManager->getExtra();
	const float	*distLUT= extra.isEnabled() ? extra.getSqrDistLUT() : CLVBSqrDistLUT::Array;
	_SqrDistMin= distLUT[NL3D_VEGETABLE_BLOCK_NUMDIST];
	_SqrDistMax= FLT_MAX;

	// append to list.
	vblist.append(this);
}


// ***************************************************************************
void			CLandscapeVegetableBlock::release(CVegetableManager *vegeManager, CTessList<CLandscapeVegetableBlock> &vblist)
{
	// State log: this is the only path that removes vegetation WITHOUT a distance
	// type change. The landscape tessellation recreates vegetable blocks when its
	// patches split or merge, so a block that was already holding instance groups
	// can be dropped and re-created at a different distance bucket - which is what
	// makes a patch of plants vanish or change species while walking, at full alpha
	// and regardless of the bucket edges. Counted under RYZOM_VEG_STATE.
	static const bool	vegStateLog= getenv("RYZOM_VEG_STATE") != NULL;
	uint	releasedIg= 0;
	for(uint j=0;j<NL3D_TESSBLOCK_TILESIZE;j++)
	{
		// release IGs first.
		for(uint i=0;i<NL3D_VEGETABLE_BLOCK_NUMDIST;i++)
		{
			if(_VegetableIG[j][i])
			{
				if(!_VegetableIG[j][i]->isEmpty())
					releasedIg++;
				vegeManager->deleteIg(_VegetableIG[j][i]);
				_VegetableIG[j][i] = nullptr;
			}
		}

		// release SB
		if(_VegetableSortBlock[j])
		{
			vegeManager->deleteSortBlock(_VegetableSortBlock[j]);
			_VegetableSortBlock[j] = nullptr;
		}
	}

	// _CurDistType is the distance bucket the block was in, which is the proxy
	// for how far the camera was: a block released with live instance groups while
	// in a near bucket is vegetation that disappears at full alpha.
	if(vegStateLog && releasedIg > 0)
		nlinfo("VEGSTATE: block (%.1f %.1f) release with %u live ig, bucket %u",
			_Center.x, _Center.y, releasedIg, (uint)_CurDistType);

	// reset state.
	_CurDistType= NL3D_VEGETABLE_BLOCK_NUMDIST;
	{
		const CVegetableExtra	&extra= vegeManager->getExtra();
		const float				*distLUT= extra.isEnabled() ? extra.getSqrDistLUT() : CLVBSqrDistLUT::Array;
		_SqrDistMin= distLUT[NL3D_VEGETABLE_BLOCK_NUMDIST];
		_SqrDistMax= FLT_MAX;
	}

	// remove from list.
	vblist.remove(this);
}

// ***************************************************************************
void			CLandscapeVegetableBlock::update(const CVector &viewCenter, CVegetableManager *vegeManager)
{
	float	sqrDist= (viewCenter-_Center).sqrnorm();

	// Stock 0..50m edges, or the opt-in 0..100m edges of CVegetableExtra.
	const CVegetableExtra	&extra= vegeManager->getExtra();
	const float	*distLUT= extra.isEnabled() ? extra.getSqrDistLUT() : CLVBSqrDistLUT::Array;

	// compute new distance type. Incremental mode.
	uint	newDistType= _CurDistType;
	while(sqrDist<distLUT[newDistType])
	{
		newDistType--;
	}
	while(newDistType<NL3D_VEGETABLE_BLOCK_NUMDIST && sqrDist>distLUT[newDistType+1])
	{
		newDistType++;
	}
	/*
		NB: to test but may be better than
			newDistType= floor()(delta.norm() / NL3D_VEGETABLE_BLOCK_ELTDIST);
	*/


	// Change of distance type??
	if(newDistType!=_CurDistType)
	{
		// Transition log for the extra path only; the regular path stays quiet.
		static	const	bool	vegStateLogEnabled= getenv("RYZOM_VEG_STATE")!=NULL;
		const	bool	vegStateLog= vegStateLogEnabled && extra.isEnabled();
		if(vegStateLog)
			nlinfo("VEGSTATE: block (%.1f %.1f) dist %.1f cur %u -> %u", _Center.x, _Center.y, sqrtf(sqrDist), _CurDistType, newDistType);

		// Erase or create IGs.
		if(newDistType>_CurDistType)
		{
			// For all tiles
			for(uint j=0;j<NL3D_TESSBLOCK_TILESIZE;j++)
			{
				// Erase no more needed Igs.
				for(uint i=_CurDistType; i<newDistType; i++)
				{
					if(_VegetableIG[j][i])
					{
						if(vegStateLog)
							nlinfo("VEGSTATE:   delete d%u tile %u", i, j);
						vegeManager->deleteIg(_VegetableIG[j][i]);
						_VegetableIG[j][i] = nullptr;
					}
				}

				// update the sort block for this tile
				_VegetableSortBlock[j]->updateSortBlock(*vegeManager);
			}
		}
		else
		{
			// Create a context for creation.
			CLandscapeVegetableBlockCreateContext	ctx;
			ctx.init(_Patch, _Ts, _Tt);

			// create new Igs, for all tiles. A bucket this layout can never fill
			// (merged-far only fills the farthest one) is skipped: no instance group
			// is allocated per tile just to be deleted again, and the state log stays
			// honest about what was actually created.
			for(uint i=newDistType; i<_CurDistType; i++)
			{
				if(!extra.hasAnySources(i))
					continue;
				if(vegStateLog)
					nlinfo("VEGSTATE:   create d%u", i);
				createVegetableIGForDistType(i, vegeManager, ctx);
			}

			// For all tiles
			for(uint j=0;j<NL3D_TESSBLOCK_TILESIZE;j++)
			{
				// update the sort block for this tile
				_VegetableSortBlock[j]->updateSortBlock(*vegeManager);
			}
		}

		// copy new dist type, and cache the bucket bounds used by
		// isInCurrentDistanceType() to skip this call while nothing can change.
		_CurDistType= uint8(newDistType);
		_SqrDistMin= distLUT[newDistType];
		_SqrDistMax= (newDistType < NL3D_VEGETABLE_BLOCK_NUMDIST)
			? distLUT[newDistType+1] : FLT_MAX;
	}


}


// ***************************************************************************
/// Name a TVegSkipReason for the state log.
static const char	*vegSkipReasonName(TVegSkipReason reason)
{
	switch(reason)
	{
		case VegSkipWaterOff:      return "vegetable disabled on this tile";
		case VegSkipNoTile:        return "no tile material loaded";
		case VegSkipNoSource:      return "no species for this bucket";
		case VegSkipUnderBuilding: return "under a building layer";
		default:                   return "generated";
	}
}


// ***************************************************************************
void			CLandscapeVegetableBlock::createVegetableIGForDistType(uint i, CVegetableManager *vegeManager,
	CLandscapeVegetableBlockCreateContext &vbCreateCtx)
{
	// check
	nlassert(NL3D_TESSBLOCK_TILESIZE==4);
	nlassert(_VegetableIG[0][i]==NULL);
	nlassert(_VegetableIG[1][i]==NULL);
	nlassert(_VegetableIG[2][i]==NULL);
	nlassert(_VegetableIG[3][i]==NULL);

	// Create vegetables instances per tile_material.
	sint	tms,tmt;
	// for all tiles
	nlassert(NL3D_TESSBLOCK_TILESIZE==4);
	for(tmt= _Tt; tmt<_Tt+2; tmt++)
	{
		for(tms= _Ts; tms<_Ts+2; tms++)
		{
			uint	tileId= tms-_Ts + (tmt-_Tt)*2;

			// create the instance group in the good sortBlock
			CVegetableInstanceGroup		*vegetIg= vegeManager->createIg(_VegetableSortBlock[tileId]);
			_VegetableIG[tileId][i]= vegetIg;

			// generate. Under RYZOM_VEG_STATE ask why an empty instance group is empty:
			// a tile whose material is not loaded yet (VegSkipNoTile) is temporary and
			// will only be retried on this block's next distance change, which the merged
			// layout makes rare - that is how a patch of ground stays bare.
			static const bool	vegStateLog= getenv("RYZOM_VEG_STATE") != NULL;
			TVegSkipReason	skipReason= VegSkipGenerated;
			_Patch->generateTileVegetable(vegetIg, i, tms, tmt, vbCreateCtx, vegStateLog ? &skipReason : NULL);

			// If the ig is empty, delete him. This optimize rendering because no useless ig are
			// tested for rendering. This speed up some 1/10 of ms...
			if(vegetIg->isEmpty())
			{
				if(vegStateLog)
				{
					static uint	emptyIgCount= 0;
					emptyIgCount++;
					// the first ones in detail, then only the running total: enough to
					// tell the causes apart without flooding the log on a long run.
					if(emptyIgCount <= 60)
						nlinfo("VEGSTATE:   ig d%u tile %u EMPTY (%s) at (%.1f %.1f) tileId %u",
							i, tileId, vegSkipReasonName(skipReason), _Center.x, _Center.y,
							_Patch->getTileIdAt(_Ts + (tileId & 1), _Tt + (tileId >> 1)));
					else if((emptyIgCount & 0xFF) == 0)
						nlinfo("VEGSTATE:   %u empty ig so far (last %s)", emptyIgCount, vegSkipReasonName(skipReason));
				}
				vegeManager->deleteIg(vegetIg);
				_VegetableIG[tileId][i] = nullptr;
			}

			// NB: vegtable SortBlock is updated in CLandscapeVegetableBlock::update() after
		}
	}

}


// ***************************************************************************
// ***************************************************************************
// CLandscapeVegetableIGCreateContext
// ***************************************************************************
// ***************************************************************************


// ***************************************************************************
CLandscapeVegetableBlockCreateContext::CLandscapeVegetableBlockCreateContext()
{
	_Patch = nullptr;
}


// ***************************************************************************
void			CLandscapeVegetableBlockCreateContext::init(CPatch *patch, uint ts, uint tt)
{
	nlassert(patch);
	_Empty= true;
	_Patch= patch;
	_Ts= ts;
	_Tt= tt;
}


// ***************************************************************************
void			CLandscapeVegetableBlockCreateContext::eval(uint ts, uint tt, float x, float y, CVector &pos)
{
	nlassert(NL3D_TESSBLOCK_TILESIZE==4);

	// If never created, do it now
	// ==================
	if(_Empty)
	{
		// Eval Position and normals for the 9 vertices (around the 2x2 tiles)
		for(uint j=0; j<3;j++)
		{
			float	t= (float)(_Tt+j)/_Patch->getOrderT();
			for(uint i=0; i<3;i++)
			{
				float	s= (float)(_Ts+i)/_Patch->getOrderS();
				// eval position.
				// use computeVertex() and not bpatch->eval() because vegetables must follow the
				// noise (at least at tile precision...). It is slower but necessary.
				_Pos[j*3+i]= _Patch->computeVertex(s, t);
			}
		}

		// Ok, it's done
		_Empty= false;
	}


	// Eval, with simple bilinear
	// ==================
	nlassert(ts==_Ts || ts==_Ts+1);
	nlassert(tt==_Tt || tt==_Tt+1);
	uint	ds= ts-_Ts;
	uint	dt= tt-_Tt;
	// Indices.
	uint	v00= dt*3 + ds;
	uint	v10= v00 + 1;
	uint	v01= v00 + 3;
	uint	v11= v00 + 4;
	// BiLinearFactor.
	float	dxdy= (1-x)*(1-y);
	float	dx2dy= x*(1-y);
	float	dxdy2= (1-x)*y;
	float	dx2dy2= x*y;

	// Compute Pos.
	pos = _Pos[v00] * dxdy;
	pos+= _Pos[v10] * dx2dy;
	pos+= _Pos[v01] * dxdy2;
	pos+= _Pos[v11] * dx2dy2;
}


} // NL3D
