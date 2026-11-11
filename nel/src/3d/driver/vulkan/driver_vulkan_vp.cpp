// NeL - MMORPG Framework <https://wiki.ryzom.dev/>
// Copyright (C) 2026  The Ryzom Core Community
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

/**
 * User vertex programs for the Vulkan driver.
 *
 * Two kinds of engine programs are compiled here:
 *
 *   - nelvp assembly programs, converted to Vulkan GLSL by
 *     convertNelvpToGLSL() (ported from driver_opengl3_nelvp.cpp). The 96
 *     constant registers become the NlNelvpConstants UBO at binding 4 and
 *     the setUniform family and setUniformMatrix write straight into it.
 *
 *   - GLSL vertex programs written by the engine (the wind tree, vegetable
 *     and landscape UBO programs, profile glsl330v). Their builtin UBO blocks
 *     and the blocks described by CSource::UniformBufferFormats are injected
 *     the same way the GL3 driver's insertBuiltinHeaders() does, and the
 *     program's main() is wrapped so the GL clip-space z can be remapped to
 *     the Vulkan [0, w] range.
 *
 * Either way the compiled shader replaces the mega vertex stage while the
 * program is active; the mega pixel stage and its descriptor layout stay
 * untouched, which is what the engine expects of a user vertex program.
 *
 * The glsl3vi "insert" profile (GPU skinning / MRM geomorph snippets) has no
 * shader of its own: the snippet defines nlPreTransform() and is spliced into
 * every mega vertex program variant, like the GL3 driver. The insert program's
 * UniformBufferFormats declare its user blocks (NlMorph at binding 4,
 * NlSkeleton at binding 6) which the draw stages into the descriptor ring.
 */

#include "stdvulkan.h"
#include "driver_vulkan.h"

#include <sstream>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "nel/3d/vertex_program.h"
#include "nel/3d/vertex_program_parse.h"
#include "nel/3d/uniform_buffer_format.h"

using namespace NLMISC;
using namespace NL3D;

namespace NLVULKAN {

namespace /* anonymous */ {

// ---------------------------------------------------------------------------
// Vertex program attribute and varying locations
// ---------------------------------------------------------------------------

/// Vertex shader input slots, in CVertexBuffer flag order used by nelvp's
/// v[0]..v[15] registers. The same numbering as the GL3 driver's TAttribOffset.
enum TAttribSlot
{
	SlotPosition,
	SlotWeight,
	SlotNormal,
	SlotPrimaryColor,
	SlotSecondaryColor,
	SlotFog,
	SlotPaletteSkin,
	SlotTangent,
	SlotTexCoord0,
	SlotTexCoord1,
	SlotTexCoord2,
	SlotTexCoord3,
	SlotTexCoord4,
	SlotTexCoord5,
	SlotTexCoord6,
	SlotTexCoord7,
	SlotCount
};

static const uint16 s_AttribFlags[SlotCount] =
{
	CVertexBuffer::PositionFlag,
	CVertexBuffer::WeightFlag,
	CVertexBuffer::NormalFlag,
	CVertexBuffer::PrimaryColorFlag,
	CVertexBuffer::SecondaryColorFlag,
	CVertexBuffer::FogFlag,
	CVertexBuffer::PaletteSkinFlag,
	0,
	CVertexBuffer::TexCoord0Flag,
	CVertexBuffer::TexCoord1Flag,
	CVertexBuffer::TexCoord2Flag,
	CVertexBuffer::TexCoord3Flag,
	CVertexBuffer::TexCoord4Flag,
	CVertexBuffer::TexCoord5Flag,
	CVertexBuffer::TexCoord6Flag,
	CVertexBuffer::TexCoord7Flag
};

/// The 16 generic varyings of the mega pixel program. Every user vertex
/// program declares all of them so the fragment stage's declared inputs always
/// have a matching vertex output (Vulkan interface matching, unlike GL SSO).
struct CVarying
{
	const char	*Name;
	const char	*Init;	// value before the program body runs
};

static const CVarying s_Varyings[16] =
{
	{ "ecPos",       "vec4(0.0)" },
	{ "vertexColor", "vec4(1.0)" },
	{ "normal",      "vec4(0.0, 0.0, 1.0, 0.0)" },
	{ "diffuseColor", "materialColor" },
	{ "specularColor", "vec4(0.0)" },
	{ "worldPos",    "vec4(0.0)" },
	{ "paletteSkin", "vec4(0.0)" },
	{ "tangent",     "vec4(0.0)" },
	{ "texCoord0",   "vec4(0.0)" },
	{ "texCoord1",   "vec4(0.0)" },
	{ "texCoord2",   "vec4(0.0)" },
	{ "texCoord3",   "vec4(0.0)" },
	{ "texCoord4",   "vec4(0.0)" },
	{ "texCoord5",   "vec4(0.0)" },
	{ "texCoord6",   "vec4(0.0)" },
	{ "texCoord7",   "vec4(0.0)" }
};

static const int VaryingEcPos = 0;
static const int VaryingDiffuseColor = 3;
static const int VaryingSpecularColor = 4;

/// nelvp output register -> generic varying location, or -1 when the output
/// has no Vulkan varying of its own. Fog is dropped: the mega pixel program
/// computes fog from the synthesized ecPos, so writing a fog varying would
/// only collide with worldPos at location 5.
static int nelvpOutputVarying(int outputRegister)
{
	switch (outputRegister)
	{
	case CVPOperand::OPrimaryColor: return VaryingDiffuseColor;
	case CVPOperand::OSecondaryColor: return VaryingSpecularColor;
	case CVPOperand::OTex0: return 8;
	case CVPOperand::OTex1: return 9;
	case CVPOperand::OTex2: return 10;
	case CVPOperand::OTex3: return 11;
	case CVPOperand::OTex4: return 12;
	case CVPOperand::OTex5: return 13;
	case CVPOperand::OTex6: return 14;
	case CVPOperand::OTex7: return 15;
	}
	return -1;
}

// ---------------------------------------------------------------------------
// nelvp -> GLSL
// ---------------------------------------------------------------------------

// Maximum number of nelvp constant registers (ARB_vertex_program spec).
static const int NELVP_NUM_CONSTANTS = 96;

static inline char swizzleChar(CVPSwizzle::EComp c)
{
	static const char s[] = { 'x', 'y', 'z', 'w' };
	return s[c];
}

// Write a swizzle suffix to the stream, truncated to numComponents.
// If numComponents == 4 and the swizzle is identity, nothing is written.
static void writeSwizzle(std::stringstream &ss, const CVPSwizzle &swz, int numComponents = 4)
{
	if (numComponents == 4 && swz.isIdentity())
		return;
	ss << '.';
	for (int i = 0; i < numComponents; ++i)
		ss << swizzleChar(swz.Comp[i]);
}

// Write a scalar swizzle (for scalar ops like RCP, RSQ).
static void writeScalarSwizzle(std::stringstream &ss, const CVPSwizzle &swz)
{
	ss << '.' << swizzleChar(swz.Comp[0]);
}

// Write a write mask suffix.
static void writeWriteMask(std::stringstream &ss, uint mask)
{
	if (mask == 0xF)
		return;
	ss << '.';
	if (mask & 1) ss << 'x';
	if (mask & 2) ss << 'y';
	if (mask & 4) ss << 'z';
	if (mask & 8) ss << 'w';
}

static int writeMaskCount(uint mask)
{
	int count = 0;
	if (mask & 1) ++count;
	if (mask & 2) ++count;
	if (mask & 4) ++count;
	if (mask & 8) ++count;
	return count;
}

static bool destHasGLSLName(const CVPOperand &op)
{
	if (op.Type == CVPOperand::Variable || op.Type == CVPOperand::AddressRegister)
		return true;
	if (op.Type != CVPOperand::OutputRegister)
		return false;
	const int reg = (int)op.Value.OutputRegisterValue;
	if (reg == CVPOperand::OHPosition || reg == CVPOperand::OPointSize)
		return true;
	return nelvpOutputVarying(reg) >= 0;
}

// Write a source operand reference. numComponents matches the destination
// write mask width for component-wise ops.
static void writeSrcOperand(std::stringstream &ss, const CVPOperand &op, int registerCount, int numComponents = 4)
{
	if (op.Negate)
		ss << "(-";

	switch (op.Type)
	{
	case CVPOperand::Variable:
		ss << "R" << op.Value.VariableValue;
		break;
	case CVPOperand::Constant:
		if (op.Indexed)
			ss << "c[clamp(A0x + (" << op.Value.ConstantValue << "), 0, " << (registerCount - 1) << ")]";
		else
			ss << "c[" << op.Value.ConstantValue << "]";
		break;
	case CVPOperand::InputRegister:
		ss << "v" << (int)op.Value.InputRegisterValue;
		break;
	case CVPOperand::OutputRegister:
		{
			const int reg = (int)op.Value.OutputRegisterValue;
			const int varying = nelvpOutputVarying(reg);
			if (reg == CVPOperand::OHPosition)
				ss << "gl_Position";
			else if (reg == CVPOperand::OPointSize)
				ss << "gl_PointSize";
			else if (varying >= 0)
				ss << s_Varyings[varying].Name;
			else
				ss << "/* unsupported output " << reg << " */vec4(0.0)";
		}
		break;
	case CVPOperand::AddressRegister:
		ss << "A0x";
		break;
	default:
		ss << "/* unknown operand */vec4(0.0)";
		break;
	}

	if (op.Type != CVPOperand::AddressRegister)
		writeSwizzle(ss, op.Swizzle, numComponents);

	if (op.Negate)
		ss << ")";
}

static void writeScalarSrcOperand(std::stringstream &ss, const CVPOperand &op, int registerCount)
{
	if (op.Negate)
		ss << "(-";

	switch (op.Type)
	{
	case CVPOperand::Variable:
		ss << "R" << op.Value.VariableValue;
		break;
	case CVPOperand::Constant:
		if (op.Indexed)
			ss << "c[clamp(A0x + (" << op.Value.ConstantValue << "), 0, " << (registerCount - 1) << ")]";
		else
			ss << "c[" << op.Value.ConstantValue << "]";
		break;
	case CVPOperand::InputRegister:
		ss << "v" << (int)op.Value.InputRegisterValue;
		break;
	default:
		ss << "/* unknown operand */vec4(0.0)";
		break;
	}

	writeScalarSwizzle(ss, op.Swizzle);

	if (op.Negate)
		ss << ")";
}

// Write a destination operand (LHS of an assignment). Outputs without a
// Vulkan varying write to a discard sink.
static void writeDestOperand(std::stringstream &ss, const CVPOperand &op)
{
	switch (op.Type)
	{
	case CVPOperand::Variable:
		ss << "R" << op.Value.VariableValue;
		break;
	case CVPOperand::OutputRegister:
		{
			const int reg = (int)op.Value.OutputRegisterValue;
			const int varying = nelvpOutputVarying(reg);
			if (reg == CVPOperand::OHPosition)
				ss << "gl_Position";
			else if (reg == CVPOperand::OPointSize)
				ss << "gl_PointSize";
			else if (varying >= 0)
				ss << s_Varyings[varying].Name;
			else
				ss << "_nelvpDroppedOut";
		}
		break;
	case CVPOperand::AddressRegister:
		ss << "A0x";
		return;
	default:
		ss << "_nelvpDroppedOut";
		return;
	}

	writeWriteMask(ss, op.WriteMask);
}

// Assign a scalar expression to a destination, broadcast to the masked
// components.
static void writeBroadcastAssign(std::stringstream &ss, const CVPOperand &dest,
	const std::string &scalarExpr)
{
	const int maskCount = writeMaskCount(dest.WriteMask);

	if (dest.Type == CVPOperand::OutputRegister
		&& dest.Value.OutputRegisterValue == CVPOperand::OPointSize)
	{
		ss << "gl_PointSize = " << scalarExpr << ";\n";
		return;
	}

	writeDestOperand(ss, dest);
	ss << " = ";

	if (dest.WriteMask == 0xF)
		ss << "vec4(" << scalarExpr << ")";
	else if (maskCount == 1)
		ss << scalarExpr;
	else
	{
		switch (maskCount)
		{
		case 2: ss << "vec2(" << scalarExpr << ")"; break;
		case 3: ss << "vec3(" << scalarExpr << ")"; break;
		default: ss << "vec4(" << scalarExpr << ")"; break;
		}
	}
	ss << ";\n";
}

/// Convert a nelvp program's assembly source to Vulkan GLSL.
///
/// Returns false when the program has no usable nelvp source. The generated
/// source declares all 16 generic varyings (the mega pixel program reads a
/// subset), the NlNelvpConstants UBO at binding 4, and the epilogue that
/// rebuilds NeL eye-space ecPos from gl_Position so the pixel stage's fog
/// still works.
static bool convertNelvpToGLSL(CVertexProgram *program, std::string &outSource, uint32 &outRegisterCount,
	bool &outSpecular, uint16 &outWrittenTexCoords)
{
	IProgram::CSource *nelvpSrc = NULL;
	for (int i = 0; i < (int)program->getSourceNb(); ++i)
	{
		IProgram::CSource *s = program->getSource(i);
		if (s->Profile == IProgram::nelvp)
		{
			nelvpSrc = s;
			break;
		}
	}
	if (!nelvpSrc || !nelvpSrc->SourcePtr)
		return false;

	CVPParser parser;
	CVPParser::TProgram parsed;
	std::string parseError;
	if (!parser.parse(nelvpSrc->SourcePtr, parsed, parseError))
	{
		nlwarning("Vulkan: nelvp parse error in '%s': %s", nelvpSrc->DisplayName.c_str(), parseError.c_str());
		return false;
	}

	// Analyse register usage.
	bool inputUsed[SlotCount];
	bool outputUsed[CVPOperand::OutputRegisterCount];
	bool tempUsed[12];
	bool addressUsed = false;
	bool droppedOutputUsed = false;
	int highestConstant = -1;
	memset(inputUsed, 0, sizeof(inputUsed));
	memset(outputUsed, 0, sizeof(outputUsed));
	memset(tempUsed, 0, sizeof(tempUsed));

	for (size_t i = 0; i < parsed.size(); ++i)
	{
		const CVPInstruction &instr = parsed[i];
		const uint numSrc = instr.getNumUsedSrc();

		if (instr.Dest.Type == CVPOperand::OutputRegister)
			outputUsed[instr.Dest.Value.OutputRegisterValue] = true;
		else if (instr.Dest.Type == CVPOperand::Variable)
			tempUsed[instr.Dest.Value.VariableValue] = true;
		else if (instr.Dest.Type == CVPOperand::AddressRegister)
			addressUsed = true;

		if (!destHasGLSLName(instr.Dest))
			droppedOutputUsed = true;

		for (uint s = 0; s < numSrc; ++s)
		{
			const CVPOperand &src = instr.getSrc(s);
			if (src.Type == CVPOperand::InputRegister)
			{
				if ((int)src.Value.InputRegisterValue < SlotCount)
					inputUsed[(int)src.Value.InputRegisterValue] = true;
			}
			else if (src.Type == CVPOperand::Variable)
				tempUsed[src.Value.VariableValue] = true;
			else if (src.Type == CVPOperand::Constant)
			{
				if (src.Indexed)
					highestConstant = NELVP_NUM_CONSTANTS - 1;
				else if (src.Value.ConstantValue > highestConstant)
					highestConstant = src.Value.ConstantValue;
			}
		}
	}

	int registerCount;
	if (nelvpSrc->Features.NelvpRegisterCount > 0)
		registerCount = nelvpSrc->Features.NelvpRegisterCount;
	else
		registerCount = std::max(highestConstant + 1, 4);
	registerCount = (registerCount + 3) & ~3;
	outRegisterCount = (uint32)registerCount;

	// Texture coordinates the program writes. A vertex buffer only describes
	// the inputs, so the pixel stage needs this to know that texCoordN exists.
	outWrittenTexCoords = 0;
	for (int i = 0; i < 8; ++i)
		if (outputUsed[CVPOperand::OTex0 + i])
			outWrittenTexCoords |= (uint16)(CVertexBuffer::TexCoord0Flag << i);

	std::stringstream ss;
	ss << "#version 450\n";
	ss << "// Vulkan nelvp vertex program converted from '" << nelvpSrc->DisplayName << "'\n";
	ss << "\n";

	// The builtin UBO blocks, same declarations the mega shaders use.
	ss << "// Builtin UBO blocks (bindings 0-3)\n";
	ss << buildVulkanLightTableHeader((sint)NL_VK_MEGA_MAX_LIGHT_TABLE);
	ss << VulkanCameraHeader;
	ss << VulkanObjectHeader;
	ss << VulkanMaterialHeader;
	ss << "\n";

	// Vertex inputs, same locations as the engine vertex buffer layout.
	for (int i = 0; i < SlotCount; ++i)
		if (inputUsed[i])
			ss << "layout(location = " << i << ") in vec4 v" << i << ";\n";
	ss << "\n";

	// All generic outputs, so the pixel stage's declared inputs always match.
	for (int i = 0; i < 16; ++i)
		ss << "layout(location = " << i << ") smooth out vec4 " << s_Varyings[i].Name << ";\n";
	ss << "\n";

	// nelvp constant registers.
	ss << "layout(std140, set = 0, binding = " << NL_VK_MEGA_BINDING_USER_VERTEX_PROGRAM
	   << ") uniform NlNelvpConstants {\n";
	ss << "    vec4 c[" << registerCount << "];\n";
	ss << "};\n";
	ss << "\n";

	ss << "void main() {\n";

	// Deterministic defaults for outputs the program does not write.
	for (int i = 0; i < 16; ++i)
		ss << "  " << s_Varyings[i].Name << " = " << s_Varyings[i].Init << ";\n";
	ss << "\n";

	for (int i = 0; i < 12; ++i)
		if (tempUsed[i])
			ss << "  vec4 R" << i << " = vec4(0.0);\n";
	if (addressUsed)
		ss << "  int A0x = 0;\n";
	if (droppedOutputUsed)
		ss << "  vec4 _nelvpDroppedOut = vec4(0.0); // sink: nelvp writes with no Vulkan varying\n";
	ss << "\n";

	// Translate instructions.
	for (size_t i = 0; i < parsed.size(); ++i)
	{
		const CVPInstruction &instr = parsed[i];

		switch (instr.Opcode)
		{
		case CVPInstruction::MOV:
			{
				const int mc = writeMaskCount(instr.Dest.WriteMask);
				writeDestOperand(ss, instr.Dest);
				ss << " = ";
				writeSrcOperand(ss, instr.Src1, registerCount, mc);
				ss << ";\n";
			}
			break;

		case CVPInstruction::ARL:
			ss << "  A0x = int(floor(";
			writeScalarSrcOperand(ss, instr.Src1, registerCount);
			ss << "));\n";
			break;

		case CVPInstruction::ADD:
			{
				const int mc = writeMaskCount(instr.Dest.WriteMask);
				writeDestOperand(ss, instr.Dest);
				ss << " = ";
				writeSrcOperand(ss, instr.Src1, registerCount, mc);
				ss << " + ";
				writeSrcOperand(ss, instr.Src2, registerCount, mc);
				ss << ";\n";
			}
			break;

		case CVPInstruction::MUL:
			{
				const int mc = writeMaskCount(instr.Dest.WriteMask);
				writeDestOperand(ss, instr.Dest);
				ss << " = ";
				writeSrcOperand(ss, instr.Src1, registerCount, mc);
				ss << " * ";
				writeSrcOperand(ss, instr.Src2, registerCount, mc);
				ss << ";\n";
			}
			break;

		case CVPInstruction::MAD:
			{
				const int mc = writeMaskCount(instr.Dest.WriteMask);
				writeDestOperand(ss, instr.Dest);
				ss << " = ";
				writeSrcOperand(ss, instr.Src1, registerCount, mc);
				ss << " * ";
				writeSrcOperand(ss, instr.Src2, registerCount, mc);
				ss << " + ";
				writeSrcOperand(ss, instr.Src3, registerCount, mc);
				ss << ";\n";
			}
			break;

		case CVPInstruction::DP3:
			{
				std::stringstream expr;
				expr << "dot(";
				writeSrcOperand(expr, instr.Src1, registerCount);
				expr << ".xyz, ";
				writeSrcOperand(expr, instr.Src2, registerCount);
				expr << ".xyz)";
				writeBroadcastAssign(ss, instr.Dest, expr.str());
			}
			break;

		case CVPInstruction::DP4:
			{
				std::stringstream expr;
				expr << "dot(";
				writeSrcOperand(expr, instr.Src1, registerCount);
				expr << ", ";
				writeSrcOperand(expr, instr.Src2, registerCount);
				expr << ")";
				writeBroadcastAssign(ss, instr.Dest, expr.str());
			}
			break;

		case CVPInstruction::DST:
			ss << "  { vec4 _dst = vec4(1.0, ";
			writeSrcOperand(ss, instr.Src1, registerCount);
			ss << ".y * ";
			writeSrcOperand(ss, instr.Src2, registerCount);
			ss << ".y, ";
			writeSrcOperand(ss, instr.Src1, registerCount);
			ss << ".z, ";
			writeSrcOperand(ss, instr.Src2, registerCount);
			ss << ".w);\n  ";
			writeDestOperand(ss, instr.Dest);
			ss << " = _dst";
			writeWriteMask(ss, instr.Dest.WriteMask);
			ss << "; }\n";
			break;

		case CVPInstruction::LIT:
			ss << "  { vec4 _lit_src = ";
			writeSrcOperand(ss, instr.Src1, registerCount);
			ss << ";\n";
			ss << "  vec4 _lit = vec4(1.0, max(_lit_src.x, 0.0), "
			      "(_lit_src.x > 0.0) ? exp2(clamp(_lit_src.w, -128.0, 128.0) * log2(max(_lit_src.y, 1e-30))) : 0.0, "
			      "1.0);\n  ";
			writeDestOperand(ss, instr.Dest);
			ss << " = _lit";
			writeWriteMask(ss, instr.Dest.WriteMask);
			ss << "; }\n";
			break;

		case CVPInstruction::MIN:
			{
				const int mc = writeMaskCount(instr.Dest.WriteMask);
				writeDestOperand(ss, instr.Dest);
				ss << " = min(";
				writeSrcOperand(ss, instr.Src1, registerCount, mc);
				ss << ", ";
				writeSrcOperand(ss, instr.Src2, registerCount, mc);
				ss << ");\n";
			}
			break;

		case CVPInstruction::MAX:
			{
				const int mc = writeMaskCount(instr.Dest.WriteMask);
				writeDestOperand(ss, instr.Dest);
				ss << " = max(";
				writeSrcOperand(ss, instr.Src1, registerCount, mc);
				ss << ", ";
				writeSrcOperand(ss, instr.Src2, registerCount, mc);
				ss << ");\n";
			}
			break;

		case CVPInstruction::SLT:
		case CVPInstruction::SGE:
			{
				const bool less = instr.Opcode == CVPInstruction::SLT;
				const int mc = writeMaskCount(instr.Dest.WriteMask);
				writeDestOperand(ss, instr.Dest);
				if (mc == 1)
				{
					ss << " = (";
					writeSrcOperand(ss, instr.Src1, registerCount, 1);
					ss << (less ? " < " : " >= ");
					writeSrcOperand(ss, instr.Src2, registerCount, 1);
					ss << ") ? 1.0 : 0.0;\n";
				}
				else
				{
					static const char *vecCast[] = { NULL, NULL, "vec2", "vec3", "vec4" };
					ss << " = " << vecCast[mc] << "(" << (less ? "lessThan(" : "greaterThanEqual(");
					writeSrcOperand(ss, instr.Src1, registerCount, mc);
					ss << ", ";
					writeSrcOperand(ss, instr.Src2, registerCount, mc);
					ss << "));\n";
				}
			}
			break;

		case CVPInstruction::RCP:
			{
				std::stringstream expr;
				expr << "(1.0 / ";
				writeScalarSrcOperand(expr, instr.Src1, registerCount);
				expr << ")";
				writeBroadcastAssign(ss, instr.Dest, expr.str());
			}
			break;

		case CVPInstruction::RSQ:
			{
				std::stringstream expr;
				expr << "inversesqrt(abs(";
				writeScalarSrcOperand(expr, instr.Src1, registerCount);
				expr << "))";
				writeBroadcastAssign(ss, instr.Dest, expr.str());
			}
			break;

		case CVPInstruction::EXPP:
			ss << "  { float _exp_s = ";
			writeScalarSrcOperand(ss, instr.Src1, registerCount);
			ss << ";\n";
			ss << "  vec4 _exp = vec4(exp2(floor(_exp_s)), fract(_exp_s), exp2(_exp_s), 1.0);\n  ";
			writeDestOperand(ss, instr.Dest);
			ss << " = _exp";
			writeWriteMask(ss, instr.Dest.WriteMask);
			ss << "; }\n";
			break;

		case CVPInstruction::LOG:
			ss << "  { float _log_a = abs(";
			writeScalarSrcOperand(ss, instr.Src1, registerCount);
			ss << ");\n";
			ss << "  float _log_f = log2(max(_log_a, 1e-30));\n";
			ss << "  vec4 _log = vec4(floor(_log_f), _log_a * exp2(-floor(_log_f)), _log_f, 1.0);\n  ";
			writeDestOperand(ss, instr.Dest);
			ss << " = _log";
			writeWriteMask(ss, instr.Dest.WriteMask);
			ss << "; }\n";
			break;

		default:
			ss << "  /* unknown opcode " << (int)instr.Opcode << " */\n";
			break;
		}
	}

	// VP1.0 clamps the colour outputs to [0, 1].
	if (outputUsed[CVPOperand::OPrimaryColor])
		ss << "  diffuseColor = clamp(diffuseColor, 0.0, 1.0);\n";
	if (outputUsed[CVPOperand::OSecondaryColor])
		ss << "  specularColor = clamp(specularColor, 0.0, 1.0);\n";

	// Epilogue. ecPos must be NeL eye-space: the pixel stage uses ecPos.y as
	// the forward fog depth. inv(Projection * ChangeBasis) * gl_Position gives
	// exactly ModelView * position. gl_Position is still in the GL clip range
	// here, so this runs before the Vulkan z remap.
	ss << "\n";
	ss << "  // Synthesize the varyings the program itself has no output for\n";
	ss << "  ecPos = inverseProjectionBasis * gl_Position;\n";
	ss << "  worldPos = vec4(transpose(mat3(viewMatrix)) * (ecPos.xyz - viewMatrix[3].xyz * ecPos.w), ecPos.w);\n";
	if (inputUsed[SlotNormal])
		ss << "  normal = vec4(transpose(mat3(viewMatrix)) * normalize(mat3(modelView) * v" << SlotNormal << ".xyz), 0.0);\n";
	ss << "  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n";
	ss << "}\n";

	outSpecular = outputUsed[CVPOperand::OSecondaryColor];
	outSource = ss.str();
	return true;
}

// ---------------------------------------------------------------------------
// User UBO block declarations
// ---------------------------------------------------------------------------

static const char *s_UBTypeKeyword[] =
{
	"float",   // Float
	"vec2",    // FloatVec2
	"vec3",    // FloatVec3
	"vec4",    // FloatVec4
	"int",     // SInt
	"ivec2",   // SIntVec2
	"ivec3",   // SIntVec3
	"ivec4",   // SIntVec4
	"uint",    // UInt
	"uvec2",   // UIntVec2
	"uvec3",   // UIntVec3
	"uvec4",   // UIntVec4
	"bool",    // Boolean
	"bvec2",   // BooleanVec2
	"bvec3",   // BooleanVec3
	"bvec4",   // BooleanVec4
	"mat2",    // FloatMat2
	"mat3",    // FloatMat3
	"mat4",    // FloatMat4
	"mat2x3",  // FloatMat2x3
	"mat2x4",  // FloatMat2x4
	"mat3x2",  // FloatMat3x2
	"mat3x4",  // FloatMat3x4
	"mat4x2",  // FloatMat4x2
	"mat4x3",  // FloatMat4x3
};

/// Emit the std140 block of a CUniformBufferFormat at a Vulkan binding.
/// Ported from the GL3 driver's generateUniformBufferGLSL(), with the
/// explicit set/binding that Vulkan requires.
static bool generateUserUBO(std::stringstream &ss, const CUniformBufferFormat &ubf, uint binding)
{
	if (ubf.structCount() > 0)
	{
		nlwarning("Vulkan: user uniform block '%s' uses structs, unsupported by the vertex program compiler",
			ubf.Name.c_str());
		return false;
	}

	const std::string blockName = ubf.Name.empty()
		? ("NlUBO" + toString((int)binding))
		: ubf.Name;
	ss << "layout(std140, set = 0, binding = " << binding << ") uniform " << blockName << "\n";
	ss << "{\n";
	for (sint i = 0; i < (sint)ubf.count(); ++i)
	{
		const CUniformBufferFormat::CEntry &entry = ubf.get(i);
		ss << "    " << s_UBTypeKeyword[entry.Type] << " " << CStringMapper::unmap(entry.Name);
		if (entry.Count != 1)
			ss << "[" << entry.Count << "]";
		ss << ";\n";
	}
	ss << "};\n";
	return true;
}

} /* anonymous namespace */

/// Builtin UBO blocks + the engine's own user UBO blocks. Both the raw GLSL
/// programs assembled here and the insert mega VP variants assembled by
/// assembleMegaSource() (driver_vulkan_mega_render.cpp) use it, so the block
/// order and bindings cannot drift apart.
std::string buildVertexProgramUBOHeaders(const std::map<sint, CSmartPtr<CUniformBufferFormat> > &formats,
	bool &ok)
{
	ok = true;
	std::stringstream ss;
	ss << "// Builtin UBO blocks (bindings 0-3)\n";
	ss << buildVulkanLightTableHeader((sint)NL_VK_MEGA_MAX_LIGHT_TABLE);
	ss << VulkanCameraHeader;
	ss << VulkanObjectHeader;
	ss << VulkanMaterialHeader;

	for (std::map<sint, CSmartPtr<CUniformBufferFormat> >::const_iterator it = formats.begin();
		it != formats.end(); ++it)
	{
		if (!it->second)
			continue;
		const uint binding = NL_VK_MEGA_BINDING_USER_VERTEX_PROGRAM + (uint)it->first;
		if (it->first < 0 || binding > NL_VK_MEGA_BINDING_USER_SKELETON)
		{
			nlwarning("Vulkan: user UBO binding %d is out of range for a vertex program", (int)it->first);
			ok = false;
			return ss.str();
		}
		if (!generateUserUBO(ss, *it->second, binding))
		{
			ok = false;
			return ss.str();
		}
	}
	return ss.str();
}

namespace /* anonymous */ {

// ---------------------------------------------------------------------------
// Raw GLSL vertex programs
// ---------------------------------------------------------------------------

/// True when the source already declares an output of that name (all engine
/// GLSL vertex programs declare their outputs as vec4).
static bool declaresOutput(const std::string &src, const char *name)
{
	std::string needle = std::string("out vec4 ") + name;
	size_t at = 0;
	while ((at = src.find(needle, at)) != std::string::npos)
	{
		const size_t after = at + needle.size();
		if (after >= src.size() || !(isalnum((unsigned char)src[after]) || src[after] == '_'))
			return true;
		at = after;
	}
	return false;
}

/// Remove a line-level `#version` / `#extension` directive and the
/// `out gl_PerVertex { ... };` redeclaration GL sources carry. Vulkan takes
/// gl_Position as a builtin and has no use for the GL extensions.
static std::string stripGLOnlyDeclarations(const std::string &src)
{
	std::string out;
	size_t pos = 0;
	while (pos < src.size())
	{
		size_t lineEnd = src.find('\n', pos);
		if (lineEnd == std::string::npos)
			lineEnd = src.size();
		const std::string line = src.substr(pos, lineEnd - pos);
		size_t first = line.find_first_not_of(" \t");
		const bool isVersion = first != std::string::npos
			&& line.compare(first, 8, "#version") == 0;
		const bool isExtension = first != std::string::npos
			&& line.compare(first, 10, "#extension") == 0;
		if (isVersion || isExtension)
		{
			pos = lineEnd < src.size() ? lineEnd + 1 : src.size();
			continue;
		}
		out.append(line);
		if (lineEnd < src.size())
			out.push_back('\n');
		pos = lineEnd < src.size() ? lineEnd + 1 : src.size();
	}

	// gl_PerVertex redeclaration, possibly spanning lines.
	size_t at;
	while ((at = out.find("out gl_PerVertex")) != std::string::npos)
	{
		const size_t end = out.find("};", at);
		if (end == std::string::npos)
			break;
		out.erase(at, end + 2 - at);
	}
	return out;
}

/// True when a GLSL source declares a plain (non-block) uniform. Vulkan
/// requires global uniforms to live in uniform blocks, so such a source cannot
/// compile here; when the same program also has a nelvp source the conversion
/// path handles it (the bloom effect's texture-offset program is one).
static bool sourceHasPlainUniforms(const std::string &src)
{
	size_t pos = 0;
	while (pos < src.size())
	{
		size_t lineEnd = src.find('\n', pos);
		if (lineEnd == std::string::npos)
			lineEnd = src.size();
		std::string line = src.substr(pos, lineEnd - pos);
		pos = lineEnd < src.size() ? lineEnd + 1 : src.size();
		size_t first = line.find_first_not_of(" \t");
		if (first == std::string::npos || line.compare(first, 8, "uniform ") != 0)
			continue;
		// A block declaration has an opening brace on the same line.
		if (line.find('{') != std::string::npos)
			continue;
		return true;
	}
	return false;
}

/// Turn an engine GLSL vertex program (written for GL 3.3 SSO) into Vulkan
/// GLSL: prepend #version 450, the builtin and user UBO blocks, declare the
/// missing generic outputs and wrap main() for the clip-depth remap.
static bool buildRawGLSLVertexProgram(IProgram::CSource *src, std::string &outSource)
{
	std::string headers;
	bool headersOk = true;
	headers = buildVertexProgramUBOHeaders(src->UniformBufferFormats, headersOk);
	if (!headersOk)
		return false;

	std::string body = stripGLOnlyDeclarations(std::string(src->SourcePtr, src->SourceLen));

	// Rename the engine's entry point; the wrapper below calls it.
	size_t mainAt = body.find("void main()");
	size_t mainLen = strlen("void main()");
	if (mainAt == std::string::npos)
	{
		mainAt = body.find("void main(void)");
		mainLen = strlen("void main(void)");
	}
	if (mainAt == std::string::npos)
	{
		nlwarning("Vulkan: GLSL vertex program '%s' has no main(), cannot wrap it", src->DisplayName.c_str());
		return false;
	}
	body.replace(mainAt, mainLen, "void nlRawUserMain()");

	// Vulkan requires a vertex output for every fragment input the mega pixel
	// program declares. The engine's SSO programs only declare what they use.
	std::stringstream out;
	out << "#version 450\n";
	out << "// Vulkan user vertex program from '" << src->DisplayName << "'\n";
	out << headers;
	out << body;
	if (body.empty() || body[body.size() - 1] != '\n')
		out << "\n";
	out << "\n// Generic outputs the pixel stage may read but this program does not write\n";
	for (int i = 0; i < 16; ++i)
	{
		if (declaresOutput(body, s_Varyings[i].Name))
			continue;
		out << "layout(location = " << i << ") smooth out vec4 " << s_Varyings[i].Name << ";\n";
	}

	out << "\nvoid main()\n{\n";
	for (int i = 0; i < 16; ++i)
		out << "    " << s_Varyings[i].Name << " = " << s_Varyings[i].Init << ";\n";
	out << "    nlRawUserMain();\n";
	out << "    // NeL's projection writes the OpenGL clip depth range [-w, w];\n";
	out << "    // the Vulkan viewport expects [0, w].\n";
	out << "    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n";
	out << "}\n";

	outSource = out.str();
	return true;
}

/// Add the qualifiers Vulkan requires to a GLSL fragment program written for
/// GL SSO: an explicit location on the colour output, explicit locations on
/// the known varying inputs and explicit bindings on the sampler uniforms.
/// The engine sources are shared with the GL drivers, so the rewrite happens
/// here instead of changing them.
static std::string addVulkanFragmentQualifiers(const std::string &src)
{
	std::string out;
	std::vector<std::string> locals;
	size_t pos = 0;
	while (pos < src.size())
	{
		size_t lineEnd = src.find('\n', pos);
		if (lineEnd == std::string::npos)
			lineEnd = src.size();
		std::string line = src.substr(pos, lineEnd - pos);
		size_t first = line.find_first_not_of(" \t");
		const std::string indent = first == std::string::npos ? std::string() : line.substr(0, first);
		const std::string trimmed = first == std::string::npos ? std::string() : line.substr(first);

		if (trimmed == "out vec4 fragColor;")
		{
			line = indent + "layout(location = 0) out vec4 fragColor;";
		}
		else
		{
			bool varying = false;
			for (int i = 0; i < 16; ++i)
			{
				const std::string name = s_Varyings[i].Name;
				if (trimmed == "in vec4 " + name + ";" ||
					trimmed == "smooth in vec4 " + name + ";" ||
					trimmed == "flat in vec4 " + name + ";")
				{
					const std::string renamed = "nlIn_" + name;
					line = indent + "layout(location = " + NLMISC::toString(i) + ") smooth in vec4 " + renamed + ";";
					// A user PP is a full-screen pass that samples a render
					// target. Render targets are stored top-down in Vulkan while
					// NeL texture coordinates have V = 0 at the bottom, so the
					// interpolated texture coordinates are flipped here. This is
					// the same correction the builtin pixel program applies per
					// render-target stage.
					if (i >= 8)
						locals.push_back("    vec4 " + name + " = vec4(" + renamed + ".x, 1.0 - " + renamed + ".y, " + renamed + ".zw);");
					else
						locals.push_back("    vec4 " + name + " = " + renamed + ";");
					varying = true;
					break;
				}
			}
			if (!varying)
			{
				for (uint i = 0; i < NL_VK_MEGA_SAMPLER_COUNT; ++i)
				{
					const std::string name = "sampler" + NLMISC::toString((int)i);
					if (trimmed == "uniform sampler2D " + name + ";" ||
						trimmed == "uniform samplerCube " + name + ";")
					{
						const bool cube = trimmed.find("samplerCube") != std::string::npos;
						line = indent + "layout(set = 0, binding = " +
							NLMISC::toString((int)(NL_VK_MEGA_BINDING_SAMPLER_BASE + i)) + ") uniform " +
							(cube ? "samplerCube " : "sampler2D ") + name + ";";
						break;
					}
				}
			}
		}
		out += line;
		if (lineEnd < src.size())
			out.push_back('\n');
		pos = lineEnd < src.size() ? lineEnd + 1 : src.size();
	}

	// Define the flipped varyings at the top of main(). The renamed inputs are
	// only there so the body keeps using the original names.
	if (!locals.empty())
	{
		size_t mainAt = out.find("void main");
		size_t brace = mainAt == std::string::npos ? std::string::npos : out.find('{', mainAt);
		if (brace != std::string::npos)
		{
			std::string block;
			for (size_t i = 0; i < locals.size(); ++i)
				block += locals[i] + "\n";
			out.insert(brace + 1, "\n" + block);
		}
	}
	return out;
}

/// Assemble a user pixel program for Vulkan. Only UBO-formatted GLSL sources
/// are supported (plain uniforms cannot live outside a block in Vulkan); the
/// user block is bound where the engine's bindUniformBuffer() puts it.
static bool buildRawGLSLFragmentProgram(IProgram::CSource *src, std::string &outSource)
{
	if (src->UniformBufferFormats.empty())
	{
		nlwarning("Vulkan: pixel program '%s' has no uniform buffer format", src->DisplayName.c_str());
		return false;
	}

	bool headersOk = true;
	std::string headers = buildVertexProgramUBOHeaders(src->UniformBufferFormats, headersOk);
	if (!headersOk)
		return false;

	std::string body = stripGLOnlyDeclarations(std::string(src->SourcePtr, src->SourceLen));
	body = addVulkanFragmentQualifiers(body);

	std::stringstream out;
	out << "#version 450\n";
	out << "// Vulkan user pixel program from '" << src->DisplayName << "'\n";
	out << headers;
	out << body;
	if (body.empty() || body[body.size() - 1] != '\n')
		out << "\n";
	outSource = out.str();
	return true;
}

} /* anonymous namespace */

// ---------------------------------------------------------------------------
// CProgramDrvInfosVulkan
// ---------------------------------------------------------------------------

CProgramDrvInfosVulkan::CProgramDrvInfosVulkan(CDriverVulkan *drv, ItGPUPrgDrvInfoPtrList it)
	: IProgramDrvInfos(drv, it)
	, _VkDriver(drv)
	, IsNelvpConverted(false)
	, RegisterCount(0)
	, Id(0)
	, FirstDrawLogged(false)
	, OutputsSpecularColor(false)
	, IsInsertProgram(false)
	, IsPixelProgram(false)
	, WrittenTexCoords(0)
{
}

CProgramDrvInfosVulkan::~CProgramDrvInfosVulkan()
{
	if (Id != 0 && _VkDriver)
	{
		// The driver may already have released its device; the release methods
		// only touch the shader maps, never the Vulkan device.
		if (IsPixelProgram)
			_VkDriver->releaseUserPPShader(Id);
		else
			_VkDriver->releaseUserVPShader(Id);
	}
}

uint CProgramDrvInfosVulkan::getUniformIndex(const char *name) const
{
	if (!IsNelvpConverted)
		return ~0u; // the supported GLSL programs only use UBOs

	std::map<std::string, uint>::const_iterator it = ParamIndices.find(name);
	if (it != ParamIndices.end())
		return it->second;

	// Generic constant register access.
	if (strncmp(name, "constant", 8) == 0)
	{
		const char *num = name + 8;
		if (*num >= '0' && *num <= '9')
		{
			const int idx = atoi(num);
			if (idx >= 0 && idx < 96)
				return (uint)idx;
		}
	}
	return ~0u;
}

// ---------------------------------------------------------------------------
// Program compilation / selection
// ---------------------------------------------------------------------------

void CDriverVulkan::releaseUserVPShader(uint32 id)
{
	std::map<uint32, VkShaderModule>::iterator it = _UserVPShaders.find(id);
	if (it != _UserVPShaders.end())
	{
		if (_Device && it->second != VK_NULL_HANDLE)
			vkDestroyShaderModule(_Device, it->second, NULL);
		_UserVPShaders.erase(it);
	}

	// glsl3vi insert programs own one mega VP variant per cache key.
	std::map<uint64, VkShaderModule>::iterator insertIt = _InsertVPShaders.begin();
	while (insertIt != _InsertVPShaders.end())
	{
		if ((uint32)(insertIt->first >> 2) != id)
		{
			++insertIt;
			continue;
		}
		if (_Device && insertIt->second != VK_NULL_HANDLE)
			vkDestroyShaderModule(_Device, insertIt->second, NULL);
		insertIt = _InsertVPShaders.erase(insertIt);
	}

	if (_ActiveUserVP && _ActiveUserVP->Id == id)
	{
		_ActiveUserVP = NULL;
		_NelvpActiveUB = NULL;
		_BoundUserUB[UBBindingVertexProgram] = NULL;
	}
}

void CDriverVulkan::releaseUserPPShader(uint32 id)
{
	std::map<uint32, VkShaderModule>::iterator it = _UserPPShaders.find(id);
	if (it != _UserPPShaders.end())
	{
		if (_Device && it->second != VK_NULL_HANDLE)
			vkDestroyShaderModule(_Device, it->second, NULL);
		_UserPPShaders.erase(it);
	}
	if (_ActiveUserPP && _ActiveUserPP->Id == id)
	{
		_ActiveUserPP = NULL;
		_BoundUserUB[UBBindingPixelProgram] = NULL;
	}
}

bool CDriverVulkan::compilePixelProgram(CPixelProgram *program)
{
	if (program->m_DrvInfo)
		return true;
	if (program->m_CompileFailed)
		return false;
	if (!_MegaResourcesReady)
		return false;

	// The glsl300esf profile is the UBO variant the engine provides for user
	// pixel programs (FXAA). The GL330 variant uses plain uniforms, which
	// Vulkan does not allow, so it is not a fallback here.
	IProgram::CSource *src = NULL;
	for (int i = 0; i < (int)program->getSourceNb(); ++i)
	{
		if (program->getSource(i)->Profile == IProgram::glsl300esf)
		{
			src = program->getSource(i);
			break;
		}
	}
	if (!src)
	{
		nlinfo("Vulkan: pixel program has no glsl300esf source");
		program->m_CompileFailed = true;
		return false;
	}

	std::string source;
	if (!buildRawGLSLFragmentProgram(src, source))
	{
		program->m_CompileFailed = true;
		return false;
	}

	VkShaderModule module = VK_NULL_HANDLE;
	if (!compileShader(source.c_str(), &module, false))
	{
		nlwarning("Vulkan: user pixel program '%s' failed to compile", src->DisplayName.c_str());
		program->m_CompileFailed = true;
		return false;
	}

	ItGPUPrgDrvInfoPtrList it = _GPUPrgDrvInfos.insert(_GPUPrgDrvInfos.end(), (IProgramDrvInfos *)NULL);
	CProgramDrvInfosVulkan *drvInfo = new CProgramDrvInfosVulkan(this, it);
	*it = drvInfo;
	drvInfo->Id = ++_NextUserPPId;
	drvInfo->DisplayName = src->DisplayName;
	drvInfo->IsPixelProgram = true;
	_UserPPShaders[drvInfo->Id] = module;
	program->m_DrvInfo = drvInfo;
	program->buildInfo(src);
	nlinfo("Vulkan: compiled user pixel program '%s'", src->DisplayName.c_str());
	return true;
}

bool CDriverVulkan::activePixelProgram(CPixelProgram *program)
{
	if (program == NULL)
	{
		_ActiveUserPP = NULL;
		return true;
	}
	if (!program->m_DrvInfo && !compilePixelProgram(program))
	{
		_ActiveUserPP = NULL;
		return false;
	}
	_ActiveUserPP = static_cast<CProgramDrvInfosVulkan *>((IProgramDrvInfos *)program->m_DrvInfo);
	return true;
}

bool CDriverVulkan::supportVertexProgram(CVertexProgram::TProfile profile) const
{
	// Developer escape hatch and A/B switch: behave like the pre-vertex-program
	// driver, letting the engine take its non-VP fallbacks.
	if (getenv("NLVK_NOVP"))
		return false;
	if (!_MegaResourcesReady)
		return false;

	switch (profile)
	{
	case IProgram::nelvp:
	case IProgram::glsl330v:
		return true;
	case IProgram::glsl3vi:
		// GPU skinning / MRM geomorph through the insert vertex program is
		// OPT-IN until it is validated. With the insert path enabled, the
		// character's equipment meshes (head, hair, gloves) are drawn displaced
		// and rotated while the CPU-skinned body is correct, and the MRM
		// geomorph path is equally unverified. The CPU path is the one both
		// reference drivers use and it is known-correct, so it is the default.
		// Run with NLVK_GPUSKIN=1 to exercise the insert path.
		return getenv("NLVK_GPUSKIN") != NULL;
	default:
		return false;
	}
}

bool CDriverVulkan::compileInsertVertexProgram(CVertexProgram *program, IProgram::CSource *insertSrc)
{
	// A glsl3vi source defines nlPreTransform() and the user UBO blocks it
	// reads; it has no main() and cannot be compiled on its own. Splice it
	// into the mega vertex program, as the GL3 driver does.
	const std::string insertSource(insertSrc->SourcePtr, insertSrc->SourceLen);
	if (insertSource.empty())
	{
		nlwarning("Vulkan: insert vertex program '%s' has an empty source", insertSrc->DisplayName.c_str());
		program->m_CompileFailed = true;
		return false;
	}

	// Variant selection matches the mega path this driver draws with: the
	// pixel stage is always the fog/PPL variant, and supportVertexProgramClipPlanes()
	// is false, so (fogOrPpl=1, hwClip=0) is the only selectable variant. The
	// cache key keeps the GL3 variant bits so the others can be added later.
	const uint fogOrPpl = 1;
	const uint hwClip = 0;

	std::string body;
	megavkVPGenerate(body, fogOrPpl != 0, insertSource.c_str());
	std::string source;
	if (!assembleMegaSource(body, &insertSrc->UniformBufferFormats, source))
	{
		nlwarning("Vulkan: insert vertex program '%s' could not be assembled for Vulkan",
			insertSrc->DisplayName.c_str());
		program->m_CompileFailed = true;
		return false;
	}

	VkShaderModule module = VK_NULL_HANDLE;
	if (!compileShader(source.c_str(), &module, true))
	{
		nlwarning("Vulkan: insert vertex program '%s' failed to compile (fogOrPpl=%u, hwClip=%u)",
			insertSrc->DisplayName.c_str(), fogOrPpl, hwClip);
		program->m_CompileFailed = true;
		return false;
	}

	ItGPUPrgDrvInfoPtrList it = _GPUPrgDrvInfos.insert(_GPUPrgDrvInfos.end(), (IProgramDrvInfos *)NULL);
	CProgramDrvInfosVulkan *drvInfo = new CProgramDrvInfosVulkan(this, it);
	*it = drvInfo;
	drvInfo->Id = ++_NextUserVPId;
	drvInfo->DisplayName = insertSrc->DisplayName;
	drvInfo->IsInsertProgram = true;
	// The mega vertex stage always writes specularColor.
	drvInfo->OutputsSpecularColor = true;
	_InsertVPShaders[insertVertexShaderKey(drvInfo->Id, fogOrPpl, hwClip)] = module;
	program->m_DrvInfo = drvInfo;
	program->buildInfo(insertSrc);
	nlinfo("Vulkan: compiled insert vertex program '%s' (mega VP, fogOrPpl=%u, hwClip=%u)",
		insertSrc->DisplayName.c_str(), fogOrPpl, hwClip);
	return true;
}

bool CDriverVulkan::compileVertexProgram(CVertexProgram *program)
{
	if (getenv("NLVK_NOVP"))
		return false;
	if (program->m_DrvInfo)
		return true;
	if (program->m_CompileFailed)
		return false;
	if (!_MegaResourcesReady)
		return false;

	// glsl3vi programs are snippets spliced into the mega vertex program, not
	// complete shaders; compile the mega VP variants they can select.
	for (int i = 0; i < (int)program->getSourceNb(); ++i)
	{
		if (program->getSource(i)->Profile == IProgram::glsl3vi)
			return compileInsertVertexProgram(program, program->getSource(i));
	}

	// Prefer a complete GLSL program when the engine provides one (wind tree,
	// vegetable, landscape). Otherwise convert the nelvp assembly.
	IProgram::CSource *glslSrc = NULL;
	IProgram::CSource *nelvpSrc = NULL;
	for (int i = 0; i < (int)program->getSourceNb(); ++i)
	{
		IProgram::CSource *s = program->getSource(i);
		if (s->Profile == IProgram::glsl330v && !glslSrc)
			glslSrc = s;
		else if (s->Profile == IProgram::nelvp && !nelvpSrc)
			nelvpSrc = s;
	}

	// A GLSL source with plain uniforms cannot compile for Vulkan, but the
	// program may also ship a nelvp source the converter handles (bloom's
	// texture-offset program does). Prefer that instead of failing first.
	if (glslSrc && nelvpSrc && glslSrc->SourcePtr &&
		sourceHasPlainUniforms(std::string(glslSrc->SourcePtr, glslSrc->SourceLen)))
	{
		nlinfo("Vulkan: GLSL vertex program '%s' uses plain uniforms; using its nelvp source",
			glslSrc->DisplayName.c_str());
		glslSrc = NULL;
	}

	std::string source;
	uint32 registerCount = 0;
	bool specular = false;

	if (glslSrc)
	{
		if (buildRawGLSLVertexProgram(glslSrc, source))
		{
			VkShaderModule module = VK_NULL_HANDLE;
			if (compileShader(source.c_str(), &module, true))
			{
				ItGPUPrgDrvInfoPtrList it = _GPUPrgDrvInfos.insert(_GPUPrgDrvInfos.end(), (IProgramDrvInfos *)NULL);
				CProgramDrvInfosVulkan *drvInfo = new CProgramDrvInfosVulkan(this, it);
				*it = drvInfo;
				drvInfo->Id = ++_NextUserVPId;
				drvInfo->DisplayName = glslSrc->DisplayName;
				drvInfo->OutputsSpecularColor = glslSrc->Features.OutputsSpecularColor;
				_UserVPShaders[drvInfo->Id] = module;
				program->m_DrvInfo = drvInfo;
				program->buildInfo(glslSrc);
				nlinfo("Vulkan: compiled user vertex program '%s' (GLSL)", glslSrc->DisplayName.c_str());
				return true;
			}
			nlwarning("Vulkan: GLSL vertex program '%s' failed to compile", glslSrc->DisplayName.c_str());
		}
		else
		{
			nlwarning("Vulkan: GLSL vertex program '%s' could not be assembled for Vulkan",
				glslSrc->DisplayName.c_str());
		}
	}

	if (nelvpSrc)
	{
		uint16 writtenTexCoords = 0;
		if (!convertNelvpToGLSL(program, source, registerCount, specular, writtenTexCoords))
		{
			program->m_CompileFailed = true;
			return false;
		}

		VkShaderModule module = VK_NULL_HANDLE;
		if (!compileShader(source.c_str(), &module, true))
		{
			nlwarning("Vulkan: converted nelvp vertex program '%s' failed to compile",
				nelvpSrc->DisplayName.c_str());
			program->m_CompileFailed = true;
			return false;
		}

		// Append the converted source to the program so features() and
		// source() describe what actually runs (as the GL3 driver does).
		IProgram::CSource *converted = new IProgram::CSource();
		converted->Profile = IProgram::glsl330v;
		converted->DisplayName = nelvpSrc->DisplayName + " (vulkan)";
		converted->setSource(source);
		converted->ParamIndices = nelvpSrc->ParamIndices;
		converted->Features = nelvpSrc->Features;
		converted->Features.NelvpRegisterCount = (uint16)registerCount;
		converted->Features.OutputsSpecularColor = specular;
		converted->Features.OutputsWorldSpacePosition = false;
		converted->Features.UsesCameraUBO = true;
		uint16 vertexFormat = 0;
		{
			// Inputs the converter declared are exactly the ones the program uses.
			for (int i = 0; i < SlotCount; ++i)
			{
				if (source.find(std::string("in vec4 v") + toString(i) + ";") != std::string::npos)
					vertexFormat |= s_AttribFlags[i];
			}
		}
		converted->Features.VPVertexFormat = vertexFormat;
		program->addSource(converted);

		CUniformBuffer *constants = new CUniformBuffer();
		constants->Format.Name = "NlNelvpConstants";
		constants->Format.push("c", CUniformBufferFormat::FloatVec4, (sint)registerCount);
		constants->UsageHint = CUniformBuffer::StreamDraw;
		void *mem = constants->lock();
		memset(mem, 0, registerCount * 16);
		constants->unlock();

		ItGPUPrgDrvInfoPtrList it = _GPUPrgDrvInfos.insert(_GPUPrgDrvInfos.end(), (IProgramDrvInfos *)NULL);
		CProgramDrvInfosVulkan *drvInfo = new CProgramDrvInfosVulkan(this, it);
		*it = drvInfo;
		drvInfo->Id = ++_NextUserVPId;
		drvInfo->DisplayName = nelvpSrc->DisplayName;
		drvInfo->IsNelvpConverted = true;
		drvInfo->RegisterCount = registerCount;
		drvInfo->NelvpConstantUB = constants;
		drvInfo->ParamIndices = nelvpSrc->ParamIndices;
		drvInfo->OutputsSpecularColor = specular;
		drvInfo->WrittenTexCoords = writtenTexCoords;
		_UserVPShaders[drvInfo->Id] = module;
		program->m_DrvInfo = drvInfo;
		program->buildInfo(converted);
		nlinfo("Vulkan: compiled user vertex program '%s' (nelvp, %u constant registers)",
			nelvpSrc->DisplayName.c_str(), registerCount);
		return true;
	}

	nlwarning("Vulkan: vertex program '%s' has no source this driver can compile",
		program->getSourceNb() == 0 ? "<unnamed>" : program->getSource(0)->DisplayName.c_str());
	program->m_CompileFailed = true;
	return false;
}

bool CDriverVulkan::activeVertexProgram(CVertexProgram *program)
{
	if (program == NULL)
	{
		_ActiveUserVP = NULL;
		_NelvpActiveUB = NULL;
		_VPSpecularOutput = true; // the builtin mega vertex program outputs it
		return true;
	}

	if (!program->m_DrvInfo && !compileVertexProgram(program))
	{
		_ActiveUserVP = NULL;
		_NelvpActiveUB = NULL;
		_VPSpecularOutput = true;
		return false;
	}

	CProgramDrvInfosVulkan *drvInfo = static_cast<CProgramDrvInfosVulkan *>(
		(IProgramDrvInfos *)program->m_DrvInfo);
	_ActiveUserVP = drvInfo;
	_NelvpActiveUB = drvInfo->IsNelvpConverted ? (CUniformBuffer *)drvInfo->NelvpConstantUB : NULL;
	_VPSpecularOutput = drvInfo->OutputsSpecularColor;
	// The bound user UBO is driver state, not program state: the landscape
	// activates its program before every draw but only binds the shared
	// NlLandscape block once per frame (GL3 keeps it bound the same way).
	return true;
}

// ---------------------------------------------------------------------------
// Per-program uniforms
// ---------------------------------------------------------------------------

bool CDriverVulkan::hasActiveUserVPS()
{
	if (!_ActiveUserVP || _ActiveUserVP->Id == 0)
		return false;
	if (_ActiveUserVP->IsInsertProgram)
	{
		// Insert programs run a mega VP variant; the draw selects the fog/PPL
		// variant (see compileInsertVertexProgram()).
		return _InsertVPShaders.find(insertVertexShaderKey(_ActiveUserVP->Id, 1, 0))
			!= _InsertVPShaders.end();
	}
	return _UserVPShaders.find(_ActiveUserVP->Id) != _UserVPShaders.end();
}

CUniformBuffer *CDriverVulkan::activeUserUB(TUBBinding binding)
{
	if (!_ActiveUserVP)
		return NULL;
	if (binding == UBBindingVertexProgram && _ActiveUserVP->IsNelvpConverted)
		return _ActiveUserVP->NelvpConstantUB;
	return _BoundUserUB[binding];
}

bool CDriverVulkan::stageUserUB(TUBBinding binding, VkDescriptorBufferInfo &info)
{
	CUniformBuffer *ub = activeUserUB(binding);
	const uint32 size = ub ? (uint32)ub->Format.size() : 0;
	if (!ub || size == 0 || !ub->data())
	{
		// Keep the descriptor valid for programs that declare no user block.
		info.buffer = _MegaUserBuffer;
		info.offset = 0;
		info.range = 256;
		return true;
	}

	// A block that has not been touched since it was copied into this frame's
	// ring is still there at the same offset: reuse the copy. The 12 KiB
	// skeleton block is bound once per skeleton and read by every one of its
	// skinned draws, so this removes a large per-draw memcpy.
	const uint bindingIndex = (uint)binding;
	if (_UserUBStageCache[bindingIndex] == ub &&
		_UserUBStageSize[bindingIndex] == size &&
		!ub->Touched)
	{
		info.buffer = _UniformRing;
		info.offset = _UserUBStageOffset[bindingIndex];
		info.range = size;
		return true;
	}

	const uint32 offset = allocUniformBlock(ub->data(), size);
	if (offset == 0xFFFFFFFF)
	{
		nlwarning("Vulkan: uniform ring exhausted while uploading a user vertex program block");
		return false;
	}
	info.buffer = _UniformRing;
	info.offset = offset;
	info.range = size;
	_UserUBStageCache[bindingIndex] = ub;
	_UserUBStageOffset[bindingIndex] = offset;
	_UserUBStageSize[bindingIndex] = size;
	ub->Touched = false;
	return true;
}

/// Write one vec4 into the active nelvp constant UBO. Returns false when no
/// converted nelvp program is active.
static bool writeNelvpConstant(CUniformBuffer *ub, uint index, const float v[4])
{
	if (!ub || index == ~0u)
		return false;
	const uint32 size = (uint32)ub->Format.size();
	if ((uint64)index * 16 + 16 > size)
	{
		nlwarning("Vulkan: nelvp uniform index %u is outside the %u-byte constant block", index, size);
		return true;
	}
	ub->lock();
	ub->set((sint)(index * 16), v[0], v[1], v[2], v[3]);
	ub->unlock();
	return true;
}

void CDriverVulkan::setUniform1f(TProgram program, uint index, float f0)
{
	if (program != VertexProgram) return;
	const float v[4] = { f0, 0.f, 0.f, 0.f };
	writeNelvpConstant(_NelvpActiveUB, index, v);
}

void CDriverVulkan::setUniform2f(TProgram program, uint index, float f0, float f1)
{
	if (program != VertexProgram) return;
	const float v[4] = { f0, f1, 0.f, 0.f };
	writeNelvpConstant(_NelvpActiveUB, index, v);
}

void CDriverVulkan::setUniform3f(TProgram program, uint index, float f0, float f1, float f2)
{
	if (program != VertexProgram) return;
	const float v[4] = { f0, f1, f2, 0.f };
	writeNelvpConstant(_NelvpActiveUB, index, v);
}

void CDriverVulkan::setUniform4f(TProgram program, uint index, float f0, float f1, float f2, float f3)
{
	if (program != VertexProgram) return;
	const float v[4] = { f0, f1, f2, f3 };
	writeNelvpConstant(_NelvpActiveUB, index, v);
}

void CDriverVulkan::setUniform1i(TProgram program, uint index, sint32 i0)
{
	if (program != VertexProgram) return;
	float v[4];
	memset(v, 0, sizeof(v));
	memcpy(&v[0], &i0, sizeof(i0));
	writeNelvpConstant(_NelvpActiveUB, index, v);
}

void CDriverVulkan::setUniform2i(TProgram program, uint index, sint32 i0, sint32 i1)
{
	if (program != VertexProgram) return;
	float v[4];
	memset(v, 0, sizeof(v));
	memcpy(&v[0], &i0, sizeof(i0));
	memcpy(&v[1], &i1, sizeof(i1));
	writeNelvpConstant(_NelvpActiveUB, index, v);
}

void CDriverVulkan::setUniform3i(TProgram program, uint index, sint32 i0, sint32 i1, sint32 i2)
{
	if (program != VertexProgram) return;
	float v[4];
	memset(v, 0, sizeof(v));
	memcpy(&v[0], &i0, sizeof(i0));
	memcpy(&v[1], &i1, sizeof(i1));
	memcpy(&v[2], &i2, sizeof(i2));
	writeNelvpConstant(_NelvpActiveUB, index, v);
}

void CDriverVulkan::setUniform4i(TProgram program, uint index, sint32 i0, sint32 i1, sint32 i2, sint32 i3)
{
	if (program != VertexProgram) return;
	float v[4];
	memcpy(&v[0], &i0, sizeof(i0));
	memcpy(&v[1], &i1, sizeof(i1));
	memcpy(&v[2], &i2, sizeof(i2));
	memcpy(&v[3], &i3, sizeof(i3));
	writeNelvpConstant(_NelvpActiveUB, index, v);
}

void CDriverVulkan::setUniform1ui(TProgram program, uint index, uint32 ui0)
{
	if (program != VertexProgram) return;
	float v[4];
	memset(v, 0, sizeof(v));
	memcpy(&v[0], &ui0, sizeof(ui0));
	writeNelvpConstant(_NelvpActiveUB, index, v);
}

void CDriverVulkan::setUniform2ui(TProgram program, uint index, uint32 ui0, uint32 ui1)
{
	if (program != VertexProgram) return;
	float v[4];
	memset(v, 0, sizeof(v));
	memcpy(&v[0], &ui0, sizeof(ui0));
	memcpy(&v[1], &ui1, sizeof(ui1));
	writeNelvpConstant(_NelvpActiveUB, index, v);
}

void CDriverVulkan::setUniform3ui(TProgram program, uint index, uint32 ui0, uint32 ui1, uint32 ui2)
{
	if (program != VertexProgram) return;
	float v[4];
	memset(v, 0, sizeof(v));
	memcpy(&v[0], &ui0, sizeof(ui0));
	memcpy(&v[1], &ui1, sizeof(ui1));
	memcpy(&v[2], &ui2, sizeof(ui2));
	writeNelvpConstant(_NelvpActiveUB, index, v);
}

void CDriverVulkan::setUniform4ui(TProgram program, uint index, uint32 ui0, uint32 ui1, uint32 ui2, uint32 ui3)
{
	if (program != VertexProgram) return;
	float v[4];
	memcpy(&v[0], &ui0, sizeof(ui0));
	memcpy(&v[1], &ui1, sizeof(ui1));
	memcpy(&v[2], &ui2, sizeof(ui2));
	memcpy(&v[3], &ui3, sizeof(ui3));
	writeNelvpConstant(_NelvpActiveUB, index, v);
}

void CDriverVulkan::setUniform3f(TProgram program, uint index, const NLMISC::CVector &v)
{
	if (program != VertexProgram) return;
	const float values[4] = { v.x, v.y, v.z, 0.f };
	writeNelvpConstant(_NelvpActiveUB, index, values);
}

void CDriverVulkan::setUniform4f(TProgram program, uint index, const NLMISC::CVector &v, float f3)
{
	if (program != VertexProgram) return;
	const float values[4] = { v.x, v.y, v.z, f3 };
	writeNelvpConstant(_NelvpActiveUB, index, values);
}

void CDriverVulkan::setUniform4f(TProgram program, uint index, const NLMISC::CRGBAF &rgba)
{
	if (program != VertexProgram) return;
	const float values[4] = { rgba.R, rgba.G, rgba.B, rgba.A };
	writeNelvpConstant(_NelvpActiveUB, index, values);
}

void CDriverVulkan::setUniform4x4f(TProgram program, uint index, const NLMISC::CMatrix &m)
{
	if (program != VertexProgram || !_NelvpActiveUB || index == ~0u)
		return;
	// nelvp stores matrix rows in consecutive constant registers; CMatrix is
	// column-major, so transpose before writing.
	CMatrix mt = m;
	mt.transpose();
	CUniformBuffer *ub = _NelvpActiveUB;
	const uint32 size = (uint32)ub->Format.size();
	if ((uint64)index * 16 + 64 > size)
	{
		nlwarning("Vulkan: nelvp matrix index %u is outside the %u-byte constant block", index, size);
		return;
	}
	ub->lock();
	ub->set((sint)(index * 16), mt);
	ub->unlock();
}

void CDriverVulkan::setUniform4fv(TProgram program, uint index, size_t num, const float *src)
{
	if (program != VertexProgram) return;
	for (size_t i = 0; i < num; ++i)
		if (!writeNelvpConstant(_NelvpActiveUB, (uint)(index + i), src + i * 4))
			break;
}

void CDriverVulkan::setUniform4iv(TProgram program, uint index, size_t num, const sint32 *src)
{
	if (program != VertexProgram) return;
	for (size_t i = 0; i < num; ++i)
	{
		float v[4];
		memcpy(v, src + i * 4, sizeof(v));
		if (!writeNelvpConstant(_NelvpActiveUB, (uint)(index + i), v))
			break;
	}
}

void CDriverVulkan::setUniform4uiv(TProgram program, uint index, size_t num, const uint32 *src)
{
	if (program != VertexProgram) return;
	for (size_t i = 0; i < num; ++i)
	{
		float v[4];
		memcpy(v, src + i * 4, sizeof(v));
		if (!writeNelvpConstant(_NelvpActiveUB, (uint)(index + i), v))
			break;
	}
}

void CDriverVulkan::setUniformMatrix(TProgram program, uint index, TMatrix matrix, TTransform transform)
{
	if (program != VertexProgram || !_NelvpActiveUB)
		return;

	// nelvp expects old-GL-style matrices: ModelView includes the basis change
	// from NeL space to GL eye space, exactly like the GL3 driver.
	CMatrix mat;
	switch (matrix)
	{
	case IDriver::ModelView:
		mat = _ChangeBasis * _ModelViewMatrix;
		break;
	case IDriver::Projection:
		mat = _GLProjMat;
		break;
	case IDriver::ModelViewProjection:
	default:
		mat = _GLProjMat * _ChangeBasis * _ModelViewMatrix;
		break;
	}

	switch (transform)
	{
	case IDriver::Inverse:
		mat.invert();
		break;
	case IDriver::Transpose:
		mat.transpose();
		break;
	case IDriver::InverseTranspose:
		mat.transpose();
		mat.invert();
		break;
	default:
		break;
	}

	setUniform4x4f(program, index, mat);
}

void CDriverVulkan::setUniformFog(TProgram program, uint index)
{
	if (program != VertexProgram || !_NelvpActiveUB)
		return;
	// The nelvp fog vector needs the basis-changed ModelView (GL eye space).
	const CMatrix nelvpMV = _ChangeBasis * _ModelViewMatrix;
	const float *v = nelvpMV.get();
	const float values[4] = { -v[2], -v[6], -v[10], -v[4] };
	writeNelvpConstant(_NelvpActiveUB, index, values);
}

bool CDriverVulkan::bindUniformBuffer(TUBBinding binding, CUniformBuffer *ub)
{
	if (binding < 0 || binding >= UBBindingCount)
		return false;
	_BoundUserUB[binding] = ub;
	return true;
}

} /* namespace NLVULKAN */

/* end of file */
