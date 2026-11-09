/** \file decal_manager.cpp
 * Batched decal rendering manager for NeL 3D.
 *
 * Implements the rendering pseudocode from the intern report (§.6.3).
 * Includes the vertex program for distance attenuation, Z blending,
 * and diffuse color (ported from the legacy CLegacyDecal system).
 */

/* Copyright, 2007 Nevrax Ltd.
 *
 * This file is part of NEVRAX NEL.
 * NEVRAX NEL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.

 * NEVRAX NEL is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with NEVRAX NEL; see the file COPYING. If not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 */

#include "std3d.h"
#include "nel/3d/decal_manager.h"
#include "nel/3d/scene.h"

#include <algorithm>

using namespace std;
using namespace NLMISC;
using namespace NL3D;


// ***************************************************************************
// Comparator for sorting decals by priority (lower priority value = rendered first)
static bool decalPriorityCompare(const CDecal *a, const CDecal *b)
{
	return a->getPriority() < b->getPriority();
}


// ***************************************************************************
// Vertex program assembly code for decal attenuation (ported from legacy system)
// Constants:
//   c[0-3]: ModelViewProjection matrix
//   c[4]:   WorldToUV row 0 (→ tex U)
//   c[5]:   WorldToUV row 1 (→ tex V)
//   c[6]:   Camera position (relative to model origin)
//   c[7]:   DistScaleBias (x=scale, y=bias, z=0.0, w=1.0)
//   c[8]:   Diffuse color (RGB in [0,1])
//   c[11]:  BlendScale (x=bottomScale, y=bottomBias, z=topScale, w=topBias)
static const char *DecalAttenuationVertexProgramCode =
"!!VP1.0\n\
	DP4 o[HPOS].x, c[0], v[0];	          #transform vertex in view space\n\
	DP4 o[HPOS].y, c[1], v[0];\n\
	DP4 o[HPOS].z, c[2], v[0];\n\
	DP4 o[HPOS].w, c[3], v[0];\n\
	# transform texcoord 0\n\
	DP4 o[TEX0].x, c[4], v[0];\n\
	DP4 o[TEX0].y, c[5], v[0];\n\
	#compute distance from camera\n\
	ADD R0, v[0], -c[6];\n\
	DP3 R0.x, R0, R0;\n\
	RSQ R0.x, R0.x;\n\
	RCP R0.x, R0.x;\n\
	MUL o[COL0].xyz, c[8], v[3];\n\
	#compute attenuation with distance\n\
	MAD R0.w, R0.x, c[7].x, c[7].y;\n\
	# clamp in [0, 1]\n\
	MIN R0.w, R0.w, c[7].w;\n\
	MAX R0.w, R0.w, c[7].z;\n\
	#compute bottom blend\n\
	MAD R1.x, v[0].z, c[11].x, c[11].y;\n\
	MIN R1.x, R1.x, c[7].w;\n\
	MAX R1.x, R1.x, c[7].z;\n\
	MUL R0.w, R1.x, R0.w;\n\
	#compute top blend\n\
	MAD R1.x, v[0].z, c[11].z, c[11].w;\n\
	MIN R1.x, R1.x, c[7].w;\n\
	MAX R1.x, R1.x, c[7].z;\n\
	MUL R0.w, R1.x, R0.w;\n\
	#apply vertex alpha\n\
	MUL o[COL0].w, v[3].w, R0.w;\n\
	END \n";


// ***************************************************************************
CVertexProgramDecalAttenuation::CVertexProgramDecalAttenuation()
{
	CSource *source = new CSource();
	source->Profile = nelvp;
	source->DisplayName = "nelvp/DecalAttenuation";
	source->setSourcePtr(DecalAttenuationVertexProgramCode);
	source->ParamIndices["modelViewProjection"] = 0;
	source->ParamIndices["worldToUV0"] = 4;
	source->ParamIndices["worldToUV1"] = 5;
	source->ParamIndices["refCamDist"] = 6;
	source->ParamIndices["distScaleBias"] = 7;
	source->ParamIndices["diffuse"] = 8;
	source->ParamIndices["blendScale"] = 11;
	addSource(source);
}


// ***************************************************************************
CVertexProgramDecalAttenuation::~CVertexProgramDecalAttenuation()
{
}


// ***************************************************************************
void CVertexProgramDecalAttenuation::buildInfo()
{
	m_Idx.WorldToUV0 = getUniformIndex("worldToUV0");
	nlassert(m_Idx.WorldToUV0 != std::numeric_limits<uint>::max());
	m_Idx.WorldToUV1 = getUniformIndex("worldToUV1");
	nlassert(m_Idx.WorldToUV1 != std::numeric_limits<uint>::max());
	m_Idx.RefCamDist = getUniformIndex("refCamDist");
	nlassert(m_Idx.RefCamDist != std::numeric_limits<uint>::max());
	m_Idx.DistScaleBias = getUniformIndex("distScaleBias");
	nlassert(m_Idx.DistScaleBias != std::numeric_limits<uint>::max());
	m_Idx.Diffuse = getUniformIndex("diffuse");
	nlassert(m_Idx.Diffuse != std::numeric_limits<uint>::max());
	m_Idx.BlendScale = getUniformIndex("blendScale");
	nlassert(m_Idx.BlendScale != std::numeric_limits<uint>::max());
}


// ***************************************************************************
CDecalManager::CDecalManager() :
		_NextMaterialId(0),
		_UseVertexProgram(false),
		_DistScale(0.f),
		_DistBias(1.f)
{
	// Fixed-size AGP Volatile vertex buffer with Position + TexCoord0 + PrimaryColor (see PDF §4.5.4).
	// UV coordinates are precomputed on CPU by CDecal::generateUVs() and written to TexCoord0.
	// Per-vertex colors are precomputed by CDecal::computeColors().
	// This avoids per-decal VP constant changes and enables true batching by texture.
	// core4 replaced TPreferredMemory with TBufferUsage: the old AGPVolatile is now
	// CVertexBuffer::FullStream (see the serialisation mapping in vertex_buffer.cpp:913,
	// "case 4: _BufferUsage = FullStream; // AGPVolatile"). keepLocalMemory stays true
	// because the fill writes three interleaved attributes into the same buffer.
	_VB.setBufferUsage(CVertexBuffer::FullStream, true);
	_VB.setVertexFormat(CVertexBuffer::PositionFlag | CVertexBuffer::TexCoord0Flag | CVertexBuffer::PrimaryColorFlag);
	_VB.setNumVertices(NL3D_DECAL_VB_MAX_VERTICES);

	// Route texture stage 1 to read from TexCoord0 (same as stage 0).
	// The material uses 2 stages but we only have one UV channel in the VB.
	_VB.setUVRouting(1, 0);

	// Create the vertex program instance (retained for potential future use)
	_VertexProgram = new CVertexProgramDecalAttenuation();
}


// ***************************************************************************
CDecalManager::~CDecalManager()
{
}


// ***************************************************************************
void CDecalManager::clearAllDecals()
{
	_Decals.clear();
}


// ***************************************************************************
void CDecalManager::addDecal(CDecal *decal, uint32 materialId)
{
	_Decals[materialId].push_back(decal);
}


// ***************************************************************************
uint32 CDecalManager::registerMaterial(const CMaterial &mat)
{
	CRegisteredMaterial rm;
	rm.Mat = mat;
	rm.Id = _NextMaterialId;
	_Materials.push_back(rm);
	return _NextMaterialId++;
}


// ***************************************************************************
// Rendering: implements the pseudocode from PDF §.6.3
//
// Per-vertex colors are precomputed on the CPU by CDecal::computeColors().
// UV coordinates are precomputed on the CPU by CDecal::generateUVs() and
// written into TexCoord0. This avoids per-decal VP constant updates and
// enables true batching: a draw call is only needed when the texture
// pointer changes or the VB overflows.
void CDecalManager::flush(CScene *sc)
{
	if (_Decals.empty())
		return;

	IDriver *drv = sc->getRenderTrav().getDriver();

	drv->activeVertexProgram(NULL);
	drv->activeVertexBuffer(_VB);
	drv->setupModelMatrix(CMatrix::Identity);

	// The vertex buffer is ring-buffered (CVertexBuffer::FullStream), so a lock re-bases the
	// region a draw addresses. Interleaving locks and draws therefore renders only the first
	// batch of a frame - measured with the sample's --multi scenario (4 decals clipped,
	// 1 drawn) before this was restructured. Now a chunk of vertices is filled under ONE lock,
	// released, and only then are that chunk's draws issued.
	//
	// A chunk is one buffer's worth of vertices. Overflow needs a second chunk, which is why
	// this walks a flat list instead of nesting group/decal loops: a batch must never straddle
	// a lock boundary.
	struct TDrawBatch
	{
		CMaterial *Mat;
		uint32 FirstVertex;
		uint32 NumVerts;
	};

	std::vector<TDrawBatch> batches;
	std::vector<CDecal *> sorted; // decals in group order, priority-sorted inside a group

	TDecalMap::iterator matIt = _Decals.begin();
	for (; matIt != _Decals.end(); ++matIt)
	{
		std::vector<CDecal*> &decals = matIt->second;
		if (decals.empty())
			continue;
		// Lower priority values are rendered first.
		std::sort(decals.begin(), decals.end(), decalPriorityCompare);
		for (uint d = 0; d < decals.size(); ++d)
			sorted.push_back(decals[d]);
	}

	CVertexBufferReadWrite *vba = NULL;
	uint32 vbOffset = 0;

	for (uint d = 0; d < sorted.size(); ++d)
	{
		CDecal *decal = sorted[d];
		std::vector<CVector> &verts = decal->getVertices(false);
		if (verts.empty())
			continue;

		const std::vector<CUV> &uvs = decal->getUVs();
		const std::vector<NLMISC::CRGBA> &colors = decal->getColors();
		const bool hasUVs = (uvs.size() == verts.size());
		const bool hasColors = (colors.size() == verts.size());

		const uint32 length = (uint32)verts.size();
		uint32 srcOffset = 0;
		while (srcOffset < length)
		{
			if (vba == NULL)
			{
				if (!batches.empty())
				{
					// Close the previous chunk: unlock first, then draw what it holds.
					delete vba; // (null here; kept for symmetry)
				}
				vba = new CVertexBufferReadWrite();
				_VB.lock(*vba);
				vbOffset = 0;
			}

			uint32 space = NL3D_DECAL_VB_MAX_VERTICES - vbOffset;
			if (space == 0)
			{
				// Full: release the lock, draw this chunk, and start the next one.
				delete vba;
				vba = NULL;
				for (uint b = 0; b < batches.size(); ++b)
				{
					nlassert(batches[b].NumVerts != 0 && (batches[b].NumVerts % 3) == 0);
					drv->renderRawTriangles(*batches[b].Mat, batches[b].FirstVertex, batches[b].NumVerts / 3);
				}
				batches.clear();
				continue;
			}

			const uint32 batch = std::min(length - srcOffset, space);
			for (uint32 i = 0; i < batch; ++i)
			{
				const uint32 vi = vbOffset + i;
				const uint32 si = srcOffset + i;
				*vba->getVertexCoordPointer(vi) = verts[si];
				vba->setTexCoord(vi, 0, hasUVs ? uvs[si] : CUV(0, 0));
				vba->setColor(vi, hasColors ? colors[si] : CRGBA::White);
			}

			// One batch per decal segment. Grouping by texture pointer buys nothing here:
			// every CDecal owns its own CMaterial (CDecal::_Mat) and setTexture() creates a
			// new CTextureFile per decal, so two decals never share a texture pointer.
			if (!batches.empty() && batches.back().Mat == &decal->getMaterial() &&
				batches.back().FirstVertex + batches.back().NumVerts == vbOffset)
			{
				batches.back().NumVerts += batch;
			}
			else
			{
				TDrawBatch nb;
				nb.Mat = &decal->getMaterial();
				nb.FirstVertex = vbOffset;
				nb.NumVerts = batch;
				batches.push_back(nb);
			}

			vbOffset += batch;
			srcOffset += batch;
		}
	}

	delete vba; // unlock the last chunk
	vba = NULL;

	for (uint b = 0; b < batches.size(); ++b)
	{
		nlassert(batches[b].NumVerts != 0 && (batches[b].NumVerts % 3) == 0);
		drv->renderRawTriangles(*batches[b].Mat, batches[b].FirstVertex, batches[b].NumVerts / 3);
	}
}
