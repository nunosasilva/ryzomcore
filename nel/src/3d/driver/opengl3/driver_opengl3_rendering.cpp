// NeL - MMORPG Framework <http://dev.ryzom.com/projects/nel/>
// Copyright (C) 2010  Winch Gate Property Limited
//
// This source file has been modified by the following contributors:
// Copyright (C) 2013  Laszlo KIS-ADAM (dfighter) <dfighter1985@gmail.com>
// Copyright (C) 2014-2015  Jan BOON (Kaetemi) <jan.boon@kaetemi.be>
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

#include "stdopengl3.h"

#include "driver_opengl3.h"
#include "nel/3d/index_buffer.h"
#include "driver_opengl3_vertex_buffer.h"

namespace NL3D {
namespace NLDRIVERGL3 {

#ifdef USE_OPENGLES3
// WebGL 2.0 does not support client-side index arrays.
// Upload index data to a scratch element buffer before calling glDrawElements.
void CDriverGL3::drawElementsWebGL(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	GLsizeiptr indexSize = (type == GL_UNSIGNED_SHORT) ? sizeof(uint16) : sizeof(uint32);
	GLsizeiptr dataSize = count * indexSize;

	nglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _ScratchElementBuffer);
	if (dataSize > _ScratchElementBufferSize)
	{
		nglBufferData(GL_ELEMENT_ARRAY_BUFFER, dataSize, indices, GL_STREAM_DRAW);
		_ScratchElementBufferSize = dataSize;
	}
	else
	{
		nglBufferData(GL_ELEMENT_ARRAY_BUFFER, dataSize, indices, GL_STREAM_DRAW);
	}
	glDrawElements(mode, count, type, (const void *)0);
	nglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}
#endif

// ***************************************************************************

bool CDriverGL3::renderLines(CMaterial& mat, uint32 firstIndex, uint32 nlines)
{
	H_AUTO_OGL(CDriverGL3_renderLines)

	// setup material
	if (!setupMaterial(mat) || _LastIB._Values == nullptr)
		return false;

	if (_CurrentVertexBufferGL && _CurrentVertexBufferGL->isInvalid()) return true;
	// render primitives.
	//==============================
	// start multipass.
	uint	nPass;
	nPass= beginMultiPass();
	// draw all passes.
	for (uint pass=0;pass<nPass; pass++)
	{
		// setup the pass.
		if (!setupPass(pass))
			return false;

		// draw the primitives.
		if (nlines)
		{
			if (_LastIB._Format == CIndexBuffer::Indices16)
			{
#ifdef USE_OPENGLES3
				drawElementsWebGL(GL_LINES,2*nlines,GL_UNSIGNED_SHORT,((uint16 *) _LastIB._Values)+firstIndex);
#else
				glDrawElements(GL_LINES,2*nlines,GL_UNSIGNED_SHORT,((uint16 *) _LastIB._Values)+firstIndex);
#endif
			}
			else
			{
				nlassert(_LastIB._Format == CIndexBuffer::Indices32);
#ifdef USE_OPENGLES3
				drawElementsWebGL(GL_LINES,2*nlines,GL_UNSIGNED_INT,((uint32 *) _LastIB._Values)+firstIndex);
#else
				glDrawElements(GL_LINES,2*nlines,GL_UNSIGNED_INT,((uint32 *) _LastIB._Values)+firstIndex);
#endif
			}
		}
	}
	// end multipass.
	endMultiPass();

	// Profiling.
	_PrimitiveProfileIn.NLines+= nlines;
	_PrimitiveProfileOut.NLines+= nlines;

	if (_CurrentVertexBufferGL)
		_CurrentVertexBufferGL->setFrameInFlight(_SwapBufferCounter);

	return true;
}

// ***************************************************************************

bool CDriverGL3::renderTriangles(CMaterial& mat, uint32 firstIndex, uint32 ntris)
{
	H_AUTO_OGL(CDriverGL3_renderTriangles);

	// setup material
	if (!setupMaterial(mat) || _LastIB._Values == nullptr)
		return false;

	if (_CurrentVertexBufferGL && _CurrentVertexBufferGL->isInvalid()) return true;

	// render primitives.
	//==============================
	// start multipass.
	uint	nPass;

	nPass= beginMultiPass();

	// draw all passes.
	for (uint pass=0;pass<nPass; pass++)
	{
		// setup the pass.
		if (!setupPass(pass))
			return false;

		// draw the primitives.
		if (ntris)
		{
			if (_LastIB._Format == CIndexBuffer::Indices16)
			{
#ifdef USE_OPENGLES3
				drawElementsWebGL(GL_TRIANGLES,3*ntris,GL_UNSIGNED_SHORT, ((uint16 *) _LastIB._Values)+firstIndex);
#else
				glDrawElements(GL_TRIANGLES,3*ntris,GL_UNSIGNED_SHORT, ((uint16 *) _LastIB._Values)+firstIndex);
#endif
			}
			else
			{
				nlassert(_LastIB._Format == CIndexBuffer::Indices32);
#ifdef USE_OPENGLES3
				drawElementsWebGL(GL_TRIANGLES,3*ntris,GL_UNSIGNED_INT, ((uint32 *) _LastIB._Values)+firstIndex);
#else
				glDrawElements(GL_TRIANGLES,3*ntris,GL_UNSIGNED_INT, ((uint32 *) _LastIB._Values)+firstIndex);
#endif
			}
		}
	}
	// end multipass.
	endMultiPass();


	// Profiling.
	_PrimitiveProfileIn.NTriangles+= ntris;
	_PrimitiveProfileOut.NTriangles+= ntris * nPass;

	if (_CurrentVertexBufferGL)
		_CurrentVertexBufferGL->setFrameInFlight(_SwapBufferCounter);

	return true;
}

// ***************************************************************************

bool CDriverGL3::renderSimpleTriangles(uint32 firstTri, uint32 ntris)
{
	H_AUTO_OGL(CDriverGL3_renderSimpleTriangles);

	setupBuiltinPrograms();

	nlassert(ntris>0);

	if (_CurrentVertexBufferGL && _CurrentVertexBufferGL->isInvalid()) return true;
	// Don't setup any material here.

	// render primitives.
	//==============================
	// NO MULTIPASS HERE!!
	// draw the primitives. (nb: ntris>0).

	if (_LastIB._Format == CIndexBuffer::Indices16)
	{
#ifdef USE_OPENGLES3
		drawElementsWebGL(GL_TRIANGLES,3*ntris,GL_UNSIGNED_SHORT, ((uint16 *) _LastIB._Values)+firstTri);
#else
		glDrawElements(GL_TRIANGLES,3*ntris,GL_UNSIGNED_SHORT, ((uint16 *) _LastIB._Values)+firstTri);
#endif
	}
	else
	{
		nlassert(_LastIB._Format == CIndexBuffer::Indices32);
#ifdef USE_OPENGLES3
		drawElementsWebGL(GL_TRIANGLES,3*ntris,GL_UNSIGNED_INT, ((uint32 *) _LastIB._Values)+firstTri);
#else
		glDrawElements(GL_TRIANGLES,3*ntris,GL_UNSIGNED_INT, ((uint32 *) _LastIB._Values)+firstTri);
#endif
	}

	// Profiling.
	_PrimitiveProfileIn.NTriangles+= ntris;
	_PrimitiveProfileOut.NTriangles+= ntris;

	if (_CurrentVertexBufferGL)
		_CurrentVertexBufferGL->setFrameInFlight(_SwapBufferCounter);

	return true;
}

// ***************************************************************************

bool CDriverGL3::renderRawPoints(CMaterial& mat, uint32 startIndex, uint32 numPoints)
{
	H_AUTO_OGL(CDriverGL3_renderRawPoints)

	// setup material
	if (!setupMaterial(mat))
		return false;

	if (_CurrentVertexBufferGL && _CurrentVertexBufferGL->isInvalid()) return true;
	// render primitives.
	//==============================
	// start multipass.
	uint	nPass;
	nPass= beginMultiPass();
	// draw all passes.
	for (uint pass=0;pass<nPass; pass++)
	{
		// setup the pass.
		if (!setupPass(pass))
			return false;

		// draw the primitives.
		if (numPoints)
			glDrawArrays(GL_POINTS, startIndex, numPoints);
	}
	// end multipass.
	endMultiPass();


	// Profiling.
	_PrimitiveProfileIn.NPoints+= numPoints;
	_PrimitiveProfileOut.NPoints+= numPoints * nPass;

	if (_CurrentVertexBufferGL)
		_CurrentVertexBufferGL->setFrameInFlight(_SwapBufferCounter);

	return true;
}

// ***************************************************************************

bool CDriverGL3::renderRawLines(CMaterial& mat, uint32 startIndex, uint32 numLines)
{
	H_AUTO_OGL(CDriverGL3_renderRawLines)

	// setup material
	if (!setupMaterial(mat))
		return false;

	if (_CurrentVertexBufferGL && _CurrentVertexBufferGL->isInvalid()) return true;
	// render primitives.
	//==============================
	// start multipass.
	uint	nPass;
	nPass= beginMultiPass();
	// draw all passes.
	for (uint pass=0;pass<nPass; pass++)
	{
		// setup the pass.
		if (!setupPass(pass))
			return false;

		// draw the primitives.
		if (numLines)
			glDrawArrays(GL_LINES, startIndex << 1, numLines << 1);
	}
	// end multipass.
	endMultiPass();


	// Profiling.
	_PrimitiveProfileIn.NLines  += numLines ;
	_PrimitiveProfileOut.NLines += numLines  * nPass;

	if (_CurrentVertexBufferGL)
		_CurrentVertexBufferGL->setFrameInFlight(_SwapBufferCounter);

	return true;
}

// ***************************************************************************

bool CDriverGL3::renderRawTriangles(CMaterial& mat, uint32 startIndex, uint32 numTris)
{
	H_AUTO_OGL(CDriverGL3_renderRawTriangles)

	// setup material
	if (!setupMaterial(mat))
		return false;

	if (_CurrentVertexBufferGL && _CurrentVertexBufferGL->isInvalid()) return true;
	// render primitives.
	//==============================
	// start multipass.
	uint	nPass;
	nPass= beginMultiPass();
	// draw all passes.
	for (uint pass=0;pass<nPass; pass++)
	{
		// setup the pass.
		if (!setupPass(pass))
			return false;

		// draw the primitives.
		if (numTris)
		{
			{
				static uint s_r=0;
				if (s_r++ < 4)
				{
					// Where does the first vertex of this batch land in clip space?
					// Same expression the object UBO uploads (driver_opengl3_render_state.cpp:463),
					// column-vector convention as the shaders use it: clip = M * v.
					const CMatrix mvp = _GLProjMat * _ChangeBasis * _ModelViewMatrix;
					const float *c = mvp.get();
					const float vx = -6.f, vy = -6.f, vz = 0.f;
					const float cx = c[0]*vx + c[4]*vy + c[8]*vz + c[12];
					const float cy = c[1]*vx + c[5]*vy + c[9]*vz + c[13];
					const float cz = c[2]*vx + c[6]*vy + c[10]*vz + c[14];
					const float cw = c[3]*vx + c[7]*vy + c[11]*vz + c[15];
					const float *pm = _GLProjMat.get();
					const float *mm = _ModelViewMatrix.get();
					nlinfo("DECALPROBE glDrawArrays start=%u ntris=%u projRow0=(%.2f %.2f %.2f %.2f) mvRow0=(%.2f %.2f %.2f %.2f) clip(-6,-6,0)=(%.2f %.2f %.2f %.2f) glErr=0x%x",
						(uint)(startIndex*3), (uint)(numTris*3),
						pm[0], pm[1], pm[2], pm[3], mm[0], mm[1], mm[2], mm[3],
						cx, cy, cz, cw, (uint)glGetError());
				}
			}
			glDrawArrays(GL_TRIANGLES, startIndex*3, numTris*3);
		}
	}
	// end multipass.
	endMultiPass();


	// Profiling.
	_PrimitiveProfileIn.NTriangles  += numTris ;
	_PrimitiveProfileOut.NTriangles += numTris  * nPass;

	if (_CurrentVertexBufferGL)
		_CurrentVertexBufferGL->setFrameInFlight(_SwapBufferCounter);

	return true;
}

// ***************************************************************************

bool CDriverGL3::renderRawQuads(CMaterial& mat, uint32 startIndex, uint32 numQuads)
{
	H_AUTO_OGL(CDriverGL3_renderRawQuads)
	if (!numQuads) return true;

	// setup material
	if (!setupMaterial(mat))
		return false;

	if (_CurrentVertexBufferGL && _CurrentVertexBufferGL->isInvalid()) return true;

	const uint32 QUAD_BATCH_SIZE = 2048;
	static GLshort defaultIndices[QUAD_BATCH_SIZE * 6];
	static bool	init = false;
	if (!init)
	{
		// setup the base index buffer
		for (uint k = 0; k < QUAD_BATCH_SIZE; ++k)
		{
			// first tri
			defaultIndices[k * 6] = (GLshort) (k * 4);
			defaultIndices[k * 6 + 1] = (GLshort) (k * 4 + 1);
			defaultIndices[k * 6 + 2] = (GLshort) (k * 4 + 2);
			// second tri
			defaultIndices[k * 6 + 3] = (GLshort) (k * 4);
			defaultIndices[k * 6 + 4] = (GLshort) (k * 4 + 2);
			defaultIndices[k * 6 + 5] = (GLshort) (k * 4 + 3);

		}
		init = true;
	}

	// render primitives.
	//==============================
	// start multipass.
	uint	nPass;
	nPass= beginMultiPass();
	// draw all passes.
	for (uint pass=0;pass<nPass; pass++)
	{
		// setup the pass.
		if (!setupPass(pass))
			return false;

		uint32 currIndex = startIndex;
		uint32 numLeftQuads = numQuads;

		// draw first batch of quads using the static setupped array
		if (startIndex < QUAD_BATCH_SIZE)
		{
			// draw first quads (as pair of tri to have guaranteed orientation)
			uint numQuadsToDraw = std::min(QUAD_BATCH_SIZE - startIndex, numQuads);
#ifdef USE_OPENGLES3
			drawElementsWebGL(GL_TRIANGLES, 6 * numQuadsToDraw, GL_UNSIGNED_SHORT, defaultIndices + 6 * startIndex);
#else
			glDrawElements(GL_TRIANGLES, 6 * numQuadsToDraw, GL_UNSIGNED_SHORT, defaultIndices + 6 * startIndex);
#endif
			numLeftQuads -= numQuadsToDraw;
			currIndex += 4 * numQuadsToDraw;
		}

		// draw remaining quads
		while (numLeftQuads)
		{
			// TODO : resetting vertex pointer would avoid the need to rebuild indices each times
			uint32 numQuadsToDraw = std::min(numLeftQuads, QUAD_BATCH_SIZE);
			// draw all quads
			if (4 * numQuadsToDraw + currIndex <= (1 << 16))
			{
				// indices fits on 16 bits
				GLshort indices[QUAD_BATCH_SIZE * 6];
				GLshort *curr = indices;
				GLshort *end = indices + 6 * numQuadsToDraw;
				uint16 vertexIndex = (uint16) currIndex;
				do
				{
					*curr++ = vertexIndex;
					*curr++ = vertexIndex + 1;
					*curr++ = vertexIndex + 2;
					*curr++ = vertexIndex;
					*curr++ = vertexIndex + 2;
					*curr++ = vertexIndex + 3;
					vertexIndex += 4;
				}
				while (curr != end);
#ifdef USE_OPENGLES3
				drawElementsWebGL(GL_TRIANGLES, 6 * numQuadsToDraw, GL_UNSIGNED_SHORT, indices);
#else
				glDrawElements(GL_TRIANGLES, 6 * numQuadsToDraw, GL_UNSIGNED_SHORT, indices);
#endif
			}
			else
			{
				// indices fits on 32 bits
				GLint indices[QUAD_BATCH_SIZE];
				GLint *curr = indices;
				GLint *end = indices + 6 * numQuadsToDraw;
				uint32 vertexIndex = currIndex;
				do
				{
					*curr++ = vertexIndex;
					*curr++ = vertexIndex + 1;
					*curr++ = vertexIndex + 2;
					*curr++ = vertexIndex;
					*curr++ = vertexIndex + 2;
					*curr++ = vertexIndex + 3;
					vertexIndex += 4;
				}
				while (curr != end);
#ifdef USE_OPENGLES3
				drawElementsWebGL(GL_TRIANGLES, 6 * numQuadsToDraw, GL_UNSIGNED_INT, indices);
#else
				glDrawElements(GL_TRIANGLES, 6 * numQuadsToDraw, GL_UNSIGNED_INT, indices);
#endif
			}
			numLeftQuads -= numQuadsToDraw;
			currIndex += 4 * numQuadsToDraw;
		}
	}
	// end multipass.
	endMultiPass();


	// Profiling.
	_PrimitiveProfileIn.NQuads  += numQuads ;
	_PrimitiveProfileOut.NQuads += numQuads  * nPass;

	if (_CurrentVertexBufferGL)
		_CurrentVertexBufferGL->setFrameInFlight(_SwapBufferCounter);

	return true;
}

}
}

