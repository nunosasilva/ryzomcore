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

#include <cstdlib>

#include "nel/3d/mesh_multi_lod_instance.h"
#include "nel/3d/mesh_multi_lod.h"
#include "nel/3d/coarse_mesh_manager.h"
#include "nel/3d/scene.h"
#include "nel/3d/driver.h"
#include "nel/3d/modern_shadow.h"

#include "nel/misc/debug.h"

using namespace NLMISC;

#ifdef DEBUG_NEW
#define new DEBUG_NEW
#endif

namespace NL3D
{

// ***************************************************************************

	CMeshMultiLodInstance::CMeshMultiLodInstance ()
{
	// No flags
	Flags=0;
	// The LOD selection is computed by traverseLoadBalancing() for the models
	// that are in view. A caster that is currently outside the view frustum
	// (the mask keeps drawing the scene's models, see CScene::getAllModels())
	// never went through it, so the fields it would read must have a defined
	// value: draw slot 0, no second slot.
	Lod0=0;
	Lod1=0xffffffff;
	BlendFactor=0.f;
	PolygonCountLod0=0.f;
	PolygonCountLod1=0.f;
	_CoarseMeshDistance = -1.f;
	_LastCoarseMesh = nullptr;
	_LastCoarseMeshNumVertices= 0;
}

// ***************************************************************************

CMeshMultiLodInstance::~CMeshMultiLodInstance ()
{
}


// ***************************************************************************

void		CMeshMultiLodInstance::registerBasic()
{
	CScene::registerModel (MeshMultiLodInstanceId, MeshBaseInstanceId, CMeshMultiLodInstance::creator);
}


// ***************************************************************************
CRGBA		CMeshMultiLodInstance::getCoarseMeshLighting()
{
	CScene	*scene= getOwnerScene();
	nlassert(scene);

	// compute his sun contribution result, and update
	CRGBA	sunContrib= scene->getSunDiffuse();
	// simulate/average diffuse lighting over the mesh by dividing diffuse by 2.
	sunContrib.modulateFromuiRGBOnly(sunContrib, getLightContribution().SunContribution/2 );
	// Add Ambient
	sunContrib.addRGBOnly(sunContrib, scene->getSunAmbient());
	sunContrib.A= 255;

	return sunContrib;
}


// ***************************************************************************

void		CMeshMultiLodInstance::traverseLoadBalancing()
{
	// Call previous
	CMeshBaseInstance::traverseLoadBalancing ();

	// If this is the second pass of LoadBalancing, choose the Lods, according to getNumTrianglesAfterLoadBalancing()
	CLoadBalancingTrav		&loadTrav= getOwnerScene()->getLoadBalancingTrav();
	if(loadTrav.getLoadPass()==1)
	{
		// Get a pointer on the shape
		CMeshMultiLod *shape=safe_cast<CMeshMultiLod*> ((IShape*)Shape);

		// Reset render pass
		if	(!getBypassLODOpacityFlag())
		{
			setTransparency(false);
			setOpacity(false);
		}

		// Get the wanted number of polygons
		float polygonCount= getNumTrianglesAfterLoadBalancing ();

		// Look for the good slot
		uint meshCount=(uint)shape->_MeshVector.size();
		Lod0=0;
		if (meshCount>1)
		{
			// Look for good i
			while ( polygonCount < shape->_MeshVector[Lod0].EndPolygonCount )
			{
				Lod0++;
				if (Lod0==meshCount-1)
					break;
			}
		}

		// The slot
		CMeshMultiLod::CMeshSlot	&slot=shape->_MeshVector[Lod0];

		// Get the distance with polygon count
		float distance=(polygonCount-slot.B)/slot.A;

		// Get the final polygon count
		if (slot.MeshGeom)
			PolygonCountLod0=slot.MeshGeom->getNumTriangles (distance);

		// Second slot in use ?
		Lod1=0xffffffff;

		// The next slot
		CMeshMultiLod::CMeshSlot	*nextSlot = nullptr;

		// Next slot exist ?
		if (Lod0!=meshCount-1)
		{
			nextSlot=&(shape->_MeshVector[Lod0+1]);
		}

		// Max dist before blend.
		// The slot that is blending out owns the blend length: its BlendOut flag
		// starts the crossfade. The next slot's BlendLength describes the
		// transition out of *it*, and using it here made the outgoing and
		// incoming meshes overlap for its length. With the plant LODs that is a
		// 15 m band against a 5 m fade-out on the near slot, so a Zorai flag (or
		// tree, or tent) stayed visible as a static copy next to the waving one
		// from 30 m down to 15 m.
		float startBlend=slot.DistMax-slot.BlendLength;

		// In blend zone ? (a zero length blend would divide by zero)
		if ( slot.BlendLength > 0.f && startBlend < distance )
		{
			// Alpha factor for main Lod
			BlendFactor = (slot.DistMax-distance)/(slot.DistMax-startBlend);
			if (BlendFactor<0)
				BlendFactor=0;
			nlassert (BlendFactor<=1);

			// Render this mesh
			if (slot.MeshGeom)
			{
				if (slot.Flags&CMeshMultiLod::CMeshSlot::BlendOut)
				{
					// Render the geom mesh with alpha blending with goodPolyCount
					if (!getBypassLODOpacityFlag()) setTransparency(true);
					Flags|=CMeshMultiLodInstance::Lod0Blend;
				}
				else
				{
					// Render the geom mesh without alpha blending with goodPolyCount
					if (!getBypassLODOpacityFlag()) setTransparency (slot.isTransparent());
					setOpacity (slot.isOpaque());
					Flags&=~CMeshMultiLodInstance::Lod0Blend;
				}
			}
			else
				Lod0=0xffffffff;

			// Next mesh, BlendIn actived ?
			if (nextSlot && shape->_MeshVector[Lod0+1].MeshGeom && (nextSlot->Flags&CMeshMultiLod::CMeshSlot::BlendIn))
			{
				// Render the geom mesh with alpha blending with nextSlot->BeginPolygonCount
				PolygonCountLod1=nextSlot->MeshGeom->getNumTriangles (distance);
				Lod1=Lod0+1;
				if (!getBypassLODOpacityFlag()) setTransparency(true);
			}
		}
		else
		{
			if (slot.MeshGeom)
			{
				// Render without blend with goodPolyCount
				if (!getBypassLODOpacityFlag())
				{
					setTransparency (slot.isTransparent());
					setOpacity (slot.isOpaque());
				}
				Flags&=~CMeshMultiLodInstance::Lod0Blend;
			}
			else
				Lod0=0xffffffff;
		}


	}
}

// ***************************************************************************
void		CMeshMultiLodInstance::changeMRMDistanceSetup(float distanceFinest, float distanceMiddle, float distanceCoarsest)
{
	if(Shape)
	{
		// Get a pointer on the shape.
		CMeshMultiLod *pMesh =safe_cast<CMeshMultiLod*> ((IShape*)Shape);
		// Affect the mesh directly.
		pMesh->changeMRMDistanceSetup(distanceFinest, distanceMiddle, distanceCoarsest);
	}
}


// ***************************************************************************
float	   CMeshMultiLodInstance::getNumTriangles (float distance)
{
	CMeshMultiLod *shape = safe_cast<CMeshMultiLod*> ((IShape*)Shape);
	return shape->getNumTrianglesWithCoarsestDist(distance, _CoarseMeshDistance);
}


// ***************************************************************************
bool		CMeshMultiLodInstance::computeWorldBBoxForShadow(NLMISC::CAABBox &worldBB)
{
	if (!isHrcVisible() || !Shape)
		return false;

	CMeshMultiLod	*shape = safe_cast<CMeshMultiLod*> ((IShape*)Shape);

	// Union of every LOD slot, not just the shape's getAABBox() (which
	// returns the first available slot). The slot the frame draws is chosen
	// by the load balancing traversal, a coarse slot is a different mesh, and
	// a caster outside the frustum keeps a stale selection - so the only
	// bound that is always right is the union of all of them.
	bool	haveBB = false;
	const uint	meshCount = shape->getNumSlotMesh();
	for (uint slot = 0; slot < meshCount; ++slot)
	{
		bool		coarseMesh = false;
		IMeshGeom	*meshGeom = shape->getSlotMesh(slot, coarseMesh);
		if (!meshGeom)
			continue;
		const CAABBox	slotBB = meshGeom->getBoundingBox().getAABBox();
		if (!haveBB)
		{
			worldBB = slotBB;
			haveBB = true;
		}
		else
			worldBB = CAABBox::computeAABBoxUnion(worldBB, slotBB);
	}
	if (!haveBB)
		return false;

	worldBB = CAABBox::transformAABBox(getWorldMatrix(), worldBB);
	return isValidModernShadowCasterBBox(worldBB);
}


// ***************************************************************************
void		CMeshMultiLodInstance::renderSlotModernShadowCaster(IDriver *driver, uint slot, CMaterial &castMat)
{
	CMeshMultiLod	*shape = safe_cast<CMeshMultiLod*> ((IShape*)Shape);
	if (slot >= shape->getNumSlotMesh())
		return;
	bool			coarseMesh = false;
	IMeshGeom		*meshGeom = shape->getSlotMesh(slot, coarseMesh);
	if (meshGeom)
		meshGeom->renderModernShadowCaster(driver, this, castMat);
}


// ***************************************************************************
void		CMeshMultiLodInstance::renderModernShadowCaster(IDriver *driver, CMaterial &castMat)
{
	if (!Shape)
		return;

	// Same convention as CMeshInstance: the cascade camera maps the world to
	// [0,1]^3, and the slot vertex buffers are local to the shape. The camera
	// is set once per cascade by the mask pass, so only the model matrix is
	// per caster.
	driver->setupModelMatrix(getWorldMatrix());

	// The mask draws slot 0, the finest LOD, *not* the slot the visible pass
	// picked. Lod0/Lod1 come from the load-balancing budget, which is per
	// frame and per view: a camera turn changes the visible set, hence the
	// budget, hence the slot, and a coarser slot can have a different
	// silhouette. In the mask that reads as the shadow of a big caster
	// changing shape or vanishing between two frames of a turn (the mats004/
	// mats005 report). The depth mask has no per-pixel cost of its own beyond
	// the triangles it draws, and a stable silhouette is worth more than the
	// budget: slot 0 never depends on the view.
	//
	// NLVK_SHADOW_MASK_LOD_VISIBLE=1 restores the old behaviour (the visible
	// Lod0, plus Lod1 during a cross-fade) so the two can be A/B'd on one
	// machine; inert without the env var.
	{
		static const bool	visibleLod = getenv("NLVK_SHADOW_MASK_LOD_VISIBLE") != NULL;
		if (visibleLod)
		{
			renderSlotModernShadowCaster(driver, Lod0, castMat);
			if (Lod1 != 0xffffffff)
				renderSlotModernShadowCaster(driver, Lod1, castMat);
		}
		else
			renderSlotModernShadowCaster(driver, 0, castMat);
	}
}


// ***************************************************************************
void		CMeshMultiLodInstance::initRenderFilterType()
{
	if(Shape)
	{
		CMeshMultiLod *shape = safe_cast<CMeshMultiLod*> ((IShape*)Shape);

		// Look only the First LOD to know if it has a VP or not
		bool			hasVP= false;
		bool			coarseMesh;
		if(shape->getNumSlotMesh()>0 && shape->getSlotMesh(0, coarseMesh))
		{
			IMeshGeom		*meshGeom= shape->getSlotMesh(0, coarseMesh);
			// hasVP possible only if not a coarseMesh.
			if(!coarseMesh)
				hasVP= meshGeom->hasMeshVertexProgram();
		}

		if(hasVP)
			_RenderFilterType= UScene::FilterMeshLodVP;
		else
			_RenderFilterType= UScene::FilterMeshLodNoVP;
	}
}


// ***************************************************************************
void		CMeshMultiLodInstance::setUVCoarseMesh( CMeshGeom &geom, uint vtDstSize, uint dstUvOff )
{
	// *** Copy UVs to the vertices

	// Src vertex buffer
	const CVertexBuffer &vbSrc=geom.getVertexBuffer();
	CVertexBufferRead vba;
	vbSrc.lock (vba);

	// Check the vertex format and src Vertices
	nlassert (vbSrc.getVertexFormat() & (CVertexBuffer::PositionFlag|CVertexBuffer::TexCoord0Flag) );
	nlassert (vbSrc.getNumVertices()==_LastCoarseMeshNumVertices);

	// src Vertex size
	uint vtSrcSize=vbSrc.getVertexSize ();

	// Copy vector
	const uint8 *vSrc = (const uint8 *)vba.getTexCoordPointer(0,0);
	uint8 *vDest = &_CoarseMeshVB[0];
	vDest+= dstUvOff;

	// Transform it
	for (uint i=0; i<_LastCoarseMeshNumVertices; i++)
	{
		// Transform position
		*(CUV*)vDest = *(const CUV*)vSrc;

		// Next point
		vSrc+=vtSrcSize;
		vDest+=vtDstSize;
	}
}
// ***************************************************************************
void		CMeshMultiLodInstance::setPosCoarseMesh( CMeshGeom &geom, const CMatrix &matrix, uint vtDstSize )
{
	// *** Transform the vertices

	// Src vertex buffer
	const CVertexBuffer &vbSrc=geom.getVertexBuffer();
	CVertexBufferRead vba;
	vbSrc.lock (vba);

	// Check the vertex format and src Vertices
	nlassert (vbSrc.getVertexFormat() & (CVertexBuffer::PositionFlag|CVertexBuffer::TexCoord0Flag) );
	nlassert (vbSrc.getNumVertices()==_LastCoarseMeshNumVertices);

	// src Vertex size
	uint vtSrcSize=vbSrc.getVertexSize ();

	// Copy vector
	const uint8 *vSrc = (const uint8 *)vba.getVertexCoordPointer (0);
	uint8 *vDest = &_CoarseMeshVB[0];

	// Transform it
	for (uint i=0; i<_LastCoarseMeshNumVertices; i++)
	{
		// Transform position
		*(CVector*)vDest = matrix.mulPoint (*(const CVector*)vSrc);

		// Next point
		vSrc+=vtSrcSize;
		vDest+=vtDstSize;
	}
}
// ***************************************************************************
void		CMeshMultiLodInstance::setColorCoarseMesh( CRGBA color, uint vtDstSize, uint dstColorOff )
{
	// *** Copy color to vertices

	// Copy vector
	uint8 *vDest = &_CoarseMeshVB[0];
	vDest+= dstColorOff;

	// Transform it
	for (uint i=0; i<_LastCoarseMeshNumVertices; i++)
	{
		// Transform position
		*(CRGBA*)vDest = color;

		// Next point
		vDest+=vtDstSize;
	}
}


} // NL3D
