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

#include <nel/3d/landscapevb_allocator.h>
#include <nel/3d/program.h>
#include <nel/3d/uniform_buffer_format.h>
#include <nel/3d/vertex_program.h>

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
}

// ----------------------------------------------------------------------------
sint main(sint argc, char *argv[])
{
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
	}
	// the lightmap twin of Tile is the 4th program the allocator creates
	testLandscapeProgram(CLandscapeVBAllocator::Tile, true, true, "landscape/Tile/lightmap", false);

	testClipVariant();
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
