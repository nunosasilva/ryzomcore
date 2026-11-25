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
#include "nel/3d/landscape.h"
#include "nel/3d/tile_vegetable_desc.h"
#include "nel/3d/bezier_patch.h"
#include "nel/misc/time_nl.h"


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
	_Dirty= false;
	_Stale= false;
	_NearRetryPending= false;
	_NearRetryTime= 0;
	_NearRetryCount= 0;
	// No instance group created yet. init() caches the far bound of the active
	// distance layout (stock 50m or extra 100m) so needsUpdate() cannot skip the
	// first update().
	_SqrDistMin= CLVBSqrDistLUT::Array[NL3D_VEGETABLE_BLOCK_NUMDIST];
	_SqrDistMax= FLT_MAX;

	for(uint j=0;j<NL3D_TESSBLOCK_TILESIZE;j++)
	{
		_VegetableSortBlock[j] = nullptr;
		_TileKey[j] = ~(uint64)0;
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
	// needsUpdate() cannot skip the first update(). The tile keys are seeded from
	// the patch, so a block created out of range does not report a false change
	// on its first dirty refresh (no instance group was generated yet).
	const CVegetableExtra	&extra= vegetManager->getExtra();
	const float	*distLUT= extra.isEnabled() ? extra.getSqrDistLUT() : CLVBSqrDistLUT::Array;
	_SqrDistMin= distLUT[NL3D_VEGETABLE_BLOCK_NUMDIST];
	_SqrDistMax= FLT_MAX;
	_Dirty= false;
	_Stale= false;
	_NearRetryPending= false;
	_NearRetryTime= 0;
	_NearRetryCount= 0;
	for(uint j=0;j<NL3D_TESSBLOCK_TILESIZE;j++)
	{
		_TileKey[j]= patch->getTileVegetableKey(_Ts + (j & 1), _Tt + (j >> 1));
	}

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
	_Dirty= false;
	_Stale= false;
	_NearRetryPending= false;
	_NearRetryTime= 0;
	_NearRetryCount= 0;
	for(uint j=0;j<NL3D_TESSBLOCK_TILESIZE;j++)
		_TileKey[j]= ~(uint64)0;
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

	// Transition / refresh log, opt-in through RYZOM_VEG_STATE=1. Kept for the
	// stock layout too: the block lifecycle is what explains a bare patch.
	static	const	bool	vegStateLog= getenv("RYZOM_VEG_STATE")!=NULL;
	const	bool			bucketChanged= newDistType != _CurDistType;
	if(vegStateLog)
	{
		if(bucketChanged)
			nlinfo("VEGSTATE: block (%.1f %.1f) dist %.1f cur %u -> %u", _Center.x, _Center.y, sqrtf(sqrDist), _CurDistType, newDistType);
	}

	// Erase the buckets that leave the alive range first, so a dirty refresh
	// never regenerates an instance group that is about to be destroyed.
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
		}

		// The block is fully out of range: its old field is gone. Re-seed the
		// keys from the current tiles so a later re-entry generates the new
		// region instead of keeping a stale state forever.
		if(_Stale && newDistType == NL3D_VEGETABLE_BLOCK_NUMDIST)
		{
			for(uint j=0;j<NL3D_TESSBLOCK_TILESIZE;j++)
				_TileKey[j]= _Patch->getTileVegetableKey(_Ts + (j & 1), _Tt + (j >> 1));
			_Stale= false;
		}
	}

	// The block's tile materials changed since the instance groups were generated
	// (a tile of the 2x2 tessblock arrived late, was replaced, or a zone/region
	// transition re-tessellated the border). Regenerate only the tiles whose data
	// changed, in place, at the buckets that are alive at this distance: the
	// visible field outside this block is not touched, and if the tile data is
	// identical nothing is regenerated at all.
	const uint	aliveFloor= newDistType > _CurDistType ? newDistType : _CurDistType;
	const bool	refreshed= _Dirty;
	uint		refreshedIg= 0;
	if(refreshed)
		refreshedIg= refreshVegetableIgs(aliveFloor, vegeManager);
	_Dirty= false;

	// A near IG came out empty at generation time (the login world-load can do
	// this before the patch/tile data is fully usable). Retry the null alive
	// near buckets after a short delay, without waiting for a tile event or a
	// MicroVeget toggle. The retry is limited to d0..d2: the far d4 empties are
	// expected at the far edge and regenerating thousands of them would be pure
	// cost. createVegetableIGForTile() schedules the next attempt if it is still
	// empty, up to a small cap.
	uint		retryIg= 0;
	if(_NearRetryPending && NLMISC::CTime::getLocalTime() >= _NearRetryTime)
	{
		_NearRetryPending= false;
		if(vegStateLog)
			nlinfo("VEGSTATE:   near retry (first bucket %u)", aliveFloor);
		retryIg= refreshVegetableIgs(aliveFloor, vegeManager, 3);
	}

	if(newDistType<_CurDistType)
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
	}

	// A bucket transition and/or a refresh changed what the sort blocks hold.
	// The instance groups themselves are already in the sort blocks' lists; this
	// recomputes the sort block bounds and ZSort sides.
	if(bucketChanged || (refreshed && refreshedIg > 0) || retryIg > 0)
	{
		for(uint j=0;j<NL3D_TESSBLOCK_TILESIZE;j++)
		{
			// update the sort block for this tile
			_VegetableSortBlock[j]->updateSortBlock(*vegeManager);
		}
	}

	if(bucketChanged)
	{
		// copy new dist type, and cache the bucket bounds used by
		// needsUpdate() to skip this call while nothing can change.
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
		case VegSkipNoTile:        return "no tile data";
		case VegSkipNoSource:      return "no species for this bucket";
		case VegSkipUnderBuilding: return "under a building layer";
		default:                   return "generated";
	}
}


// ***************************************************************************
void			CLandscapeVegetableBlock::createVegetableIGForTile(uint tileId, uint distType,
	CVegetableManager *vegeManager, CLandscapeVegetableBlockCreateContext &vbCreateCtx)
{
	nlassert(tileId < NL3D_TESSBLOCK_TILESIZE);
	nlassert(distType < NL3D_VEGETABLE_BLOCK_NUMDIST);
	nlassert(_VegetableIG[tileId][distType]==NULL);

	uint	tms= _Ts + (tileId & 1);
	uint	tmt= _Tt + (tileId >> 1);

	// create the instance group in the good sortBlock
	CVegetableInstanceGroup		*vegetIg= vegeManager->createIg(_VegetableSortBlock[tileId]);
	_VegetableIG[tileId][distType]= vegetIg;

	// generate. Under RYZOM_VEG_STATE ask why an empty instance group is empty.
	// A tile with no material (VegSkipNoTile) is retried when its tile key changes:
	// CPatch marks this block dirty when a tile material arrives/leaves, and
	// refreshVegetableIgs() regenerates the tiles whose data changed.
	static const bool	vegStateLog= getenv("RYZOM_VEG_STATE") != NULL;
	TVegSkipReason	skipReason= VegSkipGenerated;
	// Always ask for the skip reason: the empty near-retry decision below must
	// not depend on the state log being enabled.
	_Patch->generateTileVegetable(vegetIg, distType, tms, tmt, vbCreateCtx, &skipReason);

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
				nlinfo("VEGSTATE:   ig d%u slot %u EMPTY (%s) at (%.1f %.1f) material %u",
					distType, tileId, vegSkipReasonName(skipReason), _Center.x, _Center.y,
					_Patch->getTileIdAt(tms, tmt));
			else if((emptyIgCount & 0xFF) == 0)
				nlinfo("VEGSTATE:   %u empty ig so far (last %s)", emptyIgCount, vegSkipReasonName(skipReason));
		}

		// Near group with authored sources that generated no instances: the
		// world-load can do this before the patch/tile data is fully usable.
		// Retry the near buckets after 1, 2 then 3 seconds; if the retry still
		// comes out empty, give up (the far d4 empties are expected and are
		// deliberately excluded). Without this the group stays null and nothing
		// marks the block dirty, so only a full MicroVeget toggle would fix it.
		if(distType < 3 && skipReason == VegSkipGenerated && _NearRetryCount < 3)
		{
			++_NearRetryCount;
			_NearRetryPending= true;
			_NearRetryTime= NLMISC::CTime::getLocalTime() + _NearRetryCount;
			if(vegStateLog)
				nlinfo("VEGSTATE:   near retry %u scheduled for d%u slot %u (material %u) in %us",
					(uint)_NearRetryCount, distType, tileId, _Patch->getTileIdAt(tms, tmt),
					(uint)_NearRetryCount);
		}
		vegeManager->deleteIg(vegetIg);
		_VegetableIG[tileId][distType] = nullptr;
	}
	else if(distType < 3)
	{
		// A healthy near group: a later empty group is a new event with its own
		// retry budget.
		_NearRetryCount= 0;
	}

	// NB: vegtable SortBlock is updated in CLandscapeVegetableBlock::update() after
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
	for(uint tileId= 0; tileId < NL3D_TESSBLOCK_TILESIZE; ++tileId)
		createVegetableIGForTile(tileId, i, vegeManager, vbCreateCtx);

	// The instance groups were generated from the current tile data: cache the
	// keys, so a later refresh can tell whether a tile changed at all. This is
	// also what resolves a pending region change: a block that re-enters after
	// leaving the far edge generates the new tiles here.
	for(uint tileId= 0; tileId < NL3D_TESSBLOCK_TILESIZE; ++tileId)
	{
		uint	tms= _Ts + (tileId & 1);
		uint	tmt= _Tt + (tileId >> 1);
		_TileKey[tileId]= _Patch->getTileVegetableKey(tms, tmt);
	}
	_Stale= false;
}


// ***************************************************************************
/// Number of authored species in the near bands (0..2). Used to decide whether a
/// tile-id change brings MORE near vegetation than the old field: sparse -> dense
/// must regenerate (the current tile has grass the retained field does not),
/// dense -> sparse (or equal) keeps the old field.
static uint		nearBandSourceCount(const CTileVegetableDesc &desc)
{
	uint	n= 0;
	for(uint k= 0; k < 3; ++k)
		n+= (uint)desc.getVegetableList(k).size();
	return n;
}


// ***************************************************************************
uint			CLandscapeVegetableBlock::refreshVegetableIgs(uint firstBucket, CVegetableManager *vegeManager,
	uint lastBucket)
{
	const CVegetableExtra	&extra= vegeManager->getExtra();
	const bool				extraEnabled= extra.isEnabled();
	static const bool		vegStateLog= getenv("RYZOM_VEG_STATE") != NULL;
	const NLMISC::TTicks	start= vegStateLog ? NLMISC::CTime::getPerformanceTime() : 0;

	CLandscapeVegetableBlockCreateContext	ctx;
	ctx.init(_Patch, _Ts, _Tt);

	// Low 48 bits: the three 16-bit tile ids. The water state lives above them.
	const uint64	idMask= 0x0000FFFFFFFFFFFFULL;

	// A near-only retry must not adopt a changed tile key: the full refresh is
	// the one that regenerates every alive bucket for a changed tile.
	const bool	nearOnly= lastBucket < NL3D_VEGETABLE_BLOCK_NUMDIST;

	uint	changedTiles= 0;
	uint	retainedTiles= 0;
	uint	filledTiles= 0;
	uint	regeneratedIg= 0;
	for(uint tileId= 0; tileId < NL3D_TESSBLOCK_TILESIZE; ++tileId)
	{
		uint	tms= _Ts + (tileId & 1);
		uint	tmt= _Tt + (tileId >> 1);
		uint64	key= _Patch->getTileVegetableKey(tms, tmt);

		// Nothing is alive for this block: there is no visible field to preserve.
		// Re-seed the key so a later entry generates from the current tiles.
		if(firstBucket >= NL3D_VEGETABLE_BLOCK_NUMDIST)
		{
			_TileKey[tileId]= key;
			continue;
		}

		const bool	keyChanged= key != _TileKey[tileId];
		if(nearOnly && keyChanged)
			continue;
		const uint	oldTile0= (uint)((_TileKey[tileId] >> 32) & 0xFFFF);
		const uint	newTile0= _Patch->getTileIdAt(tms, tmt);
		const bool	oldHasTile= oldTile0 != NL_TILE_ELM_LAYER_EMPTY;
		const bool	newHasTile= newTile0 != NL_TILE_ELM_LAYER_EMPTY;
		const bool	idsChanged= (_TileKey[tileId] & idMask) != (key & idMask);

		// Compare the near authored lists of the old and the new tile. A
		// sparse -> dense tile change means the current tile has near grass the
		// retained field does not: regenerate it instead of keeping the old field.
		// Dense -> sparse (or equal) keeps the old field.
		const uint	oldNearSources= oldHasTile
			? nearBandSourceCount(_Patch->getTileVegetableDesc(oldTile0)) : 0;
		const uint	newNearSources= newHasTile
			? nearBandSourceCount(_Patch->getTileVegetableDesc(newTile0)) : 0;
		const bool	moreNearSources= newNearSources > oldNearSources;

		// Tileset/region change under a live block: keep the existing instance
		// groups (the old field) and only fill the alive buckets whose IG is
		// missing, unless the new tile has more near sources (sparse -> dense).
		// The old field stays visible while this block remains within the far
		// edge; when it leaves, its IGs are deleted and a later re-entry
		// generates the new region.
		const bool	retain= extraEnabled && keyChanged && oldHasTile && newHasTile
			&& idsChanged && !moreNearSources;
		if(retain)
		{
			_Stale= true;
			++retainedTiles;
			if(vegStateLog)
				nlinfo("VEGSTATE:   retain slot %u old material %u new material %u "
					"near %u -> %u (old field kept)",
					tileId, oldTile0, newTile0, oldNearSources, newNearSources);
		}
		else if(keyChanged)
		{
			// Material availability changed (empty tile -> populated, or the
			// reverse), the water state changed, or the new tile brings more
			// near sources (sparse -> dense): the M1 refresh. Existing IGs are
			// regenerated from the current tiles.
			++changedTiles;
			if(vegStateLog && moreNearSources && idsChanged)
				nlinfo("VEGSTATE:   refresh slot %u material %u -> %u near %u -> %u (new field)",
					tileId, oldTile0, newTile0, oldNearSources, newNearSources);
		}

		bool	tileFilled= false;
		for(uint i= firstBucket; i < lastBucket; ++i)
		{
			if(!extra.hasAnySources(i))
				continue;

			const bool	missingIg= _VegetableIG[tileId][i] == nullptr;
			if(!keyChanged && !missingIg)
				continue;
			if(retain && !missingIg)
				continue;

			if(_VegetableIG[tileId][i])
			{
				if(vegStateLog)
					nlinfo("VEGSTATE:   refresh delete d%u slot %u", i, tileId);
				vegeManager->deleteIg(_VegetableIG[tileId][i]);
				_VegetableIG[tileId][i] = nullptr;
			}

			if(vegStateLog)
				nlinfo("VEGSTATE:   refresh create d%u slot %u (material %u)", i, tileId, newTile0);
			createVegetableIGForTile(tileId, i, vegeManager, ctx);
			++regeneratedIg;
			if(!keyChanged)
				tileFilled= true;
		}

		if(tileFilled)
			++filledTiles;

		// A retained tile keeps the key of the IGs that are still drawn; a
		// changed non-retained tile adopts the current key. An unchanged tile
		// only had missing IGs filled.
		if(!retain)
			_TileKey[tileId]= key;
	}

	if(vegStateLog && (changedTiles > 0 || retainedTiles > 0 || filledTiles > 0))
	{
		double	ms= NLMISC::CTime::ticksToSecond(NLMISC::CTime::getPerformanceTime() - start) * 1000.0;
		nlinfo("VEGSTATE:   refresh %u/%u tiles, %u ig, %u retained, %u filled, stale %u in %.3f ms",
			changedTiles, (uint)NL3D_TESSBLOCK_TILESIZE, regeneratedIg, retainedTiles,
			filledTiles, (uint)_Stale, ms);
	}

	return regeneratedIg;
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
