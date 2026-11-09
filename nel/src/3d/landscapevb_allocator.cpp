// NeL - MMORPG Framework <http://dev.ryzom.com/projects/nel/>
// Copyright (C) 2010  Winch Gate Property Limited
//
// This source file has been modified by the following contributors:
// Copyright (C) 2013  Jan BOON (Kaetemi) <jan.boon@kaetemi.be>
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

#include "nel/3d/landscapevb_allocator.h"
#include "nel/3d/driver.h"
#include "nel/3d/uniform_buffer.h"
#include "nel/misc/fast_mem.h"


using namespace std;
using namespace NLMISC;

#ifdef DEBUG_NEW
#define new DEBUG_NEW
#endif

namespace NL3D
{

/*
	Once a reallocation of a VBHard occurs, how many vertices we add to the re-allocation, to avoid
	as possible reallocations.
*/
#define	NL3D_LANDSCAPE_VERTEX_ALLOCATE_SECURITY		1024
/*
	The start size of the array.
*/
#define	NL3D_LANDSCAPE_VERTEX_ALLOCATE_START		4048


#define	NL3D_VERTEX_FREE_MEMORY_RESERVE	1024
/*// 65000 is a maximum because of GeForce limitations.
#define	NL3D_VERTEX_MAX_VERTEX_VBHARD	40000*/


// ***************************************************************************
CLandscapeVBAllocator::CLandscapeVBAllocator(TType type, const std::string &vbName)
{
	_Type= type;
	_VBName= vbName;
	_VertexFreeMemory.reserve(NL3D_VERTEX_FREE_MEMORY_RESERVE);

	_ReallocationOccur= false;
	_NumVerticesAllocated= 0;
	_UnsynchronizedMode= false;
	_BufferLocked= false;
	_LastFarVB = nullptr;
	_LastNearVB = nullptr;

	for(uint i=0;i<MaxVertexProgram;i++)
		_VertexProgram[i] = nullptr;
}

// ***************************************************************************
CLandscapeVBAllocator::~CLandscapeVBAllocator()
{
	clear();
}


// ***************************************************************************
void			CLandscapeVBAllocator::updateDriver(IDriver *driver)
{
	// test change of driver.
	nlassert(driver);
	if( _Driver == nullptr || driver!=_Driver )
	{
		deleteVertexBuffer();
		_Driver= driver;


		// If change of driver, delete the VertexProgram first, if any
		deleteVertexProgram();
		// Then rebuild VB format, and VertexProgram, if needed.
		// Do it only if VP supported by GPU.
		setupVBFormatAndVertexProgram(!_Driver->isVertexProgramEmulated() && (
			_Driver->supportVertexProgram(CVertexProgram::nelvp) ||
			// the four landscape programs are hand-written GLSL on a user UBO, so a driver
			// that can take UBOs can run them without nelvp support. This is the same predicate
			// setupVBFormatAndVertexProgram() branches on below, so the gate and the code
			// path cannot disagree. Only CDriverGL3 returns true (driver_opengl3.h:1497);
			// no driver today takes this half without also taking the nelvp half.
			_Driver->supportBuiltinUBO()));

		// must reallocate the VertexBuffer.
		if( _NumVerticesAllocated>0 )
			allocateVertexBuffer(_NumVerticesAllocated);
	}
}


// ***************************************************************************
void			CLandscapeVBAllocator::clear()
{
	// clear list.
	_VertexFreeMemory.clear();
	_DeferredFreeVertices.clear();
	_NumVerticesAllocated= 0;

	// delete the VB.
	deleteVertexBuffer();

	// delete vertex Program, if any
	deleteVertexProgram();

	// clear other states.
	_ReallocationOccur= false;
	_UnsynchronizedMode= false;
	_Driver = nullptr;
}



// ***************************************************************************
void			CLandscapeVBAllocator::resetReallocation()
{
	_ReallocationOccur= false;
}



// ***************************************************************************
// ***************************************************************************
// allocation.
// ***************************************************************************
// ***************************************************************************



// ***************************************************************************
uint			CLandscapeVBAllocator::allocateVertex()
{
	// if no more free, allocate.
	if( _VertexFreeMemory.empty() )
	{
		// enlarge capacity.
		uint	newResize;
		if(_NumVerticesAllocated==0)
			newResize= NL3D_LANDSCAPE_VERTEX_ALLOCATE_START;
		else
			newResize= NL3D_LANDSCAPE_VERTEX_ALLOCATE_SECURITY;
		_NumVerticesAllocated+= newResize;
		// re-allocate VB.
		#ifdef NL_LANDSCAPE_INDEX16
			nlassert(_NumVerticesAllocated <= 65535);
		#endif
		allocateVertexBuffer(_NumVerticesAllocated);
		// resize infos on vertices.
		_VertexInfos.resize(_NumVerticesAllocated);

		// Fill list of free elements.
		for(uint i=0;i<newResize;i++)
		{
			// create a new entry which points to this vertex.
			// the list is made so allocation is in growing order.
			_VertexFreeMemory.push_back( _NumVerticesAllocated - (i+1) );

			// Mark as free the new vertices. (Debug).
			_VertexInfos[_NumVerticesAllocated - (i+1)].Free= true;
		}
	}

	// get a vertex (pop_back).
	uint	id= _VertexFreeMemory.back();
	// delete this vertex free entry.
	_VertexFreeMemory.pop_back();

	// check and Mark as not free the vertex. (Debug).
	nlassert(id<_NumVerticesAllocated);
	nlassert(_VertexInfos[id].Free);
	_VertexInfos[id].Free= false;




	return id;
}

// ***************************************************************************
void			CLandscapeVBAllocator::deleteVertex(uint vid)
{
	nlassert(vid<_NumVerticesAllocated);
	nlassert(!_VertexInfos[vid].Free);

	if (_UnsynchronizedMode && _Driver)
	{
		// Defer the free until the GPU is done with this vertex.
		// Stamp with counter-1: the vertex was last drawn in the previous frame.
		uint64 counter = _Driver->getSwapBufferCounter();
		uint64 stamp = (counter > 0) ? counter - 1 : 0;
		SDeferredFree df;
		df.VertexId = vid;
		df.FrameCounter = stamp;
		_DeferredFreeVertices.push_back(df);
	}
	else
	{
		// Immediate free.
		_VertexInfos[vid].Free= true;
		_VertexFreeMemory.push_back( vid );
	}
}


// ***************************************************************************
void			CLandscapeVBAllocator::processDeferredFrees(uint64 swapBufferInFlight)
{
	if (_DeferredFreeVertices.empty()) return;

	size_t writeIdx = 0;
	for (size_t i = 0; i < _DeferredFreeVertices.size(); ++i)
	{
		if (_DeferredFreeVertices[i].FrameCounter < swapBufferInFlight)
		{
			// GPU is done with this frame — return vertex to free list.
			_VertexInfos[_DeferredFreeVertices[i].VertexId].Free = true;
			_VertexFreeMemory.push_back(_DeferredFreeVertices[i].VertexId);
		}
		else
		{
			// Still in-flight, keep in deferred queue.
			_DeferredFreeVertices[writeIdx++] = _DeferredFreeVertices[i];
		}
	}
	_DeferredFreeVertices.resize(writeIdx);
}


// ***************************************************************************
void			CLandscapeVBAllocator::forceReallocation()
{
	if (_NumVerticesAllocated > 0)
		allocateVertexBuffer(_NumVerticesAllocated);
}


// ***************************************************************************
void			CLandscapeVBAllocator::lockBuffer(CFarVertexBufferInfo &farVB)
{
	nlassert( _Type==Far0 || _Type==Far1 );

	// force unlock
	unlockBuffer();

	_LastFarVB = &farVB;

	// The VB attribute layout depends on a VP being in use, not on which kind of
	// VP: the GLSL/UBO twin needs exactly the same v[8..11] channels as the nelvp.
	farVB.setupVertexBuffer(_VB, _VertexProgram[0] != nullptr || _VertexProgramUBO[0] != nullptr);

	_BufferLocked= true;
}
// ***************************************************************************
void			CLandscapeVBAllocator::lockBuffer(CNearVertexBufferInfo &tileVB)
{
	nlassert(_Type==Tile);

	// force unlock
	unlockBuffer();

	_LastNearVB = &tileVB;

	tileVB.setupVertexBuffer(_VB, _VertexProgram[0] != nullptr || _VertexProgramUBO[0] != nullptr);

	_BufferLocked= true;
}
// ***************************************************************************
void			CLandscapeVBAllocator::unlockBuffer()
{
	if(_BufferLocked)
	{
		if (_LastFarVB)
			_LastFarVB->setupNullPointers();
		_LastFarVB = nullptr;
		if (_LastNearVB)
			_LastNearVB->setupNullPointers();
		_LastNearVB = nullptr;
		_BufferLocked= false;
	}
}


// ***************************************************************************
// ***************************************************************************
// VertexBuffer mgt.
// ***************************************************************************
// ***************************************************************************


// ***************************************************************************
void			CLandscapeVBAllocator::activate(uint vpId)
{
	nlassert(_Driver);
	nlassert(!_BufferLocked);

	activateVP(vpId);

	_Driver->activeVertexBuffer(_VB);
}


// ***************************************************************************
void			CLandscapeVBAllocator::activateVP(uint vpId)
{
	nlassert(_Driver);

	// A VB can be laid out for one program family only, so the two families must never
	// both be live for the same vpId: setupVertexBuffer (landscapevb_allocator.cpp:263/277)
	// derives the attribute layout from "a program exists", and a half-set allocator would
	// silently pick one. Assert it instead of resolving the ambiguity by priority.
	nlassert(!( _VertexProgram[vpId] && _VertexProgramUBO[vpId] ));

	// The UBO/GLSL twin takes priority when the driver supports UBOs.
	if (isUBOVertexProgram(vpId))
	{
		nlverify(_Driver->activeVertexProgram(_VertexProgramUBO[vpId]));
		return;
	}

	// If enabled, activate Vertex program first.
	if (_VertexProgram[vpId])
	{
		//nlinfo("\nSTARTVP\n%s\nENDVP\n", _VertexProgram[vpId]->getProgram().c_str());
		nlverify(_Driver->activeVertexProgram(_VertexProgram[vpId]));
	}
}


// ***************************************************************************
void				CLandscapeVBAllocator::deleteVertexBuffer()
{
	// must unlock VBhard before.
	unlockBuffer();

	// delete the soft one.
	_VB.deleteAllVertices();
}


// ***************************************************************************
void				CLandscapeVBAllocator::allocateVertexBuffer(uint32 numVertices)
{
	// no allocation must be done if the Driver is not setuped, or if the driver has been deleted by refPtr.
	nlassert(_Driver);

	// allocate() =>_ReallocationOccur= true;
	_ReallocationOccur= true;
	// must unlock VBhard before.
	unlockBuffer();

	// Choose buffer mode based on driver capability.
	if (_UnsynchronizedMode)
		_VB.setBufferUsage(CVertexBuffer::UnsynchronizedWrite, false);
	else
		_VB.setBufferUsage(CVertexBuffer::PartialWrite, false);
	_VB.setNumVertices(numVertices);
	_VB.setName (_VBName);
}


// ***************************************************************************
// ***************************************************************************
// Vertex Program.
// ***************************************************************************
// ***************************************************************************


// ***************************************************************************
/*
	Common Part. Inputs and constants.

	Inputs
	--------
	Standard:
	v[0] == StartPos.	Hence, It is the SplitPoint of the father face.
	v[8] == Tex0 (xy)
	v[9] == Tex1 (xy) (different meanings for Far and Tile).
	v[13] == Tex2 (xy) (just for Tile mode).

	Geomorph:
	v[10] == { GeomFactor, MaxNearLimit }
		* where GeomFactor == max(SizeFaceA, SizeFaceB) * OORefineThreshold.
		It's means vertices are re-computed when the RefineThreshold setup change.

		* MaxNearLimit= max(nearLimitFaceA, nearLimitFaceB)

	v[11].xyz == EndPos-StartPos

	Alpha: NB: Since only useful for Far1, v[12] is not in the VB for Far0 and Tile VertexBuffer.
	v[12] == { TransitionSqrMin, OOTransitionSqrDelta}
		* TransitionSqrMin, OOTransitionSqrDelta : Alpha transition, see preRender().
			There is only 3 values possibles. It depends on Far1 type. Changed in preRender()


	Constant:
	--------
	Setuped at beginning of CLandscape::render()
	c[0..3]= ModelViewProjection Matrix.
	c[4]= {0, 1, 0.5, 0}
	c[5]= RefineCenter
	c[6]= {TileDistFarSqr, OOTileDistDeltaSqr, *, *}
	c[7]= ???
	c[8..11]= ModelView Matrix (for Fog).
	c[12]= PZBModelPosition: landscape center / delta Position to apply before multipliying by mviewMatrix


	Fog Note:
	-----------
	Fog is computed on geomorphed position R1.
	R1.w==1, and suppose that ModelViewMatrix has no Projection Part.
	Then Homogenous-coordinate == Non-Homogenous-coordinate.
	Hence we need only (FogVector*R1).z to get the FogC value.
	=> computed in just on instruction.
*/


// ***********************
/*
	Common start of the program for Far0, Far1 and Tile mode.
	It compute the good Geomorphed position.
	At the end of this program, nothing is written in the output register, and we have in the Temp Registers:

	- R0= scratch
	- R1= CurrentPos geomorphed
	- R2.x= sqrDist= (startPos - RefineCenter).sqrnorm(). Useful for alpha computing.

Pgr Len= 18.
NB: 9 ope for normal errorMetric, and 9 ope for smoothing with TileNear.

	The C code for this Program is:  (v[] means data is a vertex input, c[] means it is a constant)
	{
		// Compute Basic ErrorMetric.
		sqrDist= (v[StartPos] - c[RefineCenter]).sqrnorm()
		ErrorMetric= v[GeomFactor] / sqrDist

		// Compute ErrorMetric modified by TileNear transition.
		f= (c[TileDistFarSqr] - sqrDist) * c[OOTileDistDeltaSqr]
		clamp(f, 0, 1);
		// ^4 gives better smooth result
		f= sqr(f);	f= sqr(f);
		// interpolate the errorMetric
		ErrorMetricModified= v[MaxNearLimit]*f + ErrorMetric*(1-f);

		// Take the max errorMetric.
		ErrorMetric= max(ErrorMetric, ErrorMetricModified);

		// Interpolate StartPos to EndPos, between 1 and 2.
		f= ErrorMetric - 1;
		clamp(f, 0, 1);
		R1= f * v[EndPos-StartPos] + StartPos;
	}

*/

// ***************************************************************************
// Hand-written GLSL landscape vertex program (Tile only), UBO based.
// Mirrors NL3D_LandscapeCommonStartProgram + NL3D_LandscapeTileEndProgram.
//
// Data comes from the user UBO 'NlLandscape' (binding UBBindingVertexProgram) and
// from the builtin NlModel (modelViewProjection) / NlCamera (inverseProjectionBasis)
// UBOs. The nelvp 'o[FOGC]' output is intentionally dropped: the GL3 mega pixel
// program computes fog per fragment from ecPos, and the fog VB varying slot (5) is
// taken by worldPos whenever the PP is built with fogOrPpl.

// UBO format + member offsets, shared by both Tile variants.
NLMISC::CSmartPtr<CUniformBufferFormat> CLandscapeVBAllocator::_UBFormat;
CLandscapeVBAllocator::CLandscapeUBOOffsets CLandscapeVBAllocator::_UBOOffsets;

// GLSL 330 header for the separate-shader-objects path (desktop GL3)
static const char* LandscapeVPCodeGLSL_Header =
	"#version 330\n"
	"#extension GL_ARB_separate_shader_objects : enable\n"
	"out gl_PerVertex { vec4 gl_Position; };\n";

// GLSL 300 es header for the linked program path (GLES3 / WebGL2)
static const char* LandscapeVPCodeGLSL_ES_Header =
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n";

// Linked mode: the mega pixel program declares every possible input, and a
// linked program fails to build if a fragment input has no matching vertex
// output. So declare the full varying set the mega PP can ask for (names
// match g_AttribNames / the VaryingLocation* table), and write only the ones
// this program actually produces. layout() qualifiers are not allowed here.
static const char* LandscapeVPCodeGLSL_VaryingsLinked =
	"smooth out vec4 ecPos;\n"
	"smooth out vec4 vertexColor;\n"
	"smooth out vec4 worldPos;\n"
	"smooth out vec4 weight;\n"
	"smooth out vec4 normal;\n"
	"smooth out vec4 fog;\n"
	"smooth out vec4 paletteSkin;\n"
	"smooth out vec4 tangent;\n"
	"smooth out vec4 texCoord0;\n"
	"smooth out vec4 texCoord1;\n"
	"smooth out vec4 texCoord2;\n"
	"smooth out vec4 texCoord3;\n"
	"smooth out vec4 texCoord4;\n"
	"smooth out vec4 texCoord5;\n"
	"smooth out vec4 texCoord6;\n"
	"smooth out vec4 texCoord7;\n"
	"smooth out vec4 diffuseColor;\n"
	"smooth out vec4 specularColor;\n";

// SSO mode: only the varyings actually written, at the locations the mega PP
// reads them (ecPos=0, texCoord0=8, texCoord1=9).
// Far1 only: the far-tile alpha fade writes o[COL0].w, which is 'diffuseColor'
// (driver_opengl3_nelvp.cpp maps OPrimaryColor -> diffuseColor) and the mega PP
// reads it unconditionally as its starting fragColor (driver_opengl3_pixel_mega_program.cpp:148,
// used at :446). Location 3 = VaryingLocationDiffuseColor = PrimaryColor
// (driver_opengl3_program.h:131). Tile/Far0 leave o[COL0] alone, so they must not
// declare it.
static const char* LandscapeVPCodeGLSL_VaryingsSSO_Far1 =
	"layout(location = 3) smooth out vec4 diffuseColor;\n";

static const char* LandscapeVPCodeGLSL_VaryingsSSO =
	"layout(location = 0) smooth out vec4 ecPos;\n"
	"layout(location = 8) smooth out vec4 texCoord0;\n"
	"layout(location = 9) smooth out vec4 texCoord1;\n";

// Shared body. Two compile-time switches, mirroring the four nelvp end programs:
//   NL_LANDSCAPE_LIGHTMAP - Tex0 comes from v[12] (uv2), as in
//     NL3D_LandscapeTileLightMapEndProgram.
//   NL_LANDSCAPE_FAR1     - adds the far-tile alpha fade of
//     NL3D_LandscapeFar1EndProgram (o[COL0].w), which reads v[12] as AlphaInfo.
// v[12] is the SAME vertex-buffer channel in both cases: NL3D_LANDSCAPE_VPPOS_TEX2
// and NL3D_LANDSCAPE_VPPOS_ALPHAINFO are both CVertexBuffer::TexCoord4
// (landscapevb_allocator.h:47 and :50). What differs is only what the shader
// interprets it as, so the two macros must never be defined together.
static const char* LandscapeVPCodeGLSL_Body =
	"layout(location = 0) in vec4 v0;\n"		// StartPos
	"layout(location = 8) in vec4 v8;\n"		// Tex0
	"layout(location = 9) in vec4 v9;\n"		// Tex1
	"layout(location = 10) in vec4 v10;\n"	// GeomInfo (GeomFactor, MaxNearLimit)
	"layout(location = 11) in vec4 v11;\n"	// DeltaPos (EndPos - StartPos)
	"#if defined(NL_LANDSCAPE_LIGHTMAP) || defined(NL_LANDSCAPE_FAR1)\n"
	"layout(location = 12) in vec4 v12;\n"	// Tex2 (lightmap uv) or AlphaInfo (Far1)
	"#endif\n"
	"\n"
	"void main()\n"
	"{\n"
	"\t// --- Geomorph (same arithmetic as NL3D_LandscapeCommonStartProgram) ---\n"
	"\tvec4 R0 = v0 - vec4(refineCenter, 0.0);\n"
	"\tfloat sqrDist = dot(R0.xyz, R0.xyz);\n"
	"\tfloat errorMetric = v10.x / sqrDist;\n"
	"\tfloat f = clamp((tileDist.x - sqrDist) * tileDist.y, 0.0, 1.0);\n"
	"\tf = f * f * f * f;\n"
	"\tfloat modifiedMetric = f * (v10.y - errorMetric) + errorMetric;\n"
	"\tfloat geomBlend = clamp(max(errorMetric, modifiedMetric) - 1.0, 0.0, 1.0);\n"
	"\tvec4 R1 = v11 * geomBlend - vec4(pzbModelPosition, 0.0);\n"
	"\tR1 = R1 * vec4(1.0, 1.0, 1.0, 0.0) + v0;\n"
	"\t// --- Projection + uv copy ---\n"
	"\tgl_Position = modelViewProjection * R1;\n"
	"#ifdef NL_LANDSCAPE_LIGHTMAP\n"
	"\ttexCoord0 = v12;\n"
	"#else\n"
	"\ttexCoord0 = v8;\n"
	"#endif\n"
	"\ttexCoord1 = v9;\n"
	"#ifdef NL_LANDSCAPE_FAR1\n"
	"\t// --- far-tile alpha fade, the extra term of NL3D_LandscapeFar1EndProgram ---\n"
	"\t// nelvp: ADD R0.x, R2.x, -v[12].x / MUL R0.x, R0.x, v[12].y / MAX c[4].x / MIN o[COL0].w c[4].y\n"
	"\t// = clamp((sqrDist - TransitionSqrMin) * OOTransitionSqrDelta, 0, 1), c[4] = {0,1,..}, R2.x = sqrDist.\n"
	"\t// Only .w is written and then the whole colour is clamped: that is exactly what the converter\n"
	"\t// emits for this program, and o[COL0].rgb stays undefined in the nelvp too, so mirroring it is\n"
	"\t// what keeps the A/B pixel-exact.\n"
	"\tdiffuseColor.w = clamp((sqrDist - v12.x) * v12.y, 0.0, 1.0);\n"
	"\tdiffuseColor = clamp(diffuseColor, 0.0, 1.0);\n"
	"#endif\n"
	"\t// NeL-space eye position for the builtin PP fog (same epilogue as the\n"
	"\t// nelvp converter: inv(Projection * ChangeBasis) * clip position).\n"
	"\tecPos = inverseProjectionBasis * gl_Position;\n"
	"}\n";

const char* NL3D_LandscapeCommonStartProgram=
"!!VP1.0																				\n\
	# compute Basic geomorph into R0.x													\n\
	ADD	R0, v[0], -c[5];			# R0 = startPos - RefineCenter						\n\
	DP3	R2.x, R0, R0;				# R2.x= sqrDist= (startPos - RefineCenter).sqrnorm()\n\
	RCP	R0.x, R2.x;					# R0.x= 1 / sqrDist									\n\
	MUL	R0.x, v[10].x, R0.x;		# R0.x= ErrorMetric= GeomFactor / sqrDist			\n\
																						\n\
	# compute Transition Factor To TileNear Geomorph, into R0.z							\n\
	ADD	R0.z, c[6].x, -R2.x;		# R0.z= TileDistFarSqr - sqrDist					\n\
	MUL	R0.z, R0.z, c[6].y;			# R0.z= f= (TileDistFarSqr - sqrDist ) * OOTileDistDeltaSqr	\n\
	MAX R0.z, R0.z, c[4].x;																\n\
	MIN R0.z, R0.z, c[4].y;			# R0.z= f= clamp(f, 0, 1);							\n\
	MUL R0.z, R0.z, R0.z;																\n\
	MUL R0.z, R0.z, R0.z;			# R0.z= finalFactor= f^4							\n\
																						\n\
	# Apply the transition factor to the ErrorMetric => R0.w= ErrorMetricModified.		\n\
	ADD	R0.w, v[10].y, -R0.x;		# R0.w= maxNearLimit - ErrorMetric					\n\
	MAD	R0.w, R0.z, R0.w, R0.x;	# R0.w= finalFactor * (maxNearLimit - ErrorMetric) + ErrorMetric \n\
																						\n\
	# R0.w may be < R0.x; (when the point is very near). Must take the bigger errorMetric.	\n\
	MAX	R0.x, R0.x, R0.w;			# R0.x= ErrorMetric Max								\n\
																						\n\
	# apply geomorph into R1															\n\
	ADD R0.x, R0.x, -c[4].y;		# R0.x= ErrorMetric Max - 1							\n\
	MAX R0.x, R0.x, c[4].x;																\n\
	MIN R0.x, R0.x, c[4].y;			# R0.x= geomBlend= clamp(R0.x, 0, 1);				\n\
																						\n\
	# NB: Can't use MAD R1.xyz, v[11], R0.x, v[0], because can't acces 2 inputs in one op.	\n\
	# Hence, can use a MAD to Sub the Landscape Center _PZBModelPosition				\n\
	# write to R1.w is useless (but needed to avoid a read error when multiplied by 0)  \n\
	# in the next instruction															\n\
	MAD	R1.xyzw, v[11], R0.x, -c[12];													\n\
	# set w to 1 by using c[4] = { 0, 1, 0.5, 0}										\n\
	MAD	R1, R1, c[4].yyyx, v[0];	# R1= geomBlend * (EndPos-StartPos) + StartPos		\n\
";


// ***********************
// Test Speed.
/*"!!VP1.0																				\n\
	# compute Basic geomorph into R0.x													\n\
	ADD	R0, v[0], -c[5];			# R0 = startPos - RefineCenter						\n\
	DP3	R2.x, R0, R0;				# R2.x= sqrDist= (startPos - RefineCenter).sqrnorm()\n\
	MOV	R1, v[0];					# R1= geomBlend * (EndPos-StartPos) + StartPos		\n\
";
*/
const string NL3D_LandscapeTestSpeedProgram=
"	MOV R1, R1; \n	MOV R1, R1; \n	MOV R1, R1; \n	MOV R1, R1; \n	MOV R1, R1; \n\
	MOV R1, R1; \n	MOV R1, R1; \n	MOV R1, R1; \n	MOV R1, R1; \n	MOV R1, R1; \n\
";


// ***********************
/*
	Far0:
		just project, copy uv0 and uv1
		NB: leave o[COL0] undefined because the material don't care diffuse RGBA here
*/
// ***********************
const char* NL3D_LandscapeFar0EndProgram=
"	# compute in Projection space														\n\
	DP4 o[HPOS].x, c[0], R1;															\n\
	DP4 o[HPOS].y, c[1], R1;															\n\
	DP4 o[HPOS].z, c[2], R1;															\n\
	DP4 o[HPOS].w, c[3], R1;															\n\
	MOV o[TEX0], v[8];																\n\
	MOV o[TEX1], v[9];																\n\
	DP4	o[FOGC].x, c[10], R1;															\n\
	END																					\n\
";


// ***********************
/*
	Far1:
		Compute Alpha transition.
		Project, copy uv0 and uv1,
		NB: leave o[COL0] RGB undefined because the material don't care diffuse RGB
*/
// ***********************
const char* NL3D_LandscapeFar1EndProgram=
"	# compute Alpha Transition															\n\
	ADD R0.x, R2.x, -v[12].x;		# R0.x= sqrDist-TransitionSqrMin					\n\
	MUL	R0.x, R0.x, v[12].y;		# R0.x= (sqrDist-TransitionSqrMin) * OOTransitionSqrDelta	\n\
	MAX R0.x, R0.x, c[4].x;																\n\
	MIN o[COL0].w, R0.x, c[4].y;	# col.A= clamp(R0.x, 0, 1);							\n\
																						\n\
	# compute in Projection space														\n\
	DP4 o[HPOS].x, c[0], R1;															\n\
	DP4 o[HPOS].y, c[1], R1;															\n\
	DP4 o[HPOS].z, c[2], R1;															\n\
	DP4 o[HPOS].w, c[3], R1;															\n\
	MOV o[TEX0], v[8];																\n\
	MOV o[TEX1], v[9];																\n\
	DP4	o[FOGC].x, c[10], R1;															\n\
	END																					\n\
";


// ***********************
/*
	Tile:
		just project, copy uv0, uv1.
		NB: leave o[COL0] undefined because the material don't care diffuse RGBA here
*/
// ***********************
const char* NL3D_LandscapeTileEndProgram=
"	# compute in Projection space														\n\
	DP4 o[HPOS].x, c[0], R1;															\n\
	DP4 o[HPOS].y, c[1], R1;															\n\
	DP4 o[HPOS].z, c[2], R1;															\n\
	DP4 o[HPOS].w, c[3], R1;															\n\
	MOV o[TEX0], v[8];																	\n\
	MOV o[TEX1], v[9];																	\n\
	DP4	o[FOGC].x, c[10], R1;															\n\
	END																					\n\
";

/// Same version but write Tex0 to take uv2, ie v[13], for lightmap pass
const char* NL3D_LandscapeTileLightMapEndProgram=
"	# compute in Projection space														\n\
	DP4 o[HPOS].x, c[0], R1;															\n\
	DP4 o[HPOS].y, c[1], R1;															\n\
	DP4 o[HPOS].z, c[2], R1;															\n\
	DP4 o[HPOS].w, c[3], R1;															\n\
	MOV o[TEX0], v[12];																\n\
	MOV o[TEX1], v[9];																\n\
	DP4	o[FOGC].x, c[10], R1;															\n\
	END																					\n\
";


// ***************************************************************************
// Build the hand-written GLSL/UBO landscape programs for this allocator, instead of
// the nelvp ones. Used by all three types (Far0, Far1, Tile) once the driver can
// take a user UBO; 'withLightMapVariant' is true only for Tile, the only type with a
// second pass (uv2 lightmap). The nelvp programs are deliberately NOT built here:
// building both would leave the two families ambiguous for activateVP.
void			CLandscapeVBAllocator::setupUBOPrograms(bool withLightMapVariant)
{
	_VertexProgramUBO[0] = new CVertexProgramLandscapeUBO(_Type, false);
	if (withLightMapVariant)
		_VertexProgramUBO[1] = new CVertexProgramLandscapeUBO(_Type, true);

	// Deliberately not nlverify: supportBuiltinUBO() is an unconditional 'true' for the
	// whole GL3 driver (driver_opengl3.h:1497), so a driver that accepts UBOs can still
	// reject THIS source - a strict ES 3.0 / core-profile / WebGL2 compiler is enough. A
	// hard assert there would turn "this GPU dislikes my GLSL" into a crash, which is
	// worse than the pre-D.1a behaviour. Fall back to the nelvp path for this allocator
	// instead, the way CMeshVPWindTree retries after a UBO compile failure
	// (meshvp_wind_tree.cpp:1066-1071).
	bool ok = _Driver->compileVertexProgram(_VertexProgramUBO[0]);
	if (ok && withLightMapVariant)
		ok = _Driver->compileVertexProgram(_VertexProgramUBO[1]);

	if (!ok)
	{
		nlwarning("GL3: landscape %s UBO/GLSL program failed to compile, falling back to nelvp for this allocator",
			(_Type == CLandscapeVBAllocator::Far0) ? "Far0" :
			(_Type == CLandscapeVBAllocator::Far1) ? "Far1" : "Tile");

		_VertexProgramUBO[0] = 0;
		_VertexProgramUBO[1] = 0;
		_VertexProgram[0] = new CVertexProgramLandscape(_Type);
		if (withLightMapVariant)
			_VertexProgram[1] = new CVertexProgramLandscape(_Type, true);

		// The nelvp path is what every driver used before this work, so if even that
		// fails there is nothing left to try: keep the original hard failure.
		nlverify(_Driver->compileVertexProgram(_VertexProgram[0]));
		if (withLightMapVariant)
			nlverify(_Driver->compileVertexProgram(_VertexProgram[1]));
		return;
	}

	// One UBO per allocator, shared by that allocator's variants: the values it carries
	// (refineCenter, tileDist, pzbModelPosition) are identical for all three types
	// (landscape.cpp computes them once per frame), but each allocator binds its own
	// before its own draw, so no pass can read another pass's buffer.
	if (!_LandscapeUB)
	{
		_LandscapeUB = new CUniformBuffer();
		_LandscapeUB->Format = *_UBFormat;
	}
}

// ***************************************************************************
void			CLandscapeVBAllocator::deleteVertexProgram()
{
	for (uint i = 0; i < MaxVertexProgram; ++i)
	{
		if (_VertexProgram[i])
		{
			_VertexProgram[i] = nullptr; // smartptr
		}
		if (_VertexProgramUBO[i])
		{
			_VertexProgramUBO[i] = nullptr; // smartptr
		}
	}
	_LandscapeUB = nullptr;
}


// ***************************************************************************
void			CLandscapeVBAllocator::setupVBFormatAndVertexProgram(bool withVertexProgram)
{
	// If not vertexProgram mode
	if(!withVertexProgram)
	{
		// setup normal VB format.
		if(_Type==Far0)
			// v3f/t2f0/t2f1
			_VB.setVertexFormat(CVertexBuffer::PositionFlag | CVertexBuffer::TexCoord0Flag | CVertexBuffer::TexCoord1Flag);
		else if(_Type==Far1)
			// v3f/t2f/t2f1/c4ub
			_VB.setVertexFormat(CVertexBuffer::PositionFlag | CVertexBuffer::TexCoord0Flag | CVertexBuffer::TexCoord1Flag | CVertexBuffer::PrimaryColorFlag );
		else
			// v3f/t2f0/t2f1/t2f2
			_VB.setVertexFormat(CVertexBuffer::PositionFlag | CVertexBuffer::TexCoord0Flag | CVertexBuffer::TexCoord1Flag | CVertexBuffer::TexCoord2Flag);
	}
	else
	{
		// Else Setup our Vertex Program, and good VBuffers, according to _Type.

		if(_Type==Far0)
		{
			// Build the Vertex Format.
			_VB.clearValueEx();
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_STARTPOS,	CVertexBuffer::Float3);	// v[0]= StartPos.
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_TEX0,		CVertexBuffer::Float2);	// v[8]= Tex0.
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_TEX1,		CVertexBuffer::Float2);	// v[9]= Tex1.
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_GEOMINFO,	CVertexBuffer::Float2);	// v[10]= GeomInfos.
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_DELTAPOS,	CVertexBuffer::Float3);	// v[11]= EndPos-StartPos.
			_VB.initEx();

			// Init the Vertex Program.
			if (_Driver->supportBuiltinUBO())
				setupUBOPrograms(false);
			else
			{
				_VertexProgram[0] = new CVertexProgramLandscape(Far0);
				nlverify(_Driver->compileVertexProgram(_VertexProgram[0]));
			}
		}
		else if(_Type==Far1)
		{
			// Build the Vertex Format.
			_VB.clearValueEx();
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_STARTPOS,	CVertexBuffer::Float3);	// v[0]= StartPos.
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_TEX0,		CVertexBuffer::Float2);	// v[8]= Tex0.
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_TEX1,		CVertexBuffer::Float2);	// v[9]= Tex1.
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_GEOMINFO,	CVertexBuffer::Float2);	// v[10]= GeomInfos.
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_DELTAPOS,	CVertexBuffer::Float3);	// v[11]= EndPos-StartPos.
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_ALPHAINFO,	CVertexBuffer::Float2);	// v[12]= AlphaInfos.
			_VB.initEx();

			// Init the Vertex Program.
			if (_Driver->supportBuiltinUBO())
				setupUBOPrograms(false);
			else
			{
				_VertexProgram[0] = new CVertexProgramLandscape(Far1);
				nlverify(_Driver->compileVertexProgram(_VertexProgram[0]));
			}
		}
		else
		{
			// Build the Vertex Format.
			_VB.clearValueEx();
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_STARTPOS,	CVertexBuffer::Float3);	// v[0]= StartPos.
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_TEX0,		CVertexBuffer::Float2);	// v[8]= Tex0.
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_TEX1,		CVertexBuffer::Float2);	// v[9]= Tex1.
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_TEX2,		CVertexBuffer::Float2);	// v[12]= Tex2.
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_GEOMINFO,	CVertexBuffer::Float2);	// v[10]= GeomInfos.
			_VB.addValueEx(NL3D_LANDSCAPE_VPPOS_DELTAPOS,	CVertexBuffer::Float3);	// v[11]= EndPos-StartPos.
			_VB.initEx();

			// Init the Vertex Program.
			if (_Driver->supportBuiltinUBO())
				setupUBOPrograms(true);
			else
			{
				_VertexProgram[0] = new CVertexProgramLandscape(Tile, false);
				nlverify(_Driver->compileVertexProgram(_VertexProgram[0]));

				// Init the Vertex Program for lightmap pass
				_VertexProgram[1] = new CVertexProgramLandscape(Tile, true);
				nlverify(_Driver->compileVertexProgram(_VertexProgram[1]));
			}
		}
	}
}


CVertexProgramLandscape::CVertexProgramLandscape(CLandscapeVBAllocator::TType type, bool lightMap)
{
	// nelvp
	{
		CSource *source = new CSource();
		source->Profile = nelvp;
		source->DisplayName = "Landscape/nelvp";
		switch (type)
		{
		case CLandscapeVBAllocator::Far0:
			source->DisplayName += "/far0";
			source->setSource(std::string(NL3D_LandscapeCommonStartProgram) 
				+ std::string(NL3D_LandscapeFar0EndProgram));
			break;
		case CLandscapeVBAllocator::Far1:
			source->DisplayName += "/far1";
			source->setSource(std::string(NL3D_LandscapeCommonStartProgram) 
				+ std::string(NL3D_LandscapeFar1EndProgram));
			break;
		case CLandscapeVBAllocator::Tile:
			source->DisplayName += "/tile";
			if (lightMap)
			{
				source->DisplayName += "/lightmap";
				source->setSource(std::string(NL3D_LandscapeCommonStartProgram) 
					+ std::string(NL3D_LandscapeTileLightMapEndProgram));
			}
			else
			{
				source->setSource(std::string(NL3D_LandscapeCommonStartProgram) 
					+ std::string(NL3D_LandscapeTileEndProgram));
			}
			break;
		}
		source->ParamIndices["modelViewProjection"] = 0;
		source->ParamIndices["programConstants0"] = 4;
		source->ParamIndices["refineCenter"] = 5;
		source->ParamIndices["tileDist"] = 6;
		source->ParamIndices["fog"] = 10;
		source->ParamIndices["pzbModelPosition"] = 12;
		addSource(source);
	}
	// (nothing here: the hand-written GLSL twin of this program is
	// CVertexProgramLandscapeUBO, built by CLandscapeVBAllocator::setupUBOPrograms for
	// all three types. This class stays the nelvp path for drivers without UBO support.)
}

// ***************************************************************************
// UBO based GLSL landscape vertex program (Tile only).
const CLandscapeVBAllocator::CLandscapeUBOOffsets &CLandscapeVBAllocator::uboOffsets()
{
	return _UBOOffsets;
}

CVertexProgramLandscapeUBO::CVertexProgramLandscapeUBO(CLandscapeVBAllocator::TType type, bool lightMap)
{
	// The three landscape programs differ only in their END block, so one body +
	// compile-time switches covers all of them (see LandscapeVPCodeGLSL_Body above).
	// lightMap is only ever true for Tile, which is the only type with a lightmap pass.
	const bool far1 = (type == CLandscapeVBAllocator::Far1);

	// Build the UBO format once, shared by every variant of every type. All three
	// programs read the same three values: landscape.cpp:1265-1275 passes identical
	// refineCenter / TileDistFarSqr / _PZBModelPosition to Far0, Far1 and Tile, and
	// Far1's extra data (AlphaInfo) is a vertex attribute, not a uniform - so no
	// extra UBO member is needed.
	if (!CLandscapeVBAllocator::_UBFormat)
	{
		CUniformBufferFormat *fmt = new CUniformBufferFormat();
		fmt->Name = "NlLandscape";
		CLandscapeVBAllocator::_UBOOffsets.RefineCenter     = fmt->push("refineCenter", CUniformBufferFormat::FloatVec3);
		CLandscapeVBAllocator::_UBOOffsets.TileDist         = fmt->push("tileDist", CUniformBufferFormat::FloatVec2);
		CLandscapeVBAllocator::_UBOOffsets.PZBModelPosition = fmt->push("pzbModelPosition", CUniformBufferFormat::FloatVec3);
		CLandscapeVBAllocator::_UBFormat = fmt;
	}

	std::string defines;
	if (lightMap)
		defines += "#define NL_LANDSCAPE_LIGHTMAP\n";
	if (far1)
		defines += "#define NL_LANDSCAPE_FAR1\n";

	const std::string typeName =
		(type == CLandscapeVBAllocator::Far0) ? "far0" :
		(type == CLandscapeVBAllocator::Far1) ? "far1" : "tile";

	// v[12] (VB channel TexCoord4) is only read by the Tile-lightmap and Far1
	// variants, and g_VertexFlags maps attribute 12 -> TexCoord4Flag
	// (driver_opengl3_program.cpp:83-100), so VPVertexFormat must match exactly what
	// the converter would have computed from the program's used inputs.
	uint16 vpVertexFormat =
		CVertexBuffer::PositionFlag | CVertexBuffer::TexCoord0Flag | CVertexBuffer::TexCoord1Flag |
		CVertexBuffer::TexCoord2Flag | CVertexBuffer::TexCoord3Flag;
	if (lightMap || far1)
		vpVertexFormat |= CVertexBuffer::TexCoord4Flag;

	// Linked variant (GLES3 / WebGL2): no layout() on outputs, full varying set.
	{
		CSource *source = new CSource();
		source->DisplayName = std::string("glsl300esv/Landscape/") + typeName +
			(lightMap ? "/ubo/lightmap" : "/ubo");
		source->Profile = CVertexProgram::glsl300esv;
		source->Features.OnlyUBOs = true;
		source->Features.UsesCameraUBO = true; // ecPos epilogue reads inverseProjectionBasis
		source->Features.UsesObjectUBO = true; // modelViewProjection
		source->Features.VPVertexFormat = vpVertexFormat;
		source->UniformBufferFormats[UBBindingVertexProgram] = CLandscapeVBAllocator::_UBFormat;
		source->setSource(
			std::string(LandscapeVPCodeGLSL_ES_Header) + defines +
			std::string(LandscapeVPCodeGLSL_VaryingsLinked) +
			std::string(LandscapeVPCodeGLSL_Body));
		addSource(source);
	}

	// SSO variant (desktop GL3): layout() on outputs, only the written varyings.
	{
		CSource *source = new CSource();
		source->DisplayName = std::string("glsl330v/Landscape/") + typeName +
			(lightMap ? "/ubo/lightmap" : "/ubo");
		source->Profile = CVertexProgram::glsl330v;
		source->Features.OnlyUBOs = true;
		source->Features.UsesCameraUBO = true;
		source->Features.UsesObjectUBO = true;
		source->Features.VPVertexFormat = vpVertexFormat;
		source->UniformBufferFormats[UBBindingVertexProgram] = CLandscapeVBAllocator::_UBFormat;
		source->setSource(
			std::string(LandscapeVPCodeGLSL_Header) + defines +
			std::string(LandscapeVPCodeGLSL_VaryingsSSO) +
			(far1 ? std::string(LandscapeVPCodeGLSL_VaryingsSSO_Far1) : std::string()) +
			std::string(LandscapeVPCodeGLSL_Body));
		addSource(source);
	}
}

void CVertexProgramLandscapeUBO::buildInfo()
{
	// All the landscape VP data is in the NlLandscape UBO: no individual uniform
	// index exists, callers must use the UBO (see CLandscape::renderEx).
	m_Idx.ProgramConstants0 = ~0;
	m_Idx.RefineCenter = ~0;
	m_Idx.TileDist = ~0;
	m_Idx.PZBModelPosition = ~0;
}

void CVertexProgramLandscape::buildInfo()
{
	m_Idx.ProgramConstants0 = getUniformIndex("programConstants0");
	nlassert(m_Idx.ProgramConstants0 != std::numeric_limits<uint>::max());
	m_Idx.RefineCenter = getUniformIndex("refineCenter");
	nlassert(m_Idx.RefineCenter != std::numeric_limits<uint>::max());
	m_Idx.TileDist = getUniformIndex("tileDist");
	nlassert(m_Idx.TileDist != std::numeric_limits<uint>::max());
	m_Idx.PZBModelPosition = getUniformIndex("pzbModelPosition");
	nlassert(m_Idx.PZBModelPosition != std::numeric_limits<uint>::max());
}

} // NL3D
