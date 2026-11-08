// NeL - MMORPG Framework <https://wiki.ryzom.dev/>
// Copyright (C) 2025  Jan BOON (Kaetemi) <jan.boon@kaetemi.be>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of
// the License, or (at your option) any later version.
//
// This program is distributed that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public
// License for more details.
//
// You should have received a copy of the GNU Affero General Public
// License along with this program.  If not, see
// <http://www.gnu.org/licenses/>.

//
// Landscape & Vegetation Demo
//
// Stage 1: a GL3 driver, a camera and a flat ground drawn straight through
// the driver (CQuadColor), so the sample is known-good before any landscape
// code is added.
//
// Stage 2 (this file): the flat quad is replaced by a real heightfield - a
// 32x32 grid of quads whose Z comes from a procedural height function,
// normals-looked-up per vertex, height-shaded - plus tree-like billboards
// standing on that surface. Everything still goes through the GL3 driver as
// driver quads.
//
// NOT covered here (needs a ryzomcore_graphics checkout: tile bank, .zone
// files, .shd art): NL3D::CLandscape / CZone / CHeightMap patch binding and
// bind edges, CVegetableManager + CTileVegetableDesc instancing, and the
// landscape vertex program (TODO_VP_GLSL).
//

#include <nel/misc/types_nl.h>
#include <nel/misc/app_context.h>
#include <nel/misc/common.h>
#include <nel/misc/debug.h>
#include <nel/misc/event_listener.h>
#include <nel/misc/geom_ext.h>
#include <nel/misc/time_nl.h>

#include <cstdlib>
#include <cstdio>   // sscanf (fog bounds from argv)

#include <nel/3d/bezier_patch.h>
#include <nel/3d/u_driver.h>
#include <nel/3d/u_material.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef NL_OS_WINDOWS
#ifndef NL_COMP_MINGW
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace std;
using namespace NLMISC;
using namespace NL3D;

// ---------------------------------------------------------------------------
// The procedural heightfield. One scalar field sampled on a regular grid,
// which is what a landscape heightmap is: no art files, no CZone/CLandscape
// (those need a tile bank + .zone files - see 04-DONE).
// ---------------------------------------------------------------------------
class CHeightField
{
public:
	static const uint GRID = 33; // 32x32 quads -> 33x33 vertices

	float sample(float x, float y) const
	{
		// rolling hills + a ridge + a valley, all from trig, deterministic
		float h = 9.0f * sinf(x * 0.045f) + 7.0f * cosf(y * 0.055f);
		h += 5.0f * sinf((x + y) * 0.03f);
		h += 3.0f * cosf((x - y) * 0.08f);
		return h;
	}

	// precomputed grid, in world XY over [-HALF, +HALF]
	float m_Z[GRID][GRID];
};

static const float H_HALF = 120.0f; // half-size of the heightfield in metres

class CLandscapeDemo : public IEventListener
{
public:
	// defaults matter: the emscripten branch constructs with the driver only, so
	// without them the web build would not compile
	CLandscapeDemo(UDriver::TDriver driver, float fixedCamAngle = -1.f, float fogStart = 90.f, float fogEnd = 240.f);
	~CLandscapeDemo();
	void run();
	void renderOneFrame();

	virtual void operator()(const CEvent &event) NL_OVERRIDE;

private:
	bool m_CloseWindow;
	UDriver *m_Driver;
	UMaterial m_GroundMat;
	UMaterial m_TreeMat;
	CHeightField m_Field;
	double m_StartTime;
	// < 0 = animated orbit. >= 0 = freeze the camera at that angle, which is what
	// makes a two-driver A/B comparable: the orbit is 0.2 rad/s, so a 1 s startup
	// difference between drivers would rotate the view ~11 degrees.
	float m_FixedCamAngle;
	float m_FogStart;
	float m_FogEnd;
};

CLandscapeDemo::CLandscapeDemo(UDriver::TDriver driver, float fixedCamAngle, float fogStart, float fogEnd)
	: m_CloseWindow(false), m_FixedCamAngle(fixedCamAngle), m_FogStart(fogStart), m_FogEnd(fogEnd)
{
#ifdef __EMSCRIPTEN__
	m_Driver = UDriver::createDriver(0, UDriver::OpenGlEs3);
#else
	m_Driver = UDriver::createDriver(0, driver);
#endif
	if (!m_Driver)
	{
		nlerror("Failed to create driver");
		return;
	}

	m_Driver->EventServer.addListener(EventCloseWindowId, this);

	m_Driver->setDisplay(UDriver::CMode(800, 600, 32, true));
	m_Driver->setWindowTitle(ucstring("NeL Landscape & Vegetation Demo"));

	m_GroundMat = m_Driver->createMaterial();
	m_GroundMat.initUnlit();
	m_GroundMat.setZWrite(true);
	m_GroundMat.setZFunc(UMaterial::lessequal);

	m_TreeMat = m_Driver->createMaterial();
	m_TreeMat.initUnlit();
	m_TreeMat.setZWrite(true);
	m_TreeMat.setZFunc(UMaterial::lessequal);

	// Fog, for the Track B question. setupFog() takes WORLD-UNIT EYE DISTANCES
	// (legacy driver: glFogf(GL_FOG_START, start) at driver_opengl.cpp:1504), NOT
	// 0..1 fractions. The eye sits 150 m from the origin and the field spans +/-120
	// m, so 90..240 puts the gradient across the visible ground. What this A/B
	// actually measures is the fog DEPTH axis and units: the legacy path here is
	// fixed-function GL_FOG (per fragment), so per-vertex vs per-fragment is NOT
	// what differs - that needs the nelvp landscape VP (DP4 o[FOGC] in
	// landscapevb_allocator.cpp:520), which needs art files this checkout does not have.
	m_Driver->enableFog(true);
	m_Driver->setupFog(m_FogStart, m_FogEnd, CRGBA(176, 188, 204));
	m_Driver->setupFogMode(0, 1.f); // 0 = linear fog (UDriver::setupFogMode takes a uint)

	// Fill the height grid once, at init.
	for (uint j = 0; j < CHeightField::GRID; j++)
	{
		for (uint i = 0; i < CHeightField::GRID; i++)
		{
			float x = -H_HALF + (2.0f * H_HALF * float(i)) / float(CHeightField::GRID - 1);
			float y = -H_HALF + (2.0f * H_HALF * float(j)) / float(CHeightField::GRID - 1);
			m_Field.m_Z[j][i] = m_Field.sample(x, y);
		}
	}

	m_StartTime = CTime::ticksToSecond(CTime::getPerformanceTime());
}

CLandscapeDemo::~CLandscapeDemo()
{
	m_Driver->deleteMaterial(m_GroundMat);
	m_Driver->deleteMaterial(m_TreeMat);
	m_Driver->release();
	delete m_Driver;
}

void CLandscapeDemo::operator()(const CEvent &event)
{
	if (event == EventCloseWindowId)
		m_CloseWindow = true;
}

void CLandscapeDemo::renderOneFrame()
{
	if (!m_Driver->isFrameReady())
		return;

	m_Driver->EventServer.pump();

	// rebuild the frustum from the real window size, so a resized window keeps
	// square pixels instead of stretching the 800x600 aspect (same as ppl/main.cpp)
	uint32 screenW, screenH;
	m_Driver->getWindowSize(screenW, screenH);
	if (screenW == 0 || screenH == 0)
	{
		nlSleep(10);
		return;
	}
	CFrustum frustum;
	frustum.initPerspective(float(Pi / 3.0), float(screenW) / float(screenH), 0.1f, 500.f);

	double now = CTime::ticksToSecond(CTime::getPerformanceTime()) - m_StartTime;

	m_Driver->clearBuffers(CRGBA(60, 90, 140));

	// slow orbit over the ground
	float camAngle = m_FixedCamAngle >= 0.f ? m_FixedCamAngle : float(now * 0.2);
	CVector eye(cosf(camAngle) * 150.f, sinf(camAngle) * 150.f, 70.f);
	CVector target(0.f, 0.f, 0.f);
	CVector up(0.f, 0.f, 1.f);
	CVector jj = (target - eye).normed();
	CVector ii = (jj ^ up).normed();
	CVector kk = ii ^ jj;
	CMatrix camWorld;
	camWorld.setRot(ii, jj, kk, true);
	camWorld.setPos(eye);
	CMatrix viewMatrix = camWorld;
	viewMatrix.invert();

	m_Driver->setFrustum(frustum);
	m_Driver->setViewMatrix(viewMatrix);

	CMatrix modelMatrix;
	modelMatrix.identity();
	m_Driver->setModelMatrix(modelMatrix);

	const float step = (2.0f * H_HALF) / float(CHeightField::GRID - 1);

	// Terrain: one driver quad per heightfield cell, height-shaded.
	for (uint j = 0; j + 1 < CHeightField::GRID; j++)
	{
		for (uint i = 0; i + 1 < CHeightField::GRID; i++)
		{
			float x0 = -H_HALF + step * float(i);
			float y0 = -H_HALF + step * float(j);
			float x1 = x0 + step;
			float y1 = y0 + step;

			float z00 = m_Field.m_Z[j][i];
			float z10 = m_Field.m_Z[j][i + 1];
			float z11 = m_Field.m_Z[j + 1][i + 1];
			float z01 = m_Field.m_Z[j + 1][i];

			CQuadColor tile;
			tile.V0 = CVector(x0, y0, z00);
			tile.V1 = CVector(x1, y0, z10);
			tile.V2 = CVector(x1, y1, z11);
			tile.V3 = CVector(x0, y1, z01);

			// slope from the quad normal + height -> colour banding, so the
			// relief is readable and the frame is not one flat colour
			CVector e1 = tile.V1 - tile.V0;
			CVector e2 = tile.V3 - tile.V0;
			CVector nrm = (e1 ^ e2).normed();
			float slope = nrm.z;
			if (slope < 0.f)
				slope = -slope;
			float zAvg = 0.25f * (z00 + z10 + z11 + z01);
			float k = (zAvg + 24.0f) / 48.0f;
			clamp(k, 0.f, 1.f);
			uint g = uint(70 + 130 * k * (0.45f + 0.55f * slope));
			uint r = uint(40 + 90 * k);
			uint b = uint(30 + 40 * (1.0f - k));
			CRGBA c = CRGBA(r, g, b);
			tile.Color0 = tile.Color1 = tile.Color2 = tile.Color3 = c;
			m_Driver->drawQuad(tile, m_GroundMat);
		}
	}

	// "Vegetation": tree-like billboards standing on the heightfield.
	// Trunk quad + canopy quad per tree, both placed at the sampled ground
	// height. Not CVegetableManager instances (those need a CTileVegetableDesc
	// / art checkout) - geometry only, see 04-DONE.
	for (uint n = 0; n < 90; n++)
	{
		// deterministic scatter over the field
		float sx = -H_HALF + (2.0f * H_HALF * float((n * 7919u) % 1000u)) / 1000.0f;
		float sy = -H_HALF + (2.0f * H_HALF * float((n * 104729u) % 1000u)) / 1000.0f;
		float gz = m_Field.sample(sx, sy);
		CVector base(sx, sy, gz);

		float trunkH = 3.0f + float(n % 4);
		float canopyH = 4.0f + float((n * 31) % 5);
		float halfW = 0.5f + 0.05f * float(n % 3);

		// trunk (billboard in Y so it is visible from any orbit angle)
		CQuadColor trunk;
		trunk.V0 = base + CVector(-halfW, 0.f, 0.f);
		trunk.V1 = base + CVector(halfW, 0.f, 0.f);
		trunk.V2 = base + CVector(halfW, 0.f, trunkH);
		trunk.V3 = base + CVector(-halfW, 0.f, trunkH);
		trunk.Color0 = trunk.Color1 = CRGBA(70, 45, 25);
		trunk.Color2 = trunk.Color3 = CRGBA(110, 75, 45);
		m_Driver->drawQuad(trunk, m_TreeMat);

		// canopy, two crossed quads so the tree reads as a volume
		CVector cbase = base + CVector(0.f, 0.f, trunkH);
		CQuadColor canopy;
		canopy.V0 = cbase + CVector(-2.2f, 0.f, 0.f);
		canopy.V1 = cbase + CVector(2.2f, 0.f, 0.f);
		canopy.V2 = cbase + CVector(2.2f, 0.f, canopyH);
		canopy.V3 = cbase + CVector(-2.2f, 0.f, canopyH);
		canopy.Color0 = canopy.Color1 = CRGBA(25, 55, 25);
		canopy.Color2 = canopy.Color3 = CRGBA(120, 170, 90);
		m_Driver->drawQuad(canopy, m_TreeMat);

		CQuadColor canopy2;
		canopy2.V0 = cbase + CVector(0.f, -2.2f, 0.f);
		canopy2.V1 = cbase + CVector(0.f, 2.2f, 0.f);
		canopy2.V2 = cbase + CVector(0.f, 2.2f, canopyH);
		canopy2.V3 = cbase + CVector(0.f, -2.2f, canopyH);
		canopy2.Color0 = canopy2.Color1 = CRGBA(25, 55, 25);
		canopy2.Color2 = canopy2.Color3 = CRGBA(140, 190, 100);
		m_Driver->drawQuad(canopy2, m_TreeMat);
	}

	m_Driver->swapBuffers();
}

void CLandscapeDemo::run()
{
#ifndef __EMSCRIPTEN__
	while (m_Driver->isActive() && !m_CloseWindow)
	{
		renderOneFrame();
	}
#endif
}

#ifdef __EMSCRIPTEN__
static CLandscapeDemo *s_Demo = NULL;

static void emscriptenMainLoop()
{
	if (s_Demo)
		s_Demo->renderOneFrame();
}
#endif

#ifdef NL_OS_WINDOWS
sint WINAPI WinMain(HINSTANCE /* hInstance */, HINSTANCE /* hPrevInstance */, LPSTR /* cmdline */, int /* nCmdShow */)
#else
sint main(int argc, char **argv)
#endif
{
	CApplicationContext applicationContext;

	// Driver choice, so the same scene can be rendered by the legacy and the OpenGL3
	// driver for the fog A/B:  ./nl_sample_landscape [opengl|opengl3]
	NL3D::UDriver::TDriver driver = NL3D::UDriver::OpenGl3;
#ifdef __EMSCRIPTEN__
	NL3D::UDriver::TDriver unusedDriver = driver;
	(void)unusedDriver;
#else
	if (argc > 1 && NLMISC::stricmp(argv[1], "opengl") == 0)
		driver = NL3D::UDriver::OpenGl;
	// optional 2nd arg: freeze the camera (radians) so two drivers draw the same view
	float fixedCamAngle = -1.f;
	// 3rd/4th args: fog start/end in world units. Passing a huge range (1e5) means
	// "fog off", which gives the reference frame for the fog A/B.
	float fogStart = 90.f, fogEnd = 240.f;
	// sscanf, not strtof: strtof is C99 and is absent from the MSVC 2008 CRT, and vs2008.yml
	// compiles this file (WITH_NEL_SAMPLES=ON, WITH_3D=ON). sscanf is already the pattern used
	// by cluster_viewer/main.cpp:161 and by several tools.
	if (argc > 4) // both fog bounds must be present: argv[argc] is NULL
	{
		if (sscanf(argv[3], "%f", &fogStart) != 1)
			fogStart = 90.f;
		if (sscanf(argv[4], "%f", &fogEnd) != 1)
			fogEnd = 240.f;
	}
#endif

#ifdef __EMSCRIPTEN__
	static CLandscapeDemo demo(UDriver::OpenGlEs3);
	s_Demo = &demo;
	EM_ASM({ if (window.nlLoadingComplete) window.nlLoadingComplete(); });
	emscripten_set_main_loop(emscriptenMainLoop, 0, 1);
#else
	CLandscapeDemo demo(driver, fixedCamAngle, fogStart, fogEnd);
	demo.run();
#endif

	return EXIT_SUCCESS;
}
