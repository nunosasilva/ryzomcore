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


#include "nel/3d/patch.h"
#include "nel/3d/vegetable.h"
#include "nel/3d/vegetable_manager.h"
#include "nel/3d/landscape_vegetable_block.h"
#include "nel/3d/landscape.h"
#include "nel/misc/vector.h"
#include "nel/misc/common.h"
#include "nel/misc/fast_floor.h"
#include "nel/3d/tile_vegetable_desc.h"
#include "nel/3d/vegetable_light_ex.h"
#include "nel/3d/patchdlm_context.h"


using namespace std;
using namespace NLMISC;

#ifdef DEBUG_NEW
#define new DEBUG_NEW
#endif

namespace NL3D
{


// ***************************************************************************
void		CPatch::generateTileVegetable(CVegetableInstanceGroup *vegetIg, uint distType, uint ts, uint tt,
	CLandscapeVegetableBlockCreateContext &vbCreateCtx)
{
	uint	i;

	// Get tile infos for vegetable
	// =========================

	// Get the state for this vegetable tile
	CTileElement::TVegetableInfo	vegetWaterState= Tiles[tt * OrderS + ts].getVegetableState();
	// If vegetable disabled, skip!
	if(vegetWaterState == CTileElement::VegetableDisabled)
		return;

	// get the tileId under this tile (<=> the tile material)
	uint	tileId= Tiles[tt * OrderS + ts].Tile[0];

	// get list of vegetable for this tile, and for hist distanceType category.
	const CTileVegetableDesc		&tileVegetDesc= getLandscape()->getTileVegetableDesc(tileId);
	const std::vector<CVegetable>	&vegetableList= tileVegetDesc.getVegetableList(distType);
	uint							distAddSeed= tileVegetDesc.getVegetableSeed(distType);
	uint							numVegetable= (uint)vegetableList.size();

	// If any layer (2nd or 3rd) is set, but has no vegetable, then skip too
	// This is to ensure "no vegetable under buildings".
	if( Tiles[tt * OrderS + ts].Tile[1]!=NL_TILE_ELM_LAYER_EMPTY )
	{
		uint	tileId1= Tiles[tt * OrderS + ts].Tile[1];
		uint	tileId2= Tiles[tt * OrderS + ts].Tile[2];
		// NB: test distType
		if(getLandscape()->getTileVegetableDesc(tileId1).empty())
			return;
		if(tileId2!=NL_TILE_ELM_LAYER_EMPTY && getLandscape()->getTileVegetableDesc(tileId2).empty())
			return;
	}


	// compute approximate tile position and normal: get the middle
	float	tileU= (ts + 0.5f) / (float)OrderS;
	float	tileV= (tt + 0.5f) / (float)OrderT;
	CBezierPatch	*bpatch= unpackIntoCache();
	// Get approximate position for the tile (useful for noise). NB: eval() is faster than computeVertex().
	CVector		tilePos= bpatch->eval(tileU, tileV);
	// Get also the normal used for all instances on this tile (not precise,
	// don't take noise into account, but faster).
	CVector		tileNormal= bpatch->evalNormal(tileU, tileV);

	// Compute also position on middle of 4 edges of this tile, for generateGroupBiLinear().
	CVector		tilePosBiLinear[4];
	float		OOos= 1.0f / OrderS;
	float		OOot= 1.0f / OrderT;
	tilePosBiLinear[0]= bpatch->eval( (ts + 0.0f) * OOos, (tt + 0.5f) * OOot);
	tilePosBiLinear[1]= bpatch->eval( (ts + 1.0f) * OOos, (tt + 0.5f) * OOot);
	tilePosBiLinear[2]= bpatch->eval( (ts + 0.5f) * OOos, (tt + 0.0f) * OOot);
	tilePosBiLinear[3]= bpatch->eval( (ts + 0.5f) * OOos, (tt + 1.0f) * OOot);


	// compute a rotation matrix with the normal
	CMatrix		matInstance;
	matInstance.setRot(CVector::I, CVector::J, tileNormal);
	// must normalize the matrix. use the vector which is the most orthogonal to tileNormal
	// If tileNormal is much more a J vector, then use plane (I,tileNormal), and vice-versa
	if(fabs(tileNormal.y) > fabs(tileNormal.x))
		matInstance.normalize(CMatrix::ZXY);
	else
		matInstance.normalize(CMatrix::ZYX);


	// prepare color / lighting
	// =========================

	// say that ambient never change. VegetableManager handle the ambient and diffuse itself (for precomputeLighting)
	CRGBAF	ambientF= CRGBAF(1,1,1,1);

	// Compute the tileLightmap (not modified by tileColor).
	static	uint8	tileLumelmap[NL_LUMEL_BY_TILE * NL_LUMEL_BY_TILE];
	getTileLumelmapPrecomputed(ts, tt, tileLumelmap, NL_LUMEL_BY_TILE);
	// compute diffuse color by substracting from ambient.
	CRGBAF	diffuseColorF[NL_LUMEL_BY_TILE * NL_LUMEL_BY_TILE];
	// TODO_VEGET_OPTIM: optimize this.
	// For all lumel of this tile.
	for(i= 0; i<NL_LUMEL_BY_TILE*NL_LUMEL_BY_TILE; i++)
	{
		// mul by 2, because shade is done twice here: by vertex, and by landscape.
		sint	tileLumel= 2*tileLumelmap[i];
		tileLumel= min(tileLumel, 255);
		float	tlf= tileLumel / 255.f;
		diffuseColorF[i].R= tlf;
		diffuseColorF[i].G= tlf;
		diffuseColorF[i].B= tlf;
		diffuseColorF[i].A= 1;
	}

	// Compute The CVegetableLightEx, adding pointLight effect to vegetation
	// First get pointLight at this tiles.
	static	vector<CPointLightInfluence>	lightList;
	lightList.clear();
	appendTileLightInfluences( CUV(tileU, tileV), lightList);
	// for each light, modulate the factor of influence
	for(i=0; i<lightList.size();i++)
	{
		CPointLight	*pl= lightList[i].PointLight;
		// compute the attenuation to the pos of the tile
		float	att= pl->computeLinearAttenuation(tilePos);
		// modulate the influence with this factor
		lightList[i].BkupInfluence= lightList[i].Influence;
		lightList[i].Influence*= att;
	}
	// sort the light by influence
	sort(lightList.begin(), lightList.end());
	// Setup the vegetLex directly in the ig.
	CVegetableLightEx	&vegetLex= vegetIg->VegetableLightEx;
	// take only 2 first, computing direction to tilePos and computing attenuation.
	vegetLex.NumLights= min((uint)CVegetableLightEx::MaxNumLight, (uint)lightList.size());
	for(i=0;i<vegetLex.NumLights;i++)
	{
		// WARNING: can C cast to CPointLightNamed here because comes from CPatch::appendTileLightInfluences() only!
		CPointLightNamed	*pl= (CPointLightNamed*)(lightList[i].PointLight);
		// copy to vegetLex.
		vegetLex.PointLight[i]= pl;
		// get the attenuation
		vegetLex.PointLightFactor[i]= (uint)(256* lightList[i].Influence);
		// Setup the direction from pointLight.
		vegetLex.Direction[i]= tilePos - pl->getPosition();
		vegetLex.Direction[i].normalize();
	}
	// compute now the current colors of the vegetLex.
	vegetLex.computeCurrentColors();


	// Compute Dynamic Lightmap UV for this tile.
	nlassert(_DLMContext);
	CUV		dlmUV;
	// get coordinate in 0..1 in texture.
	dlmUV.U= _DLMContext->DLMUBias + _DLMContext->DLMUScale * tileU;
	dlmUV.V= _DLMContext->DLMVBias + _DLMContext->DLMVScale * tileV;
	// get coordinate in 0..255.
	CVegetableUV8	dlmUV8;
	dlmUV8.U= (uint8)NLMISC::OptFastFloor(dlmUV.U * 255 + 0.5f);
	dlmUV8.V= (uint8)NLMISC::OptFastFloor(dlmUV.V * 255 + 0.5f);
	// bound them, ensuring 8Bits UV "uncompressed" by driver are in the lightmap area.
	clamp(dlmUV8.U, _DLMContext->MinU8, _DLMContext->MaxU8);
	clamp(dlmUV8.V, _DLMContext->MinV8, _DLMContext->MaxV8);


	// for all vegetable of this list, generate instances.
	// =========================

	// Union experiment: a bucket's IG also generates the species lists of the
	// lower buckets, so the near-field species reach the far field instead of
	// stopping at their authored band. Flattened source list: the bucket's own
	// list at the band density/scale, then (for carrier buckets >= FIRST_DIST)
	// every lower list at the union density, reusing that lower band's own seed
	// and scale so the copies are duplicates of a subset of its instances.
	struct	CVegetSource
	{
		const CVegetable	*Veget;
		uint				Seed;
		float				Density;
		float				Scale;
	};
	static	std::vector<CVegetSource>	vegetSources;
	vegetSources.clear();

	// Primary source: this band's own species list.
	float	distDensityFactor= getLandscape()->_VegetableManager->getDistDensityFactor(distType);
	// Far-only tilesets (desert floor/canyon: all authored species are in the
	// last band) have nothing in the lower bands, so the global far thinning
	// would remove most of the biome's vegetation. Keep the authored density
	// there; the lower-band check is exact and costs one list scan per IG.
	if(distType + 1 == NL3D_VEGETABLE_BLOCK_NUMDIST)
	{
		bool	lowerBandsEmpty= true;
		for(uint k= 0; k<distType; ++k)
		{
			if(!tileVegetDesc.getVegetableList(k).empty())
			{
				lowerBandsEmpty= false;
				break;
			}
		}
		if(lowerBandsEmpty)
			distDensityFactor= 1.f;
	}
	const	float	distInstanceScale= getLandscape()->_VegetableManager->getDistInstanceScale(distType);
	for(i= 0; i<numVegetable; i++)
	{
		CVegetSource	src;
		src.Veget= &vegetableList[i];
		src.Seed= i + distAddSeed;
		src.Density= distDensityFactor;
		src.Scale= distInstanceScale;
		vegetSources.push_back(src);
	}

#if NL3D_VEGETABLE_UNION_LOWER_SPECIES
	// Union sources: lower bands' species, thinned for this carrier bucket and
	// generated with the lower band's own seed and scale (exact duplicates of a
	// subset of that band's instances, so a band edge never swaps the plants).
	{
		const	float	unionDensity= getLandscape()->_VegetableManager->getUnionDensity(distType);
		if(unionDensity > 0.f)
		{
			for(uint k= 0; k<distType; ++k)
			{
				const std::vector<CVegetable>	&lowerList= tileVegetDesc.getVegetableList(k);
				const uint	seed= tileVegetDesc.getVegetableSeed(k);
				const float	lowerScale= getLandscape()->_VegetableManager->getDistInstanceScale(k);
				for(uint v= 0; v<lowerList.size(); ++v)
				{
					CVegetSource	src;
					src.Veget= &lowerList[v];
					src.Seed= v + seed;
					src.Density= unionDensity;
					src.Scale= lowerScale;
					vegetSources.push_back(src);
				}
			}
		}
	}
#endif

	// Get an array for each source (static for speed).
	typedef		std::vector<NLMISC::CVector2f>	TPositionVector;
	static	std::vector<TPositionVector>	instanceUVArray;
	// realloc if necessary.
	if(instanceUVArray.size() < vegetSources.size())
	{
		// clean.
		contReset(instanceUVArray);
		// realloc.
		instanceUVArray.resize(vegetSources.size());
	}

	// First, for each source, generate the number of instance to create, and their relative position.
	for(i= 0; i<(uint)vegetSources.size(); i++)
	{
		vegetSources[i].Veget->generateGroupBiLinear(tilePos, tilePosBiLinear, tileNormal, NL3D_PATCH_TILE_AREA,
			vegetSources[i].Seed, vegetSources[i].Density, instanceUVArray[i]);
	}

	// With the union, a tile whose own band list is empty can still carry the
	// lower bands' species. Only skip when there is nothing to generate at all.
	if(vegetSources.empty())
		return;

#if NL3D_VEGETABLE_UNION_LOWER_SPECIES
	if(getenv("RYZOM_VEG_STATE") != NULL)
	{
		nlinfo("VEGSTATE: tile %u d%u own %u union %u sources %u", tileId, distType, numVegetable,
			(uint)(vegetSources.size() - numVegetable), (uint)vegetSources.size());
	}
#endif

	// Then, now that we kno how many instance to generate for each vegetable, reserve space.
	CVegetableInstanceGroupReserve	vegetIgReserve;
	for(i= 0; i<(uint)vegetSources.size(); i++)
	{
		// reseve instance space for this vegetable.
		// instanceUVArray[i].size() is the number of instances to create.
		vegetSources[i].Veget->reserveIgAddInstances(vegetIgReserve, (CVegetable::TVegetableWater)vegetWaterState, (uint)instanceUVArray[i].size());
	}
	// actual reseve memory of the ig.
	getLandscape()->_VegetableManager->reserveIgCompile(vegetIg, vegetIgReserve);

	// Enlarge the thinned instances of this band, and fade them over the last
	// 10m of the band (the far bucket fades from 90m to FARDIST), so blocks do
	// not pop in at a band edge.
	const	float	blendDistMax= (distType+1 < NL3D_VEGETABLE_BLOCK_NUMDIST)
		? (distType+1) * NL3D_VEGETABLE_BLOCK_ELTDIST
		: NL3D_VEGETABLE_BLOCK_FARDIST;


	// generate the instances for all the sources.
	for(i= 0; i<(uint)vegetSources.size(); i++)
	{
		// get the vegetable
		const CVegetable	&veget=	*vegetSources[i].Veget;

		// get the relatives position of the instances
		std::vector<CVector2f>	&instanceUV= instanceUVArray[i];

		// For all instance, generate the real instances.
		for(uint j=0; j<instanceUV.size(); j++)
		{
			// generate the position in world Space.
			// instanceUV is in [0..1] interval, which maps to a tile, so explode to the patch
			CVector		instancePos;
			vbCreateCtx.eval(ts, tt, instanceUV[j].x, instanceUV[j].y, instancePos);
			// NB: use same normal for rotation for all instances in a same tile.
			matInstance.setPos( instancePos );

			// peek color into the lightmap.
			sint	lumelS= NLMISC::OptFastFloor(instanceUV[j].x * NL_LUMEL_BY_TILE);
			sint	lumelT= NLMISC::OptFastFloor(instanceUV[j].y * NL_LUMEL_BY_TILE);
			clamp(lumelS, 0, NL_LUMEL_BY_TILE-1);
			clamp(lumelT, 0, NL_LUMEL_BY_TILE-1);

			// generate the instance of the vegetable
			veget.generateInstance(vegetIg, matInstance, ambientF,
				diffuseColorF[ (lumelT<<NL_LUMEL_BY_TILE_SHIFT) + lumelS ],
				blendDistMax, (CVegetable::TVegetableWater)vegetWaterState, dlmUV8,
				vegetSources[i].Scale);
		}
	}
}


// ***************************************************************************
void	CPatch::recreateAllVegetableIgs()
{
	// For all TessBlocks, try to release their VegetableBlock
	for(uint numtb=0; numtb<TessBlocks.size(); numtb++)
	{
		// if the vegetableBlock is deleted, and if there is at least one Material in the tessBlock, and if possible
		if( TessBlocks[numtb].VegetableBlock == nullptr && TessBlocks[numtb].TileMaterialRefCount>0
			&& getLandscape()->isVegetableActive())
		{
			// compute tessBlock coordinate
			uint tbWidth= OrderS>>1;
			uint ts= numtb&(tbWidth-1);
			uint tt= numtb/tbWidth;
			// crate the vegetable with tilecooridante (ie tessBlock coord *2);
			createVegetableBlock(numtb, ts*2, tt*2);
		}
	}

}


// ***************************************************************************
void	CPatch::deleteAllVegetableIgs()
{
	// For all TessBlocks, try to release their VegetableBlock
	for(uint i=0; i<TessBlocks.size(); i++)
	{
		releaseVegetableBlock(i);
	}

}


// ***************************************************************************
void		CPatch::createVegetableBlock(uint numTb, uint ts, uint tt)
{
	// TessBlock width
	uint	tbWidth= OrderS >> 1;
	// clipBlock width
	uint	nTbPerCb= NL3D_PATCH_VEGETABLE_NUM_TESSBLOCK_PER_CLIPBLOCK;
	uint	cbWidth= (tbWidth + nTbPerCb-1) >> NL3D_PATCH_VEGETABLE_NUM_TESSBLOCK_PER_CLIPBLOCK_SHIFT;

	// compute tessBlock coordinate.
	uint	tbs ,tbt;
	tbs= ts >> 1;
	tbt= tt >> 1;
	// compute clipBlock coordinate.
	uint	cbs,cbt;
	cbs= tbs >> NL3D_PATCH_VEGETABLE_NUM_TESSBLOCK_PER_CLIPBLOCK_SHIFT;
	cbt= tbt >> NL3D_PATCH_VEGETABLE_NUM_TESSBLOCK_PER_CLIPBLOCK_SHIFT;

	// create the vegetable block.
	CLandscapeVegetableBlock		*vegetBlock= new CLandscapeVegetableBlock;
	// Init / append to list.
	// compute center of the vegetableBlock (approx).
	CBezierPatch	*bpatch= unpackIntoCache();
	CVector		center= bpatch->eval( (float)(tbs*2+1)/OrderS, (float)(tbt*2+1)/OrderT );
	// Lower-Left tile is (tbs*2, tbt*2)
	vegetBlock->init(center, getLandscape()->_VegetableManager, VegetableClipBlocks[cbt *cbWidth + cbs], this, tbs*2, tbt*2, getLandscape()->_VegetableBlockList);

	// set in the tessBlock
	TessBlocks[numTb].VegetableBlock= vegetBlock;
}


// ***************************************************************************
void		CPatch::releaseVegetableBlock(uint numTb)
{
	// if exist, must delete the VegetableBlock.
	if(TessBlocks[numTb].VegetableBlock)
	{
		// delete Igs, and remove from list.
		TessBlocks[numTb].VegetableBlock->release(getLandscape()->_VegetableManager, getLandscape()->_VegetableBlockList);
		// delete.
		delete TessBlocks[numTb].VegetableBlock;
		TessBlocks[numTb].VegetableBlock = nullptr;
	}
}





} // NL3D
