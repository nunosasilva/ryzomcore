// NeL - MMORPG Framework <https://wiki.ryzom.dev/>
// Copyright (C) 2026  Jan BOON (Kaetemi) <jan.boon@kaetemi.be>
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

//
// Projected texture decals (CDecal / CDecalManager).
//
// Why this sample exists: the decal system is a 2007 intern implementation that upstream
// never wired to anything viewable - ryzom/ryzomcore issue #680 calls it "incomplete, has
// not been tested exhaustively, and is not integrated in the Ryzom client yet". The only
// consumer in the tree is the Ryzom client's own CLegacyDecal, which is a *different*
// implementation behind ClientCfg.NewDecalSystem, and object_viewer is MFC/WIN32 so it
// cannot be built on Linux. That left the engine feature with no way to look at it. This
// sample is that way.
//
// No asset files: the receiver is a quad built in code and the splat texture is generated
// and written to a temporary .tga, so this runs in the headless smoke test on Linux, macOS,
// Windows and WebGL2 alike.
//
// The data path it exercises is the whole decal pipeline:
//   CDecal volume -> CVisualCollisionManager::receiveDecal -> CVisualCollisionMesh clipping
//   -> CPU UV generation -> CDecalManager batching -> renderRawTriangles
//
// Usage: nl_sample_decal [--no-decal]
//   --no-decal draws the identical floor without creating the decal. The two frames must
//   differ by the projected splat and nothing else, which is what makes the capture a gate
//   rather than a screenshot.

#include <nel/misc/types_nl.h>
#include <nel/misc/app_context.h>
#include <nel/misc/common.h>
#include <nel/misc/debug.h>
#include <nel/misc/event_listener.h>
#include <nel/misc/file.h>
#include <nel/misc/geom_ext.h>
#include <nel/misc/path.h>
#include <nel/misc/time_nl.h>
#include <nel/misc/vector_2f.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#include <nel/3d/decal.h>
#include <nel/3d/scissor.h>
#include <nel/3d/u_camera.h>
#include <nel/3d/u_decal.h>
#include <nel/3d/u_driver.h>
#include <nel/3d/u_material.h>
#include <nel/3d/u_scene.h>
#include <nel/3d/u_visual_collision_manager.h>
#include <nel/3d/u_visual_collision_mesh.h>
#include <nel/3d/vertex_buffer.h>
#include <nel/3d/viewport.h>
#include <nel/3d/visual_collision_mesh.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace std;
using namespace NLMISC;
using namespace NL3D;

namespace {

// Receiver: a 20x20 m floor at z = 0, two triangles.
const float FLOOR_HALF_SIZE = 10.f;

// The splat: centred on the origin, this radius (metres).
const float DECAL_RADIUS = 6.f;

// Generated texture, written next to the working directory and removed on exit.
const char *DECAL_TEXTURE_FILE = "nl_sample_decal_hit.tga";

// A 128x128 RGBA hit marker: soft red disc with a darker ring and a white centre, fully
// transparent outside. Built in memory so the sample carries no binary asset.
void buildDecalTexture(CBitmap &bmp)
{
	const uint32 size = 128;
	bmp.resize(size, size, CBitmap::RGBA);
	uint8 *px = bmp.getPixels().getPtr();

	const float center = (float)size * 0.5f;
	const float radius = (float)size * 0.42f;
	const float ringMin = (float)size * 0.30f;

	for (uint32 y = 0; y < size; ++y)
	{
		for (uint32 x = 0; x < size; ++x)
		{
			const float dx = (float)x + 0.5f - center;
			const float dy = (float)y + 0.5f - center;
			const float d = sqrtf(dx * dx + dy * dy);
			uint8 *p = px + ((size_t)y * size + x) * 4;

			if (d > radius)
			{
				p[0] = p[1] = p[2] = p[3] = 0; // discarded by the material's alpha test
				continue;
			}

			float alpha = 1.f;
			if (d > radius - 2.f)
				alpha = (radius - d) * 0.5f;
			if (alpha < 0.f) alpha = 0.f;

			uint8 r = 220, g = 40, b = 30;
			if (d < ringMin)
			{
				r = 140; g = 20; b = 20;
				if (d < (float)size * 0.10f)
				{
					r = 255; g = 240; b = 240;
				}
			}

			p[0] = r;
			p[1] = g;
			p[2] = b;
			p[3] = (uint8)(alpha * 255.f);
		}
	}
}

// Build a camera world matrix from eye, target and up (NeL has no CMatrix::lookAt; this is
// the same construction clip_plane and water use, and the driver wants its inverse).
CMatrix buildCamWorldMatrix(const CVector &eye, const CVector &target, const CVector &up)
{
	const CVector jj = (target - eye).normed();
	const CVector ii = (jj ^ up).normed();
	const CVector kk = ii ^ jj;
	CMatrix camWorld;
	camWorld.setRot(ii, jj, kk, true);
	camWorld.setPos(eye);
	return camWorld;
}

class CDecalDemo : public IEventListener
{
public:
	CDecalDemo(bool createDecal, bool legacy)
		: m_CloseWindow(false), m_CreateDecal(createDecal), m_StartTime(0.0), m_LegacyDriver(legacy)
	{
#ifdef __EMSCRIPTEN__
		m_Driver = UDriver::createDriver(0, UDriver::OpenGlEs3);
#else
		// --legacy selects the fixed-function GL driver: it is the reference for whether a
		// decal that fails to appear is a GL3 mega-program issue or a decal-system issue.
		m_Driver = UDriver::createDriver(0, m_LegacyDriver ? UDriver::OpenGl : UDriver::OpenGl3);
#endif
		if (!m_Driver)
		{
			nlerror("Failed to create driver");
			return;
		}

		m_Driver->EventServer.addListener(EventCloseWindowId, this);
		m_Driver->setDisplay(UDriver::CMode(800, 600, 32, true));
		m_Driver->setWindowTitle(ucstring("NeL Decal Sample"));

		// Floor material: plain unlit so the receiver is unmistakable under the splat.
		m_FloorMat = m_Driver->createMaterial();
		m_FloorMat.initUnlit();
		m_FloorMat.setZWrite(true);
		m_FloorMat.setZFunc(UMaterial::lessequal);
		m_FloorMat.setDoubleSided(true);

		m_Scene = m_Driver->createScene(true);
		if (!m_Scene)
		{
			nlerror("Failed to create scene");
			return;
		}

		// The viewport has to be set on BOTH the driver and the scene, as water does
		// (water/main.cpp:1136-1137), and the scissor is explicit driver state
		// (planar_reflection/main.cpp:720-722): with either one left at its default, draws
		// report success and produce no pixels at all.
		{
			CViewport vp;
			vp.initFullScreen();
			m_Driver->setViewport(vp);
			m_Scene->setViewport(vp);

			CScissor fullScissor;
			fullScissor.initFullScreen();
			m_Driver->setScissor(fullScissor);
		}

		m_Frustum.initPerspective(float(Pi / 3.0), 800.f / 600.f, 0.5f, 200.f);

		if (m_CreateDecal)
			setupDecal();

		m_StartTime = CTime::ticksToSecond(CTime::getPerformanceTime());
	}

	~CDecalDemo()
	{
		if (m_Scene && m_Vcm)
		{
			m_Scene->deleteVisualCollisionManager(m_Vcm);
			m_Vcm = NULL;
		}
		if (m_Driver)
		{
			m_Driver->deleteMaterial(m_FloorMat);
			m_Driver->release();
			delete m_Driver;
			m_Driver = NULL;
		}
		CFile::deleteFile(DECAL_TEXTURE_FILE);
	}

	void run()
	{
#ifndef __EMSCRIPTEN__
		while (m_Driver && m_Driver->isActive() && !m_CloseWindow)
			renderOneFrame();
#endif
	}

	void renderOneFrame()
	{
		if (!m_Driver->isFrameReady())
			return;
		m_Driver->EventServer.pump();

		const double now = CTime::ticksToSecond(CTime::getPerformanceTime()) - m_StartTime;

		// One camera for both paths: the scene camera drives the decal (it is applied by the
		// clip traversal inside UScene::render), the driver matrices drive the floor quad.
		const CVector eye(-6.f, -24.f, 16.f);
		const CVector target(0.f, 0.f, 0.f);
		const CMatrix camWorld = buildCamWorldMatrix(eye, target, CVector::K);
		CMatrix viewMatrix = camWorld;
		viewMatrix.invert();

		{
			UCamera cam = m_Scene->getCam();
			cam.setTransformMode(UTransformable::DirectMatrix);
			cam.setPerspective((float)Pi / 3.0, 800.f / 600.f, 0.5f, 200.f);
			cam.setMatrix(camWorld);
		}

		m_Driver->setFrustum(m_Frustum);
		m_Driver->setViewMatrix(viewMatrix);
		{
			CMatrix modelMatrix;
			modelMatrix.identity();
			m_Driver->setModelMatrix(modelMatrix);
		}

		m_Driver->clearBuffers(CRGBA(40, 60, 90));

		// Order matters, and it is the reverse of what looks natural: the opaque receiver has
		// to be drawn FIRST. Decals have depth writes disabled (CMaterial::setZWrite(false) in
		// CDecal's constructor), so drawing the floor afterwards would pass the depth test
		// against the untouched depth buffer and paint straight over the splat. The scene
		// render - which flushes the decal manager - therefore comes last, the same order the
		// client uses (opaque pass, then the decal pass).
		{
			const CVector a(-FLOOR_HALF_SIZE, -FLOOR_HALF_SIZE, 0.f);
			const CVector b( FLOOR_HALF_SIZE, -FLOOR_HALF_SIZE, 0.f);
			const CVector c( FLOOR_HALF_SIZE,  FLOOR_HALF_SIZE, 0.f);
			const CVector d(-FLOOR_HALF_SIZE,  FLOOR_HALF_SIZE, 0.f);
			CQuadColor quad;
			quad.V0 = a; quad.V1 = b; quad.V2 = c; quad.V3 = d;
			quad.Color0 = quad.Color1 = quad.Color2 = quad.Color3 = CRGBA(90, 110, 90);
			// Sink the floor slightly so the decal (z-bias -0.06) wins the depth test.
			quad.V0.z = quad.V1.z = quad.V2.z = quad.V3.z = -0.05f;
			m_Driver->drawQuad(quad, m_FloorMat);
		}

		m_Scene->render();

		(void)now;
		m_Driver->swapBuffers();
	}

	virtual void operator()(const CEvent &event) NL_OVERRIDE
	{
		if (event == EventCloseWindowId)
			m_CloseWindow = true;
	}

private:
	void setupDecal()
	{
		// 1. Receiver geometry, shared by the collision mesh and the floor quad.
		vector<CVector> vertices;
		vertices.push_back(CVector(-FLOOR_HALF_SIZE, -FLOOR_HALF_SIZE, 0.f));
		vertices.push_back(CVector( FLOOR_HALF_SIZE, -FLOOR_HALF_SIZE, 0.f));
		vertices.push_back(CVector( FLOOR_HALF_SIZE,  FLOOR_HALF_SIZE, 0.f));
		vertices.push_back(CVector(-FLOOR_HALF_SIZE,  FLOOR_HALF_SIZE, 0.f));

		vector<uint32> triangles;
		triangles.push_back(0); triangles.push_back(1); triangles.push_back(2);
		triangles.push_back(0); triangles.push_back(2); triangles.push_back(3);

		// 2. Collision mesh. build() keeps a pointer to the vertex buffer, so it must
		//    outlive the mesh: m_ShadowVB is a member for exactly that reason.
		m_CollisionMesh = new CVisualCollisionMesh();
		if (!m_CollisionMesh->build(vertices, triangles, m_ShadowVB))
		{
			nlwarning("CVisualCollisionMesh::build failed; no decal will be drawn");
			delete m_CollisionMesh;
			m_CollisionMesh = NULL;
			return;
		}

		// 3. Register it with the scene. CDecal::computeDecal() returns early with a warning
		//    when the scene has no visual collision manager for shadows, so this manager is
		//    not optional: it is what supplies the receiver triangles.
		m_Vcm = m_Scene->createVisualCollisionManager();
		m_Scene->setVisualCollisionManagerForShadow(m_Vcm);
		UVisualCollisionMesh ucm;
		ucm.attach(m_CollisionMesh);
		m_Vcm->addMeshInstanceCollision(ucm, CMatrix::Identity, false, false);

		// 4. The decal texture: generated, written, then referenced by name (the API takes a
		//    file name and loads through CPath/CTextureFile).
		CBitmap bmp;
		buildDecalTexture(bmp);
		string texPath = CPath::getCurrentPath();
		if (texPath.empty() || (texPath[texPath.size() - 1] != '/'))
			texPath += '/';
		texPath += DECAL_TEXTURE_FILE;
		{
			// COFile is the output stream (CIFile is read-only); close it so the file is
			// complete before CTextureFile reads it back.
			COFile out;
			const bool wrote = out.open(texPath) && bmp.writeTGA(out, 32);
			out.close();
			if (!wrote)
			{
				nlwarning("Could not write '%s'; rendering without a decal", texPath.c_str());
				return;
			}
		}
		CPath::addSearchPath(CPath::getCurrentPath(), true, false);

		// 5. Create and place the decal. setWorldMatrixForSpot() builds the unit-cube volume
		//    from a 2D position and a radius; the four side clip planes are vertical, so it
		//    lands on a horizontal receiver regardless of its height.
		m_Decal = m_Scene->createDecal();
		if (m_Decal.empty())
		{
			nlwarning("UScene::createDecal failed; no decal will be drawn");
			return;
		}
		// setWorldMatrixForSpot() goes through CTransform::setMatrix(), which asserts unless
		// the object is in DirectMatrix mode (transformable.h:167).
		m_Decal.setTransformMode(UTransformable::DirectMatrix);
		m_Decal.setTexture(texPath);
		m_Decal.setWorldMatrixForSpot(CVector2f(0.f, 0.f), DECAL_RADIUS);
		m_Decal.setStatic(true);    // the receiver never moves: clip once
		m_Decal.setClippingMode(2); // geometry clipping
		m_Decal.setDiffuse(CRGBA(255, 255, 255, 255));

		nlinfo("Decal created: %u-triangle receiver, radius %.1f m, texture %s",
			(uint)(triangles.size() / 3), DECAL_RADIUS, texPath.c_str());
	}

	UDriver		*m_Driver;
	UScene		*m_Scene;
	UMaterial	m_FloorMat;
	CFrustum	m_Frustum;
	CVisualCollisionMesh *m_CollisionMesh;
	UVisualCollisionManager *m_Vcm;
	UDecal		m_Decal;
	CVertexBuffer m_ShadowVB; // must outlive m_CollisionMesh (build() stores a pointer)
	bool		m_CloseWindow;
	bool		m_CreateDecal;
	double		m_StartTime;
	bool		m_LegacyDriver;
};

CDecalDemo *g_Demo = NULL;

#ifdef __EMSCRIPTEN__
void emscriptenMainLoop()
{
	if (g_Demo)
		g_Demo->renderOneFrame();
}
#endif

} // namespace

#ifdef NL_OS_WINDOWS
sint WINAPI WinMain(HINSTANCE /* hInstance */, HINSTANCE /* hPrevInstance */, LPSTR cmdline, int /* nCmdShow */)
#else
sint main(int argc, char **argv)
#endif
{
	CApplicationContext applicationContext;

	bool createDecal = true;
	bool legacy = false;
#ifdef NL_OS_WINDOWS
	if (cmdline && strstr(cmdline, "--no-decal") != NULL)
		createDecal = false;
#else
	for (int i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--no-decal") == 0)
			createDecal = false;
		else if (strcmp(argv[i], "--legacy") == 0)
			legacy = true;
	}
#endif

#ifdef __EMSCRIPTEN__
	static CDecalDemo demo(createDecal, legacy);
	g_Demo = &demo;
	emscripten_set_main_loop(emscriptenMainLoop, 0, 1);
#else
	CDecalDemo demo(createDecal, legacy);
	demo.run();
#endif

	return EXIT_SUCCESS;
}
