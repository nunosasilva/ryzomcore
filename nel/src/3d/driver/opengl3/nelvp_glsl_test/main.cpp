// -----------------------------------------------------------------------------
/**
 * \file
 * CPU-only regression test for the nelvp -> GLSL vertex program converter
 * (CDriverGL3::convertNelvpToGLSL).
 *
 * Why this exists: the GL3 driver converts every shipped nelvp vertex program to
 * GLSL at compile time, and the conversion is the thing that makes the legacy
 * programs (landscape, vegetation, water...) render at all under OpenGL3. Nothing
 * in the tree asserted *what the conversion produces*: the only coverage was
 * rendered frames from nel/samples/3d (which need Xvfb + a GL context and are
 * Unix-only), and those frames cannot tell a correct conversion from a
 * correct-looking one that drops a varying.
 *
 * What it is NOT: it does not touch GL. No context, no window, no Xvfb, no
 * ryzomcore_graphics asset. It runs on Windows, macOS and Linux in milliseconds,
 * which is the point - see AGENTS_todo.md ## Multi-arch: the headless frame
 * harness is deliberately Unix-only, so a converter regression has to be caught
 * somewhere else.
 *
 * Reaching CDriverGL3: this test includes the driver's own header
 * (driver_opengl3.h, not a public NeL header) and links the driver library. That
 * only links when the driver is a STATIC library - a MODULE driver exports just
 * NL3D_createIDriverInstance / NL3D_interfaceVersion (driver_opengl3.def). The
 * CMakeLists gates on WITH_STATIC_DRIVERS for that reason.
 *
 * \author Nevrax France
 * \date 2026
 */
#ifdef NL_OS_WINDOWS
#	define WIN32_LEAN_AND_MEAN
#	define NOMINMAX
#endif

#include <iostream>
#include <string>
#include <cstdlib>

// same order as the driver's own translation units (driver_opengl3_nelvp.cpp:20):
// stdopengl3.h brings in the GL headers that driver_opengl3_states.h needs, and
// driver_opengl3.h is the PCH for the driver library.
#include "stdopengl3.h"
#include "driver_opengl3.h"
#include "driver_opengl3_program.h"
#include "driver_opengl3_extension.h"

#include <nel/3d/landscapevb_allocator.h>
#include <nel/3d/program.h>
#include <nel/3d/uniform_buffer_format.h>
#include <nel/3d/vertex_program.h>
#include <nel/3d/vegetable_def.h>

using namespace NL3D;

// ----------------------------------------------------------------------------
// tiny harness: no test framework in NeL, and a framework would be a bigger
// dependency than the ~40 asserts here.
// ----------------------------------------------------------------------------
static int gChecks = 0;
static int gFailures = 0;

static void check(bool condition, const std::string &what)
{
	++gChecks;
	if (!condition)
	{
		std::cout << "  FAIL: " << what << std::endl;
		++gFailures;
	}
}

static bool has(const std::string &hay, const std::string &needle)
{
	return hay.find(needle) != std::string::npos;
}

static void checkHas(const std::string &glsl, const std::string &needle, const std::string &what)
{
	check(has(glsl, needle), what + " [missing '" + needle + "']");
}

static void checkNot(const std::string &glsl, const std::string &needle, const std::string &what)
{
	check(!has(glsl, needle), what + " [unexpected '" + needle + "']");
}

// Dump mode: ./nelvp_glsl_test --dump > out.glsl, used when re-baselining the
// expected conversion output by hand.
static bool gDump = false;

// Return the body of "#ifdef <macro> ... #endif" in a generated source, or "" if
// there is no such block. The hand-written landscape program shares one body across
// all four variants and switches it with compile-time defines, so checking for plain
// text presence proves nothing: the text is in every variant's source.
static std::string ifdefBlock(const std::string &glsl, const std::string &macro)
{
	std::string start = "#ifdef " + macro;
	size_t from = glsl.find(start);
	if (from == std::string::npos)
		return "";
	from = glsl.find('\n', from);
	if (from == std::string::npos)
		return "";
	size_t to = glsl.find("#endif", from);
	if (to == std::string::npos)
		return "";
	return glsl.substr(from, to - from);
}

// The source with every "#ifdef <macro> ... #endif" block removed, so a check on
// the remainder means "outside that guard".
static std::string stripIfdefBlocks(const std::string &glsl, const std::string &macro)
{
	std::string out;
	std::string remaining = glsl;
	for (;;)
	{
		std::string start = "#ifdef " + macro;
		size_t from = remaining.find(start);
		if (from == std::string::npos)
		{
			out += remaining;
			break;
		}
		out += remaining.substr(0, from);
		size_t to = remaining.find("#endif", from);
		if (to == std::string::npos)
		{
			out += remaining.substr(from);
			break;
		}
		remaining = remaining.substr(to + 6);
	}
	return out;
}

static void dumpIfAsked(const std::string &name, const std::string &glsl)
{
	if (!gDump)
		return;
	std::cout << "// ==== " << name << " ====\n" << glsl << std::endl;
}

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------

// Convert one program and return the generated source (nullptr if none added).
static IProgram::CSource *convertAndFetch(CVertexProgram &program, bool linked, bool clip)
{
	bool ok = NLDRIVERGL3::CDriverGL3::convertNelvpToGLSL(&program, linked, clip);
	check(ok, "conversion must succeed");
	if (!ok)
		return nullptr;

	IProgram::CSource *found = nullptr;
	for(size_t i = 0; i < program.getSourceNb(); ++i)
	{
		IProgram::CSource *src = program.getSource(i);
		if (src->Profile == IProgram::glsl330v || src->Profile == IProgram::glsl300esv)
		{
			check(found == nullptr, "exactly one generated source expected");
			found = src;
		}
	}
	return found;
}

// The three landscape programs actually shipped (Far0 / Far1 / Tile / Tile+lightmap),
// converted in a given mode. These are the programs the zviewer A/B on real zone art
// was measured with, so this is a test over real shipped shader source, not a mock.
static void testLandscapeProgram(CLandscapeVBAllocator::TType type, bool lightMap,
								bool linked, const std::string &tag, bool expectClamp)
{
	CVertexProgramLandscape vp(type, lightMap);

	check(vp.getSourceNb() == 1, tag + ": program starts with one nelvp source");

	IProgram::CSource *src = convertAndFetch(vp, linked, false);
	if (src == nullptr)
	{
		std::cout << "  (no source generated for " << tag << ", remaining checks skipped)" << std::endl;
		return;
	}


	const std::string glsl(src->SourcePtr, src->SourceLen);
	dumpIfAsked(tag, glsl);

	// --- profile / metadata the driver relies on ------------------------------
	check(src->Profile == (linked ? IProgram::glsl300esv : IProgram::glsl330v),
		tag + ": profile must match the link mode");
	check(std::string(src->DisplayName) == "nelvp-converted",
		tag + ": DisplayName is the name the driver logs in linkPrograms");
	check(src->Features.UsesCameraUBO,
		tag + ": ecPos epilogue reads inverseProjectionBasis from the camera UBO");
	check(src->Features.NelvpRegisterCount == 16,
		tag + ": c[0..12] must round up to a std140 vec4 multiple of 16");
	check(!src->Features.OnlyUBOs,
		tag + ": OnlyUBOs is deliberately NOT set - callers keep using setUniform4f");
	check(src->Features.VPVertexFormat != 0,
		tag + ": VPVertexFormat must describe the used input attributes");

	std::map<sint, NLMISC::CSmartPtr<CUniformBufferFormat> >::iterator ubIt
		= src->UniformBufferFormats.find(UBBindingVertexProgram);
	check(ubIt != src->UniformBufferFormats.end(),
		tag + ": constant registers need a UBO format at the user VP binding");
	if (ubIt != src->UniformBufferFormats.end() && ubIt->second != nullptr)
		check(ubIt->second->Name == "NlNelvpConstants",
			tag + ": UBO block name");

	// the friendly-name -> register map the driver uses instead of glGetUniformLocation
	check(src->ParamIndices.size() == 6, tag + ": ParamIndices carried over");
	std::map<std::string, uint>::iterator pIt = src->ParamIndices.find("fog");
	check(pIt != src->ParamIndices.end() && pIt->second == 10,
		tag + ": ParamIndices['fog'] == 10");

	// --- the generated code ---------------------------------------------------
	checkHas(glsl, linked ? "#version 300 es" : "#version 330", tag + ": version header");
	if (!linked)
		checkHas(glsl, "GL_ARB_separate_shader_objects", tag + ": SSO extension");

	checkHas(glsl, "layout(location = 11) in vec4 v11;",
		tag + ": attributes are always emitted as vec4 at explicit locations "
			"(the landscape reads v[11] as Float3, GL fills w=1)");

	// the two lines that looked wrong and are not (checked one by one in AGENTS_trackD.md):
	checkHas(glsl, "c[4].yyyx",
		tag + ": MAD R1, R1, c[4].yyyx, v[0] is the documented 'set w to 1' trick");
	checkHas(glsl, "fog.x = dot(c[10], R1);",
		tag + ": DP4 o[FOGC].x, c[10], R1 must become a scalar component assign");

	// fog varying: declared in linked mode because the mega PP declares every input
	// (WebGL2 needs a VP output for each PP input), and in SSO mode because the
	// program really writes it.
	checkHas(glsl, "out vec4 fog;", tag + ": fog varying declared");

	// what a hand-written landscape VP would have to reproduce (AGENTS_trackD.md):
	checkHas(glsl, "ecPos = inverseProjectionBasis * gl_Position;",
		tag + ": mega PP fog and ppClip both consume ecPos, the converter synthesises it");
	// The clamp is conditional on outputUsed[]: only Far1 writes a colour output
	// (o[COL0].w, the alpha blend), the other three leave o[COL0] alone on purpose
	// ("leave o[COL0] undefined because the material don't care diffuse RGBA here",
	// landscapevb_allocator.cpp:556). Asserting both halves is what pins that.
	if (expectClamp)
		checkHas(glsl, "clamp(diffuseColor, 0.0, 1.0);", tag + ": ARB VP1.0 clamps a written colour output");
	else
		checkNot(glsl, "clamp(diffuseColor", tag + ": no colour clamp when COL0 is unwritten");
}

// ----------------------------------------------------------------------------
// The hand-written GLSL/UBO landscape programs (CVertexProgramLandscapeUBO), the ones
// the GL3 driver actually uses for Far0/Far1/Tile since D.1b. They are NOT produced
// by the converter, so nothing else in this file covers them.
//
// Why a CPU test and not a frame: in the zviewer scene on real zone art (zone 8_FJ),
// the far passes contribute no pixels - forcing the Far1 alpha to a constant, offsetting
// Far1 geometry by 40 clip units and collapsing all Far1 vertices onto one point each
// give AE 0 against the unmodified frame. The programs compile and link (id=63
// VP=glsl300esv/Landscape/far1/ubo) but the tile pass already covers that geometry, so
// no shader change in the Far body can show up. The Far half of D.1b is therefore
// asserted at source level here, and stays unverified on pixels until a scene with
// visible far landscape is used.
static void testUBOLandscapeProgram(CLandscapeVBAllocator::TType type, bool lightMap,
											bool linked, const std::string &tag)
{
	CVertexProgramLandscapeUBO vp(type, lightMap);

	check(vp.getSourceNb() == 2, tag + ": must ship both link modes (linked + SSO)");

	// pick by profile rather than by index, so the test does not depend on ctor order
	IProgram::CSource *src = nullptr;
	for (size_t i = 0; i < vp.getSourceNb(); ++i)
	{
		IProgram::CSource *cand = vp.getSource(i);
		if (cand != nullptr && cand->Profile == (linked ? IProgram::glsl300esv : IProgram::glsl330v))
			src = cand;
	}
	if (src == nullptr)
	{
		std::cout << "  (no " << (linked ? "glsl300esv" : "glsl330v") << " source for " << tag << ")" << std::endl;
		return;
	}

	const std::string glsl(src->SourcePtr, src->SourceLen);
	dumpIfAsked(tag, glsl);

	const bool far1 = (type == CLandscapeVBAllocator::Far1);
	const std::string typeName = (type == CLandscapeVBAllocator::Far0) ? "far0" :
		(far1 ? "far1" : "tile");

	// --- metadata the driver keys off -------------------------------------------
	check(std::string(src->DisplayName) ==
		std::string(linked ? "glsl300esv/Landscape/" : "glsl330v/Landscape/") + typeName +
		(lightMap ? "/ubo/lightmap" : "/ubo"),
		tag + ": DisplayName is what linkPrograms logs (the nelvp path logs 'nelvp-converted' for everything)");

	std::map<sint, NLMISC::CSmartPtr<CUniformBufferFormat> >::iterator ubIt
		= src->UniformBufferFormats.find(UBBindingVertexProgram);
	check(ubIt != src->UniformBufferFormats.end() && ubIt->second != nullptr &&
		ubIt->second->Name == "NlLandscape",
		tag + ": data must arrive through the user UBO at the VP binding");
	check(src->Features.OnlyUBOs,
		tag + ": OnlyUBOs must be set - this program has no setUniform4f fallback");
	check(src->Features.UsesObjectUBO,
		tag + ": modelViewProjection comes from the builtin NlModel UBO");
	check(src->Features.UsesCameraUBO,
		tag + ": the ecPos epilogue reads inverseProjectionBasis from the NlCamera UBO");

	// no converter leftovers
	checkNot(glsl, "NlNelvpConstants", tag + ": must not carry the converter constant block");
	checkNot(glsl, "nelvp-converted", tag + ": must not be labelled as converted");

	// --- the compile-time switches ---------------------------------------------
	check(has(glsl, "#define NL_LANDSCAPE_FAR1") == far1,
		tag + ": only Far1 may define NL_LANDSCAPE_FAR1");
	check(has(glsl, "#define NL_LANDSCAPE_LIGHTMAP") == lightMap,
		tag + ": only the Tile lightmap variant may define NL_LANDSCAPE_LIGHTMAP");
	check(!has(glsl, "#define NL_LANDSCAPE_FAR1") || !has(glsl, "#define NL_LANDSCAPE_LIGHTMAP"),
		tag + ": the two macros read v[12] differently and must never co-occur");

	// --- shared body -----------------------------------------------------------
	checkHas(glsl, "gl_Position = modelViewProjection * R1;", tag + ": projection");
	checkHas(glsl, "ecPos = inverseProjectionBasis * gl_Position;", tag + ": fog epilogue (mega PP needs ecPos)");
	checkHas(glsl, "float geomBlend = clamp(max(errorMetric, modifiedMetric) - 1.0, 0.0, 1.0);",
		tag + ": geomorph blend");

	// --- the Far1-only alpha fade ---------------------------------------------
	// this is the whole of what Far1 adds over Tile/Far0, and the pixel that the
	// nelvp writes as o[COL0].w. Mirrors the converter's own emission for
	// NL3D_LandscapeFar1EndProgram (see the landscape/Far1/linked dump).
	// The body is SHARED: every variant's source text contains the fade, inside
	// "#ifdef NL_LANDSCAPE_FAR1". Plain text presence therefore proves nothing -
	// what decides whether it compiles is the define (asserted above), and what
	// has to be checked here is that the fade sits INSIDE that guard.
	const std::string guarded = ifdefBlock(glsl, "NL_LANDSCAPE_FAR1");
	check(!guarded.empty(),
		tag + ": the NL_LANDSCAPE_FAR1 block must exist in the shared body");
	check(has(guarded, "diffuseColor.w = clamp((sqrDist - v12.x) * v12.y, 0.0, 1.0);"),
		tag + ": the far-tile alpha fade of NL3D_LandscapeFar1EndProgram must be inside that guard");
	check(has(guarded, "diffuseColor = clamp(diffuseColor, 0.0, 1.0);"),
		tag + ": o[COL0] clamp, as the converter emits it once a program writes COL0");
	checkNot(stripIfdefBlocks(glsl, "NL_LANDSCAPE_FAR1"), "diffuseColor.w =",
		tag + ": no variant may write the far alpha outside the Far1 guard");
	if (!linked)
		check(has(glsl, "layout(location = 3) smooth out vec4 diffuseColor;") == far1,
			tag + ": SSO must declare diffuseColor (VaryingLocationDiffuseColor) for Far1 only");

	// --- the input attribute set must match what the body reads -----------------
	// g_VertexFlags maps attribute 12 -> TexCoord4Flag (driver_opengl3_program.cpp:83-100),
	// so a mismatch means the driver binds a VB layout the shader does not declare.
	const bool needsV12 = (lightMap || far1);
	check(((src->Features.VPVertexFormat & CVertexBuffer::TexCoord4Flag) != 0) == needsV12,
		tag + ": TexCoord4Flag must be advertised exactly when the body reads v[12]");
}

// The shipped source of one UBO landscape variant in a given link mode, or "" if the
// program does not ship that mode.
static std::string uboLandscapeSource(CVertexProgramLandscapeUBO &vp, bool linked)
{
	for (size_t i = 0; i < vp.getSourceNb(); ++i)
	{
		IProgram::CSource *cand = vp.getSource(i);
		if (cand != nullptr && cand->Profile == (linked ? IProgram::glsl300esv : IProgram::glsl330v))
			return std::string(cand->SourcePtr, cand->SourceLen);
	}
	return "";
}

// Far0 and Tile must stay THE SAME PROGRAM: NL3D_LandscapeFar0EndProgram and
// NL3D_LandscapeTileEndProgram are instruction-for-instruction identical, and the plain
// Tile variant reads no more inputs than Far0 (the extra v[12] channel its vertex buffer
// carries is only read by the lightmap pass, which sets TexCoord4Flag). That equivalence
// is the whole reason D.1b ships one body instead of four, so pin it against the source
// constants in landscapevb_allocator.cpp rather than trusting the prose comment there.
static void testFar0EqualsTile()
{
	for (int linked = 0; linked < 2; ++linked)
	{
		const bool l = (linked != 0);
		const std::string tag = l ? "ubo/landscape/Far0==Tile/linked" : "ubo/landscape/Far0==Tile/sso";

		CVertexProgramLandscapeUBO far0(CLandscapeVBAllocator::Far0, false);
		CVertexProgramLandscapeUBO tile(CLandscapeVBAllocator::Tile, false);

		const std::string a = uboLandscapeSource(far0, l);
		const std::string b = uboLandscapeSource(tile, l);
		check(!a.empty() && !b.empty(), tag + ": both programs must ship this link mode");
		check(a == b, tag + ": the two programs must be the same source text (one body, no type-specific code)");

		// the same must hold for the attribute contract, or the driver would bind a
		// different vertex layout for the same shader
		IProgram::CSource *fa = nullptr, *ft = nullptr;
		for (size_t i = 0; i < far0.getSourceNb(); ++i)
			if (far0.getSource(i) != nullptr && far0.getSource(i)->Profile == (l ? IProgram::glsl300esv : IProgram::glsl330v))
				fa = far0.getSource(i);
		for (size_t i = 0; i < tile.getSourceNb(); ++i)
			if (tile.getSource(i) != nullptr && tile.getSource(i)->Profile == (l ? IProgram::glsl300esv : IProgram::glsl330v))
				ft = tile.getSource(i);
		check(fa != nullptr && ft != nullptr && fa->Features.VPVertexFormat == ft->Features.VPVertexFormat,
			tag + ": identical VPVertexFormat");
		check(fa != nullptr && ft != nullptr && fa->Features.OnlyUBOs == ft->Features.OnlyUBOs,
			tag + ": identical OnlyUBOs flag");
	}
}

static void testClipVariant()
{
	CVertexProgramLandscape vp(CLandscapeVBAllocator::Tile, false);
	IProgram::CSource *src = convertAndFetch(vp, true, true);
	if (src == nullptr)
		return;
	const std::string glsl(src->SourcePtr, src->SourceLen);
	dumpIfAsked("landscape/Tile/clip", glsl);

	checkHas(glsl, "#extension GL_EXT_clip_cull_distance : enable",
		"clip variant: ES profile needs the extension for gl_ClipDistance");
	checkHas(glsl, "gl_ClipDistance[5] = dot(clipPlane5, ecPos);",
		"clip variant: all 6 planes folded on nlClipPlaneMask");
	checkHas(glsl, "gl_ClipDistance[5] = 1.0;",
		"clip variant: disabled planes get a neutral distance");
}

static void testColourClamp()
{
	// The ARB VP1.0 clamp is emitted only for a program that actually writes a
	// colour output; a program that does not must not get one.
	CVertexProgram vp(
		"!!VP1.0\n"
		"	MAD\tR0.xyzw, v[0].x, c[0].x, c[0].y;\n"
		"	MOV\to[COL0], R0;\n"
		"	MOV\to[HPOS], R0;\n"
		"	END\n"
		);

	IProgram::CSource *src = convertAndFetch(vp, true, false);
	if (src == nullptr)
		return;
	const std::string glsl(src->SourcePtr, src->SourceLen);
	dumpIfAsked("colour-clamp", glsl);

	checkHas(glsl, "clamp(diffuseColor, 0.0, 1.0);",
		"ARB VP1.0 clamps o[COL0] once the program writes it");
}

static void testIndexedConstants()
{
	// The vegetation LUT shape (vegetable_manager.cpp NL3D_FastBendProgram): indexed
	// constant access c[A0.x+32]. The converter cannot know which register it reads,
	// so it must size the constant UBO for the whole nelvp range and declare A0x.
	CVertexProgram vp(
		"!!VP1.0\n"
		"	MAD\tR0.x, c[17].x, v[9].z, v[9].y;\n"
		"	ARL\tA0.x, R0.x;\n"
		"	EXP\tR0.y, R0.x;\n"
		"	MAD\tR0.xyzw, R0.y, c[A0.x+32].zwww, c[A0.x+32].xyww;\n"
		"	MOV\to[HPOS], R0;\n"
		"	END\n"
		);

	IProgram::CSource *src = convertAndFetch(vp, true, false);
	if (src == nullptr)
		return;
	const std::string glsl(src->SourcePtr, src->SourceLen);
	dumpIfAsked("indexed-constants", glsl);

	check(src->Features.NelvpRegisterCount == 96,
		"indexed access: register count must cover NELVP_NUM_CONSTANTS (96)");
	checkHas(glsl, "int A0x = 0;", "indexed access: address register must be declared");
	// The index is clamped to the constant register range, which is what keeps
	// ARL-driven access inside the generated UBO (NELVP_NUM_CONSTANTS = 96).
	checkHas(glsl, "c[clamp(A0x + (32), 0, 95)]", "indexed access: index must be clamped to the UBO range");
	check(src->Features.NelvpRegisterCount == 96,
		"indexed access: must force the full 96-register UBO");
	checkNot(glsl, "/* ", "indexed access: no comment-as-LHS in the output");
}

static void testUnsupportedOutput()
{
	// o[BFC0] has no GLSL varying in this converter. Nothing shipped writes it, so
	// this is a contract test on a path reachable only from a hand-written or
	// future program - which is exactly the D.1/D.2 case.
	CVertexProgram vp(
		"!!VP1.0\n"
		"	MOV\to[BFC0], v[0];\n"
		"	MOV\to[HPOS], v[0];\n"
		"	END\n"
		);

	IProgram::CSource *src = convertAndFetch(vp, true, false);
	if (src == nullptr)
		return;
	const std::string glsl(src->SourcePtr, src->SourceLen);
	dumpIfAsked("unsupported-output", glsl);

	// The converter used to emit the comment itself as the LHS of the assignment:
	// '/* unsupported output 3 */ = v0;' is a GLSL syntax error, so any program
	// writing o[BFC0] would have failed to compile at runtime rather than render
	// wrong. It now writes to a declared discard sink instead.
	checkNot(glsl, "/* unsupported output", "unsupported output must not become a comment LHS");
	checkHas(glsl, "vec4 _nelvpDroppedOut = vec4(0.0);",
		"unsupported output must land in a declared sink");
	checkHas(glsl, "_nelvpDroppedOut = v0;", "unsupported output: the write is redirected, not deleted");
}


// ----------------------------------------------------------------------------
// The vegetation programs (the fallback for drivers without builtin UBOs, and the
// only path for the lit passes). Same rule as the landscape cases: convert the real
// shipped source, not a mock, so a converter regression cannot silently remove the
// fallback. The composition comes from buildVegetableNelvpSource() - the function the
// manager itself calls - so this cannot drift from what the engine ships.
// ----------------------------------------------------------------------------
namespace NL3D { void buildVegetableNelvpSource(uint vpType, bool fogEnabled, std::string &vpgram); }

static void testVegetableProgram(uint vpType, bool fogEnabled, const std::string &tag)
{
	NL3D::CVertexProgram vp;
	IProgram::CSource *nelvpSrc = new IProgram::CSource();
	nelvpSrc->Profile = IProgram::nelvp;
	nelvpSrc->DisplayName = "nelvp/Veget";
	std::string body;
	NL3D::buildVegetableNelvpSource(vpType, fogEnabled, body);
	nelvpSrc->setSource(body);
	vp.addSource(nelvpSrc);

	check(vp.getSourceNb() == 1, tag + ": program starts with one nelvp source");

	IProgram::CSource *src = convertAndFetch(vp, true, false);
	if (src == nullptr)
	{
		std::cout << "  (no source generated for " << tag << ", remaining checks skipped)" << std::endl;
		return;
	}

	const std::string glsl(src->SourcePtr, src->SourceLen);
	dumpIfAsked(tag, glsl);

	// --- what the driver needs to link it with the mega PP --------------------
	check(src->Profile == IProgram::glsl300esv, tag + ": linked profile");
	check(src->Features.UsesCameraUBO, tag + ": fog epilogue reads inverseProjectionBasis");
	check(src->Features.VPVertexFormat != 0, tag + ": input attributes described");

	// --- the two behaviours the hand-written twin had to reproduce -------------
	// (1) RSQ is emitted as inversesqrt(abs(x)) - the bend normalisation depends on it.
	check(glsl.find("inversesqrt") != std::string::npos, tag + ": RSQ -> inversesqrt");
	// (2) VP1.0 colour outputs are clamped; without this the z-sort fade alpha
	//     exceeds 1 and extra alpha-tested geometry passes (measured: 11385 px).
	check(glsl.find("clamp(diffuseColor") != std::string::npos,
		tag + ": colour outputs must stay clamped");
	// (3) the unlit passes read the wind LUT through a dynamic constant-register index
	//     (ARL/A0); the lit passes use the quaternion bend instead and have no LUT.
	const bool lit = (vpType == NL3D_VEGETABLE_RDRPASS_LIGHTED ||
	                  vpType == NL3D_VEGETABLE_RDRPASS_LIGHTED_2SIDED);
	if (!lit)
		check(glsl.find("A0x") != std::string::npos, tag + ": LUT index goes through ARL/A0");
	else
		check(glsl.find("A0x") == std::string::npos, tag + ": the lit bend has no LUT index");

	if (vpType == NL3D_VEGETABLE_RDRPASS_UNLIT_2SIDED_ZSORT)
		check(glsl.find("c[11].x") != std::string::npos,
			tag + ": the z-sort pass must fade on distance (c[11].x)");
	// o[FOGC] is deliberately dropped by the converter: the mega pixel program computes fog
	// per fragment from ecPos. Asserting its absence pins that behaviour (it is what makes
	// the hand-written twin and the nelvp path agree).
	check(glsl.find("FOGC") == std::string::npos, tag + ": converter drops the nelvp fog coordinate");
}


// ----------------------------------------------------------------------------
// Compressed texture support, per format.
//
// This is the one piece of driver_opengl3_extension.cpp that can be tested on any box:
// resolveCompressedTextureSupport() is a pure function over the extension names, so the
// extension sets advertised by Windows (S3TC), macOS (S3TC), Linux/Mesa (S3TC) and the
// GLES/WebGL/Android subsets (EXT_dxt1, ANGLE_dxt*) are all exercised here, without a
// context and without the platform.
// ----------------------------------------------------------------------------
static void testCompressedTextureSupport()
{
	const char *s3tc[] = { "GL_EXT_texture_compression_s3tc", "GL_ARB_texture_compression",
		"GL_EXT_texture_filter_anisotropic", NULL };
	const char *extDxt1[] = { "GL_EXT_texture_compression_dxt1", "GL_OES_compressed_paletted_texture", NULL };
	const char *angle[] = { "GL_ANGLE_texture_compression_dxt3",
		"GL_ANGLE_texture_compression_dxt5", NULL };
	const char *angleDxt1[] = { "GL_ANGLE_texture_compression_dxt1", NULL };
	const char *legacyS3[] = { "GL_S3_s3tc", NULL };
	const char *none[] = { "GL_ARB_multitexture", NULL };
	const char *empty[] = { NULL };

	struct { const char **list; bool d1, d3, d5; const char *tag; } cases[] = {
		{ s3tc,      true,  true,  true,  "S3TC (desktop GL: Windows / macOS / Linux)" },
		{ legacyS3,  true,  true,  true,  "GL_S3_s3tc (old S3 drivers, the original TODO)" },
		{ extDxt1,   true,  false, false, "GL_EXT_texture_compression_dxt1 only" },
		{ angleDxt1, true,  false, false, "ANGLE_texture_compression_dxt1 only" },
		{ angle,     false, true,  true,  "ANGLE dxt3 + dxt5 without dxt1" },
		{ none,      false, false, false, "no compression extension" },
		{ empty,     false, false, false, "empty extension list" }
	};

	for (uint c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c)
	{
		std::vector<const char *> glext;
		for (const char **e = cases[c].list; *e != NULL; ++e)
			glext.push_back(*e);

		NLDRIVERGL3::CGlExtensions ext; // the ctor must leave everything off
		NLDRIVERGL3::resolveCompressedTextureSupport(glext, ext);

		const std::string tag = std::string("dxt/") + cases[c].tag;
		check(ext.CompressedDXT1 == cases[c].d1, tag + ": DXT1");
		check(ext.CompressedDXT3 == cases[c].d3, tag + ": DXT3");
		check(ext.CompressedDXT5 == cases[c].d5, tag + ": DXT5");
		check(ext.EXTTextureCompressionS3TC == (cases[c].list == s3tc),
			tag + ": the S3TC flag tracks the desktop extension exactly");
	}

	// A driver that grants nothing must not claim anything even if a *different* subset of
	// the family is present in a later call: the resolver always assigns, never ORs in.
	{
		std::vector<const char *> g; g.push_back("GL_EXT_texture_compression_s3tc");
		NLDRIVERGL3::CGlExtensions ext;
		NLDRIVERGL3::resolveCompressedTextureSupport(g, ext);
		std::vector<const char *> g2; g2.push_back("GL_ARB_multitexture");
		NLDRIVERGL3::resolveCompressedTextureSupport(g2, ext);
		check(!ext.CompressedDXT1 && !ext.CompressedDXT3 && !ext.CompressedDXT5,
			"dxt/re-resolve: a later call with no DXT extension clears the flags");
	}
}

static void testRefusals()
{
	// A program with no nelvp source: nothing to convert, must say so and add nothing.
	CVertexProgram noNelvp;
	{
		IProgram::CSource *src = new IProgram::CSource();
		src->Profile = IProgram::glsl330v;
		src->setSource("#version 330\nvoid main(){}\n");
		noNelvp.addSource(src);
	}
	check(!NLDRIVERGL3::CDriverGL3::convertNelvpToGLSL(&noNelvp, true, false),
		"a program with no nelvp source must refuse");
	check(noNelvp.getSourceNb() == 1, "refusal must not add a source");

	// Unparseable source: the parser must refuse before any GLSL is built.
	CVertexProgram garbage("!!VP1.0\n\tNOTANINSTRUCTION R0;\n\tEND\n");
	check(!NLDRIVERGL3::CDriverGL3::convertNelvpToGLSL(&garbage, true, false),
		"unparseable nelvp must refuse");
	check(garbage.getSourceNb() == 1, "parse failure must not add a source");

	// Parses fine but no such opcode exists in the converter's switch.
	CVertexProgram unknownOp("!!VP1.0\n\tFOG R0, R1;\n\tEND\n");
	check(!NLDRIVERGL3::CDriverGL3::convertNelvpToGLSL(&unknownOp, true, false),
		"an opcode the converter does not implement must refuse, not emit partial GLSL");
	check(unknownOp.getSourceNb() == 1, "unsupported opcode must not add a source");

	// Why the converter's *source*-side unsupported branches are dead code: the parser
	// never lets an output or address register appear as a source operand at all
	// (CVPParser::parseOp2, vertex_program_parse.cpp:674-678). Pinned here so that if that
	// guard is ever relaxed, the converter's rvalue emission has to be revisited too.
	CVertexProgram srcOutReg("!!VP1.0\n"
		"	MOV\tR0.xyzw, v[0];\n"
		"	RCP\tR1.x, o[COL0].x;\n"
		"	MOV\to[HPOS], R0;\n"
		"	END\n");
	check(!NLDRIVERGL3::CDriverGL3::convertNelvpToGLSL(&srcOutReg, true, false),
		"an output register as a SOURCE must be refused by the parser");
	CVertexProgram srcAddrReg("!!VP1.0\n"
		"	MOV\tR0.xyzw, v[0];\n"
		"	RCP\tR1.x, a0.x;\n"
		"	MOV\to[HPOS], R0;\n"
		"	END\n");
	check(!NLDRIVERGL3::CDriverGL3::convertNelvpToGLSL(&srcAddrReg, true, false),
		"an address register as a scalar SOURCE must be refused by the parser");
}

// ----------------------------------------------------------------------------
sint main(sint argc, char *argv[])
{

	testCompressedTextureSupport();

	// Track D.2: the vegetation nelvp programs are the fallback for drivers without
	// builtin UBO support - convert all shipped variants.
	testVegetableProgram(NL3D_VEGETABLE_RDRPASS_UNLIT, false, "veg/unlit");
	testVegetableProgram(NL3D_VEGETABLE_RDRPASS_UNLIT, true, "veg/unlit+fog");
	testVegetableProgram(NL3D_VEGETABLE_RDRPASS_UNLIT_2SIDED, false, "veg/unlit2s");
	testVegetableProgram(NL3D_VEGETABLE_RDRPASS_UNLIT_2SIDED_ZSORT, false, "veg/unlitzsort");
	testVegetableProgram(NL3D_VEGETABLE_RDRPASS_UNLIT_2SIDED_ZSORT, true, "veg/unlitzsort+fog");
	testVegetableProgram(NL3D_VEGETABLE_RDRPASS_LIGHTED, false, "veg/lighted");
	testVegetableProgram(NL3D_VEGETABLE_RDRPASS_LIGHTED_2SIDED, true, "veg/lighted2s+fog");
	gDump = (argc > 1 && std::string(argv[1]) == "--dump");

	std::cout << "nelvp -> GLSL converter test" << std::endl;

	// The four shipped landscape programs, in both link modes. m_LinkedMegaShaders is
	// true on desktop GL 3.3 too (driver_opengl3.cpp:370/374), so the linked path is
	// the live one; SSO is the fallback after a mega-program link failure (:630).
	const CLandscapeVBAllocator::TType types[] =
		{ CLandscapeVBAllocator::Far0, CLandscapeVBAllocator::Far1, CLandscapeVBAllocator::Tile };

	for (sint t = 0; t < 3; ++t)
	{
		std::string name("landscape/");
		name += (t == 0) ? "Far0" : ((t == 1) ? "Far1" : "Tile");
		// only Far1 writes o[COL0].w
		testLandscapeProgram(types[t], false, true, name + "/linked", t == 1);
		testLandscapeProgram(types[t], false, false, name + "/sso", t == 1);

		// and the hand-written UBO twin the GL3 driver uses instead of all of the above
		std::string uname("ubo/landscape/");
		uname += (t == 0) ? "Far0" : ((t == 1) ? "Far1" : "Tile");
		testUBOLandscapeProgram(types[t], false, true, uname + "/linked");
		testUBOLandscapeProgram(types[t], false, false, uname + "/sso");
	}
	// the lightmap twin of Tile is the 4th program the allocator creates
	testLandscapeProgram(CLandscapeVBAllocator::Tile, true, true, "landscape/Tile/lightmap", false);
	testUBOLandscapeProgram(CLandscapeVBAllocator::Tile, true, true, "ubo/landscape/Tile/lightmap");

	testClipVariant();
	testFar0EqualsTile();
	testColourClamp();
	testIndexedConstants();
	testUnsupportedOutput();
	testRefusals();

	std::cout << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
	if (gFailures != 0)
	{
		std::cout << gFailures << " FAILURE(S)" << std::endl;
		return 1;
	}
	return 0;
}
