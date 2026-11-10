// NeL — 3D sample: the Specular material shader
//
// What this exercises, and why it exists:
//
// CMaterial::Specular is the only shader in the material enum that the GL3 driver carries a
// *two stage, two pass* setup for: stage 0 is the surface texture, stage 1 is a "sphere map"
// whose texture coordinates the driver generates itself (TexGenSphereMap — or a reflection map
// when stage 1 is a cube map), and the second pass adds the result. In the GL3 driver that lives
// in setupMaterial (driver_opengl3_material.cpp), in the legacy GL driver in its own specular
// case. Nothing else in this tree exercises it: no sample uses the shader, and no shipped asset
// that can be loaded here does either.
//
// So this sample builds one from nothing — a sphere with normals and two procedurally generated
// textures, no assets — and can render the *same* geometry with a plain one-stage Normal material
// instead (--normal). The difference between the two runs is the gate: if the specular path
// silently regresses (no second stage, no texgen, an env mode lost), the two frames collapse onto
// each other and the measurement says so.
//
//   nl_sample_specular            specular material (default)
//   nl_sample_specular --normal   plain Normal material, one texture: the reference
//   nl_sample_specular --cube     stage 1 is a cube map: the driver then generates the
//                                 reflection-map coordinate instead of the sphere map
//   nl_sample_specular --legacy   fixed-function GL driver (reference implementation of Specular)
//   nl_sample_specular --cutout   transparent bands in the surface texture + alpha test, on the
//                                 Normal shader (the only one with a tex-env path), no blending:
//                                 the only thing hiding the geometry is the alpha test
//   nl_sample_specular --spin     animation, for looking at it
//
// MULTI-ARCH: same pattern as the other samples — C++98 only, `sint main`, emscripten builds the
// GLES3 driver and an animation callback, and nothing here is platform specific.

#include "nel/misc/types_nl.h"
#include "nel/misc/app_context.h"
#include "nel/3d/u_driver.h"
#include "nel/3d/u_material.h"
#include "nel/3d/u_texture.h"
#include "nel/3d/texture_mem.h"
#include "nel/3d/texture_cube.h"
#include "nel/3d/material.h"
#include "nel/3d/vertex_buffer.h"
#include "nel/3d/index_buffer.h"
#include "nel/3d/viewport.h"
#include "nel/3d/scissor.h"
#include "nel/3d/frustum.h"
#include "nel/3d/driver_user.h"

#include <cstdio>
#include <cmath>

using namespace NLMISC;
using namespace NL3D;

#ifdef __EMSCRIPTEN__
#	include <emscripten/emscripten.h>
#endif

#ifdef NL_OS_WINDOWS
#	define NOMINMAX
#	include <windows.h>
#endif

// ----------------------------------------------------------------------------
// Procedural content. Two 128x128 RGBA textures: a checker "surface" for stage 0 and a radial
// "environment" for stage 1. Both are deliberately high-contrast, so a missing second stage or a
// texgen that produces a constant coordinate is obvious both to the eye and to a pixel diff.
// ----------------------------------------------------------------------------
static const uint TEX_SIZE = 128;

static void fillChecker(uint8 *pixels, bool cutout)
{
	for (uint y = 0; y < TEX_SIZE; ++y)
	{
		for (uint x = 0; x < TEX_SIZE; ++x)
		{
			const bool on = ((x / 16) + (y / 16)) & 1;
			uint8 *p = &pixels[(y * TEX_SIZE + x) * 4];
			p[0] = on ? 220 : 40;
			p[1] = on ? 180 : 60;
			p[2] = on ? 60 : 90;
			// --cutout: transparent bands through the surface texture, so the only thing hiding
			// the geometry is the alpha test.
			p[3] = (cutout && ((y / 16) & 1) && on) ? 0 : 255;
		}
	}
}

// A sphere-map environment: bright at the centre, dark at the rim, with a coloured ring, so the
// generated coordinates show up as a highlight that moves with the rotation.
static void fillEnvMap(uint8 *pixels)
{
	for (uint y = 0; y < TEX_SIZE; ++y)
	{
		for (uint x = 0; x < TEX_SIZE; ++x)
		{
			const float fx = (float(x) + 0.5f) / float(TEX_SIZE) * 2.f - 1.f;
			const float fy = (float(y) + 0.5f) / float(TEX_SIZE) * 2.f - 1.f;
			const float r = sqrtf(fx * fx + fy * fy);
			uint8 *p = &pixels[(y * TEX_SIZE + x) * 4];
			const float core = r < 0.35f ? 1.f : 0.f;
			const float ring = (r > 0.45f && r < 0.65f) ? 1.f : 0.f;
			p[0] = (uint8)(255.f * (core + 0.6f * ring));
			p[1] = (uint8)(255.f * (0.7f * core + 0.4f * ring));
			p[2] = (uint8)(255.f * (0.2f * core + 0.9f * ring));
			p[3] = 255;
		}
	}
}


// One face of a cube map: a colour ramp keyed by the face index, with a bright spot whose
// position differs per face. A reflection map that silently falls back to UV0 shows the same
// face pattern all over the sphere instead of six distinct directions.
static void fillCubeFace(uint8 *pixels, uint face)
{
	static const uint8 base[6][3] = {
		{ 220,  60,  60 }, // +X
		{  60, 220,  60 }, // -X
		{  60,  60, 220 }, // +Y
		{ 220, 220,  60 }, // -Y
		{ 220,  60, 220 }, // +Z
		{  60, 220, 220 }  // -Z
	};
	const uint sx = face & 1, sy = (face >> 1) & 1;
	for (uint y = 0; y < TEX_SIZE; ++y)
	{
		for (uint x = 0; x < TEX_SIZE; ++x)
		{
			uint8 *p = &pixels[(y * TEX_SIZE + x) * 4];
			const float fx = float(x) / float(TEX_SIZE), fy = float(y) / float(TEX_SIZE);
			const float spot = ((int(x / 16) == int(sx * 3 + 3) && int(y / 16) == int(sy * 3 + 3))) ? 1.f : 0.f;
			p[0] = (uint8)(base[face][0] * (0.35f + 0.65f * fx) + 255.f * spot) / (spot > 0.f ? 2 : 1);
			p[1] = (uint8)(base[face][1] * (0.35f + 0.65f * fy) + 255.f * spot) / (spot > 0.f ? 2 : 1);
			p[2] = (uint8)(base[face][2] * (0.35f + 0.65f * (1.f - fx)) + 255.f * spot) / (spot > 0.f ? 2 : 1);
			p[3] = 255;
		}
	}
}

// Sphere with Position + Normal: the normal is what the sphere-map texgen consumes, so it has to
// be in the vertex buffer (a quad would generate a constant coordinate).
static void buildSphere(CVertexBuffer &vb, CIndexBuffer &ib, float radius, uint slices, uint stacks)
{
	const uint numVerts = (stacks + 1) * (slices + 1);
	const uint numTris = stacks * slices * 2;

	vb.setVertexFormat(CVertexBuffer::PositionFlag | CVertexBuffer::NormalFlag | CVertexBuffer::TexCoord0Flag);
	vb.setBufferUsage(CVertexBuffer::Immutable, false);
	vb.setNumVertices(numVerts);

	ib.setFormat(NL_DEFAULT_INDEX_BUFFER_FORMAT);
	ib.setBufferUsage(CIndexBuffer::Immutable, false);
	ib.setNumIndexes(numTris * 3);

	{
		CVertexBufferReadWrite vba;
		vb.lock(vba);
		for (uint j = 0; j <= stacks; ++j)
		{
			const float phi = float(Pi) * float(j) / float(stacks);
			const float sp = sinf(phi), cp = cosf(phi);
			for (uint i = 0; i <= slices; ++i)
			{
				const float theta = 2.f * float(Pi) * float(i) / float(slices);
				const CVector n(sp * cosf(theta), sp * sinf(theta), cp);
				const uint idx = j * (slices + 1) + i;
				vba.setVertexCoord(idx, n * radius);
				vba.setNormalCoord(idx, n);
				// A UV0 that is real (not needed by the Specular shader, but a material that
				// asks for texgen on stage 0 must still have something sane to fall back to)
				vba.setTexCoord(idx, 0, float(i) / float(slices), float(j) / float(stacks));
			}
		}
	}
	{
		CIndexBufferReadWrite iba;
		ib.lock(iba);
		// setTri() takes the index-buffer position, not the triangle number (the same trap the
		// decal work hit): each triangle writes three indexes, so the offset is triIdx * 3.
		uint triIdx = 0;
		for (uint j = 0; j < stacks; ++j)
		{
			for (uint i = 0; i < slices; ++i)
			{
				const uint a = j * (slices + 1) + i;
				const uint b = a + slices + 1;
				iba.setTri(triIdx * 3, a, b, a + 1);
				++triIdx;
				iba.setTri(triIdx * 3, a + 1, b, b + 1);
				++triIdx;
			}
		}
	}
}

// Camera placement -> view matrix (same helper the decal sample uses: the driver wants the
// inverted world matrix, invert() is what turns one into the other).
static CMatrix buildCamWorldMatrix(const CVector &eye, const CVector &target, const CVector &up)
{
	const CVector jj = (target - eye).normed();
	const CVector ii = (jj ^ up).normed();
	const CVector kk = ii ^ jj;
	CMatrix camWorld;
	camWorld.setRot(ii, jj, kk, true);
	camWorld.setPos(eye);
	return camWorld;
}

// ----------------------------------------------------------------------------
class CSpecularDemo
{
public:
	CSpecularDemo(bool normalMaterial, bool legacy, bool cubeMap, bool cutout)
	{
		_NormalMaterial = normalMaterial;
		_LegacyDriver = legacy;
		_CloseWindow = false;
		_Spin = false;
		_CubeMap = cubeMap; // must be known before the textures are built below
		_Cutout = cutout;
		_Frame = 0;
		_Driver = NULL;

#ifdef __EMSCRIPTEN__
		_Driver = UDriver::createDriver(0, UDriver::OpenGlEs3);
#else
		// --legacy selects the fixed-function GL driver, which has its own (working) Specular
		// implementation: it is the reference for "the material is right but the GL3 path is not".
		_Driver = UDriver::createDriver(0, _LegacyDriver ? UDriver::OpenGl : UDriver::OpenGl3);
#endif
		if (!_Driver)
		{
			nlerror("Failed to create driver");
			return;
		}

		_Driver->setDisplay(UDriver::CMode(800, 600, 32, true));
		_Driver->setWindowTitle(ucstring("NeL Specular Sample"));

		{
			CViewport vp;
			vp.initFullScreen();
			_Driver->setViewport(vp);
			CScissor sc;
			sc.initFullScreen();
			_Driver->setScissor(sc);
		}

		// Textures, procedurally generated: the sample needs no assets (the same reason the
		// decal sample generates its own). Filled through the driver's UTextureMem, which owns
		// the bitmap: getPointer() + touch(), as ryzom/client/src/continent.cpp does.
		// Textures, procedurally generated (the sample needs no assets, like the decal sample).
		// CTextureMem is the engine-side texture the material wants; it is ref-counted, so
		// setTexture() below is what keeps it alive - no explicit delete, and no U-layer object.
		// The buffer is handed to CTextureMem with _delete = true, so it owns it (and the
		// material's reference keeps the texture alive). Handing it a temporary vector's data
		// would leave the texture reading freed memory - which is exactly what the first version
		// of this sample looked like: two random colours instead of the generated patterns.
		const uint32 texBytes = TEX_SIZE * TEX_SIZE * 4;
		uint8 *px = new uint8[texBytes];
		fillChecker(px, _Cutout);
		CTextureMem *texSurface = new CTextureMem(px, texBytes, true, false, TEX_SIZE, TEX_SIZE, CBitmap::RGBA);
		texSurface->setWrapS(ITexture::Clamp);
		texSurface->setWrapT(ITexture::Clamp);

		ITexture *texEnv = NULL;
		if (_CubeMap)
		{
			// A cube map stage is what makes the driver choose the *reflection* texgen mode
			// (TexGenReflectionMap) instead of the sphere map, so the two generated modes are
			// covered one at a time by --cube.
			CTextureCube *cube = new CTextureCube();
			for (uint f = 0; f < 6; ++f)
			{
				px = new uint8[texBytes];
				fillCubeFace(px, f);
				CTextureMem *face = new CTextureMem(px, texBytes, true, false, TEX_SIZE, TEX_SIZE, CBitmap::RGBA);
				// A face with no share name makes supportSharing() false, and the cube path still
				// calls getShareName(), which asserts on an empty name (texture_mem.h:148). Give
				// each face one; they are only ever used by this sample.
				face->setShareName(NLMISC::toString("nl_sample_specular_cube_%u", f));
				cube->setTexture((CTextureCube::TFace)f, face);
			}
			texEnv = cube;
		}
		else
		{
			px = new uint8[texBytes];
			fillEnvMap(px);
			CTextureMem *env = new CTextureMem(px, texBytes, true, false, TEX_SIZE, TEX_SIZE, CBitmap::RGBA);
			env->setWrapS(ITexture::Clamp);
			env->setWrapT(ITexture::Clamp);
			texEnv = env;
		}

		// The material under test. createMaterial() gives a UMaterial whose CMaterial the driver
		// owns (deleteMaterial() deletes it), and getObjectPtr() is how the shader is reached:
		// UMaterial does not expose setShader, and setShader(Specular) is the point of the sample.
		_Mat = _Driver->createMaterial();
		CMaterial *mat = _Mat.getObjectPtr();
		mat->setShader(_NormalMaterial ? CMaterial::Normal : CMaterial::Specular);
		mat->setLighting(false);
		mat->setDoubleSided(true);
		mat->setTexture(0, texSurface);
		if (!_NormalMaterial)
		{
			mat->setTexture(1, texEnv);
			mat->setSpecular(CRGBA(255, 255, 255));
		}
		// The fixed-function driver's Specular is a *lit* shader: its passes modulate by the
		// material colour, which defaults to black, so the fixed-function run needs an explicit
		// white colour or it renders a black sphere on a near-black background (2 colours, which
		// is exactly what this sample produced before this line was added). The GL3 path generates
		// the coordinates itself and ignores it.
		if (_LegacyDriver)
			mat->setColor(CRGBA(255, 255, 255));
		if (_Cutout)
		{
			// The alpha test needs two things a hand-built material does not get for free: the
			// wrapper's alpha taken from the texture (CMaterial::texEnvOpAlpha asserts
			// _ShaderType==Normal, so this only exists for the Normal shader - the Specular shader
			// has no tex-env path at all) and the test itself. Measured both ways: with these two
			// lines an all-transparent texture discards the whole sphere; without them it renders
			// solid even with nlAlphaTest=1 in the material block.
			mat->setBlend(false);
			mat->setAlphaTest(true);
			mat->setAlphaTestThreshold(0.5f);
			mat->texEnvOpAlpha(0, CMaterial::Replace);
			mat->texEnvArg0Alpha(0, CMaterial::Texture, CMaterial::SrcAlpha);
		}
		_Mat.setZWrite(true);
		_Mat.setZFunc(UMaterial::lessequal);

		buildSphere(_SphereVB, _SphereIB, 1.6f, 32, 16);

		// Camera: from -Y towards the origin, tilted down slightly.
		CFrustum frustum;
		frustum.initPerspective(float(Pi / 3.0), 800.f / 600.f, 0.5f, 100.f);
		_Driver->setFrustum(frustum);

		// The driver takes a *view* matrix; the inverted camera world is that matrix.
		CMatrix camWorld = buildCamWorldMatrix(CVector(0.f, -6.5f, 2.2f), CVector(0.f, 0.f, 1.6f), CVector::K);
		CMatrix view = camWorld;
		view.invert();
		_Driver->setViewMatrix(view);
	}

	~CSpecularDemo()
	{
		if (!_Driver)
			return;
		// deleteMaterial() deletes the CMaterial, which drops the textures' references.
		_Driver->deleteMaterial(_Mat);
	}

	void render()
	{
		if (!_Driver)
			return;

		_Driver->clearBuffers(CRGBA(12, 16, 28));

		// Fixed pose by default: the sample doubles as a pixel gate, and an animated angle makes
		// two runs differ purely because the capture lands on a different frame. --spin gives the
		// animation (the highlight then travels, which is what an interactive run wants).
		const float t = _Spin ? float(_Frame) / 60.f : 0.75f;
		CMatrix model;
		model.identity();
		model.rotateX(t * 0.45f);
		model.rotateZ(t * 0.7f);
		model.setPos(CVector(0.f, 0.f, 1.6f));
		_Driver->setModelMatrix(model);

		IDriver *drv = static_cast<CDriverUser *>(_Driver)->getDriver();
		drv->activeVertexBuffer(_SphereVB);
		drv->activeIndexBuffer(_SphereIB);
		// Flat renderTriangles() takes the triangle count, not the index count.
		drv->renderTriangles(*_Mat.getObjectPtr(), 0, _SphereIB.getNumIndexes() / 3);

		_Driver->swapBuffers();
		++_Frame;
	}

	void setSpin(bool spin) { _Spin = spin; }
	void closeWindow() { _CloseWindow = true; }
	bool mustClose() const { return _CloseWindow; }
	uint getFrame() const { return _Frame; }

private:
	UDriver			*_Driver;
	bool			_NormalMaterial;
	bool			_LegacyDriver;
	bool			_CloseWindow;
	bool			_Spin;
	bool			_CubeMap;
	bool			_Cutout;
	uint			_Frame;

	UMaterial		_Mat;
	CVertexBuffer	_SphereVB;
	CIndexBuffer	_SphereIB;
};


sint main(sint argc, char **argv)
{
	CApplicationContext appContext;

	bool normalMaterial = false;
	bool legacy = false;
	bool spin = false;
	bool cubeMap = false;
	bool cutout = false;
	sint maxFrames = 0; // 0 = run until the window is closed
	for (sint i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--normal") == 0)
			normalMaterial = true;
		else if (strcmp(argv[i], "--legacy") == 0)
			legacy = true;
		else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
			maxFrames = atoi(argv[++i]);
		else if (strcmp(argv[i], "--spin") == 0)
			spin = true;
		else if (strcmp(argv[i], "--cube") == 0)
			cubeMap = true;
		else if (strcmp(argv[i], "--cutout") == 0)
			cutout = true;
	}

#ifdef __EMSCRIPTEN__
	static CSpecularDemo *demo = new CSpecularDemo(normalMaterial || cutout, legacy, cubeMap, cutout);
	demo->setSpin(spin);
	static sint framesLeft = maxFrames;
	struct CLoop {
		static void run()
		{
			if (demo->mustClose() || (framesLeft != 0 && (sint)demo->getFrame() >= framesLeft))
				return;
			demo->render();
		}
	};
	emscripten_set_main_loop(CLoop::run, 0, 1);
#else
	CSpecularDemo demo(normalMaterial || cutout, legacy, cubeMap, cutout);
	demo.setSpin(spin);
	while (!demo.mustClose() && (maxFrames == 0 || (sint)demo.getFrame() < maxFrames))
	{
		demo.render();
		// The window's close button is not wired to a listener here on purpose: the sample is a
		// frame generator for the gate, so it runs a fixed number of frames when asked and
		// otherwise forever (the harness kills it).
#ifdef NL_OS_WINDOWS
		Sleep(1);
#endif
	}
#endif
	return 0;
}
