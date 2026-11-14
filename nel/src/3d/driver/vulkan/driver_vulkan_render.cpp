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
 * Rendering for the Vulkan driver: vertex/index buffers, pipeline cache,
 * materials, draw calls and the uniform upload.
 *
 * This first implementation targets the state that the 2D UI and the simple
 * world materials use. The shader is intentionally small and explicit; the
 * GL3 mega shaders will replace it once the draw path is proven.
 */

#include "stdvulkan.h"
#include "driver_vulkan.h"

#include "nel/3d/viewport.h"
#include "nel/3d/scissor.h"
#include "nel/3d/light.h"
#include "nel/3d/texture_bump.h"

#include <cmath>
#include <chrono>

#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>

using namespace std;
using namespace NLMISC;

/// NLVK_NOCULL is a diagnostic: read it once instead of scanning the
/// environment for every draw.
static bool noCullEnabled()
{
	static const bool enabled = getenv("NLVK_NOCULL") != NULL;
	return enabled;
}

/// Monotonic nanoseconds for the optional NLVK_STATS breakdown.
static uint64 nowNs()
{
	return (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}
using namespace NL3D;

namespace NLVULKAN {

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

// Texture coordinate generation modes, the same values the mega vertex
// program receives in nlTexGenMode (and the GL3 driver in TexGenModeVP).
enum TSimpleTexGenMode
{
	SimpleTexGenReflectionMap = 0,	// GL_REFLECTION_MAP_ARB (cube texture)
	SimpleTexGenSphereMap = 1,		// GL_SPHERE_MAP (2D texture)
	SimpleTexGenObjectLinear = 2,	// GL_OBJECT_LINEAR
	SimpleTexGenEyeLinear = 3,		// GL_EYE_LINEAR
};

// std140 layout, matching CDriverVulkan::SSimpleUniforms member for member.
// Implicit padding is part of the contract (marked with the C offsets).
static const char *s_UniformBlockGLSL =
	"layout(set = 0, binding = 4) uniform NlSimple {\n"
	"    mat4 mvp;\n"                       // 0
	"    mat3 normalMatrix;\n"              // 64
	"    vec4 color;\n"                     // 112
	"    vec4 diffuse;\n"                   // 128
	"    vec4 ambient;\n"                   // 144
	"    float alphaRef;\n"                 // 160
	"    int flags;\n"                      // 164
	"    uint texEnv0;\n"                   // 168
	"    uint texEnv1;\n"                   // 172
	"    uint texEnv2;\n"                   // 176
	"    uint texEnv3;\n"                   // 180
	"    uint uvRouting;\n"                 // 184
	"    uint rtFlipMask;\n"                // 188 (render targets are stored top-down)
	"    vec4 lightDirOrPos[4];\n"          // 192
	"    vec4 lightDiffuse[4];\n"           // 256
	"    int lightCount;\n"                 // 320
	"    ivec4 texGenMode;\n"               // 336 (12 bytes of std140 padding above)
	"    vec4 fogColor;\n"                  // 352
	"    vec2 fogParams;\n"                 // 368 (start, end)
	"    float fogDensity;\n"               // 376
	"    int fogMode;\n"                    // 380
	"    int fogEnabled;\n"                 // 384
	"    vec4 texGenPlane[4][4];\n"         // 400 (12 bytes of std140 padding above)
	"    mat4 texMatrix[4];\n"              // 656
	"    mat4 specularTexMtx;\n"            // 912
	"    mat4 modelView;\n"                 // 976
	"    vec4 texEnvConstant[4];\n"         // 1040 (TexEnv stage constant colours)
	"} nl;\n";                              // 1104

static std::string buildVertexShader(uint32 flags)
{
	bool hasColor = (flags & CVertexBuffer::PrimaryColorFlag) != 0;
	bool hasNormal = (flags & CVertexBuffer::NormalFlag) != 0;

	std::string src;
	src += "#version 450\n";
	src += "layout(location = 0) in vec3 attrPosition;\n";
	if (hasNormal)
		src += "layout(location = 2) in vec3 attrNormal;\n";
	if (hasColor)
		src += "layout(location = 3) in vec4 attrColor;\n";
	for (uint i = 0; i < 4; ++i)
	{
		if (flags & (CVertexBuffer::TexCoord0Flag << i))
		{
			char line[64];
			snprintf(line, sizeof(line), "layout(location = %u) in vec2 attrTexCoord%u;\n", 8 + i, i);
			src += line;
		}
	}
	src += s_UniformBlockGLSL;
	src += "layout(location = 0) out vec4 vColor;\n";
	src += "layout(location = 1) out vec4 vUv0;\n";
	src += "layout(location = 2) out vec4 vUv1;\n";
	src += "layout(location = 3) out vec4 vUv2;\n";
	src += "layout(location = 4) out vec4 vUv3;\n";
	src += "layout(location = 5) out float vFogDepth;\n";
	src += "vec4 selectUv(uint routing) {\n";
	bool anyUv = false;
	for (uint i = 0; i < 4; ++i)
	{
		if (flags & (CVertexBuffer::TexCoord0Flag << i))
		{
			char line[96];
			snprintf(line, sizeof(line), "    if (routing == %u) return vec4(attrTexCoord%u, 0.0, 1.0);\n", i, i);
			src += line;
			anyUv = true;
		}
	}
	if (!anyUv)
		src += "    // no vertex buffer texture coordinates: texgen or the white fallback\n";
	src += "    return vec4(0.0, 0.0, 0.0, 1.0);\n";
	src += "}\n";
	src += "void main() {\n"
		"    vec4 objPos = vec4(attrPosition, 1.0);\n"
		"    vec4 pos = nl.mvp * objPos;\n"
		"    pos.z = (pos.z + pos.w) * 0.5;\n"
		"    gl_Position = pos;\n"
		"    // Point-list draws (the flare occlusion probe) need a defined size;\n"
		"    // without this Vulkan leaves it undefined (1.0 on RADV, not elsewhere).\n"
		"    gl_PointSize = 1.0;\n"
		"    // Eye-space position: fog depth and the eye-linear texgen modes use it\n"
		"    // exactly like the mega vertex program (NeL eye space is Y = into screen).\n"
		"    vec4 eyePos = nl.modelView * objPos;\n"
		"    vFogDepth = abs(eyePos.y / eyePos.w);\n"
		"    // Lit materials use the diffuse colour (like the mega vertex program),\n"
		"    // unlit materials use the material colour.\n"
		"    vec4 vcol;\n"
		"    if ((nl.flags & 32) != 0) {\n";
	if (hasNormal)
	{
		src +=
			"        vec3 n = normalize(nl.normalMatrix * attrNormal);\n"
			"        vec3 lit = vec3(0.0);\n"
			"        for (int i = 0; i < nl.lightCount && i < 4; ++i) {\n"
			"            if (nl.lightDirOrPos[i].w < 0.5) {\n"
			"                vec3 ldir = normalize(-nl.lightDirOrPos[i].xyz);\n"
			"                lit += nl.lightDiffuse[i].rgb * max(dot(n, ldir), 0.0);\n"
			"            }\n"
			"        }\n"
			"        // Same split as the mega vertex program: the material diffuse\n"
			"        // modulates the lights only, the self-illumination term (ambient\n"
			"        // times material ambient, plus emissive) is added afterwards.\n"
			"        vec4 effectiveDiffuse = ((nl.flags & 64) != 0) ? vec4(1.0) : nl.diffuse;\n"
			"        vcol = vec4(effectiveDiffuse.rgb * lit + nl.ambient.rgb, 1.0);\n";
		if (hasColor)
			src +=
			"        if ((nl.flags & 64) != 0)\n"
			"            vcol *= attrColor;\n";
		src +=
			"        vcol = clamp(vcol, 0.0, 1.0);\n";
	}
	else
	{
		src +=
			"        // No normal: GL3 disables lighting for this draw.\n"
			"        vcol = nl.color;\n";
		if (hasColor)
			src += "        vcol = attrColor;\n";
	}
	src +=
		"    } else {\n"
		"        vcol = nl.color;\n";
	if (hasColor)
		src += "        vcol = attrColor;\n";
	src +=
		"    }\n"
		"    vColor = vcol;\n";
	if (hasNormal)
		src +=
		"    // Eye-space reflection vector, shared by the reflection/sphere texgen modes.\n"
		"    vec3 reflR = vec3(0.0, 0.0, -1.0);\n"
		"    {\n"
		"        vec3 n = normalize(nl.normalMatrix * attrNormal);\n"
		"        vec3 u = normalize(eyePos.xyz);\n"
		"        reflR = reflect(u, n);\n"
		"    }\n";
	else
		src += "    vec3 reflR = vec3(0.0, 0.0, -1.0);\n";
	for (uint i = 0; i < 4; ++i)
	{
		char line[2048];
		snprintf(line, sizeof(line),
			"    {\n"
			"        vec4 uv = selectUv((nl.uvRouting >> %u) & 0xFu);\n"
			"        if (nl.texGenMode[%u] == %d)\n"
			"        {\n"
			"            // Object-linear, the shadow receiver's mode: the planes are the\n"
			"            // GL defaults (object position), then the stage's texture matrix.\n"
			"            vec4 tg = vec4(dot(nl.texGenPlane[%u][0], objPos), dot(nl.texGenPlane[%u][1], objPos),\n"
			"                           dot(nl.texGenPlane[%u][2], objPos), dot(nl.texGenPlane[%u][3], objPos));\n"
			"            uv = nl.texMatrix[%u] * tg;\n"
			"        }\n"
			"        else if (nl.texGenMode[%u] == %d)\n"
			"        {\n"
			"            vec4 tg = vec4(dot(nl.texGenPlane[%u][0], eyePos), dot(nl.texGenPlane[%u][1], eyePos),\n"
			"                           dot(nl.texGenPlane[%u][2], eyePos), dot(nl.texGenPlane[%u][3], eyePos));\n"
			"            uv = nl.texMatrix[%u] * tg;\n"
			"        }\n"
			"        else if (nl.texGenMode[%u] == %d)\n"
			"        {\n"
			"            float m = 2.0 * sqrt(reflR.x * reflR.x + reflR.y * reflR.y + (reflR.z + 1.0) * (reflR.z + 1.0));\n"
			"            uv = nl.texMatrix[%u] * vec4(reflR.x / m + 0.5, reflR.y / m + 0.5, 0.0, 1.0);\n"
			"        }\n"
			"        else if (nl.texGenMode[%u] == %d)\n"
			"        {\n"
			"            uv = nl.specularTexMtx * vec4(reflR, 0.0);\n"
			"        }\n"
			"        else\n"
			"        {\n"
			"            uv = nl.texMatrix[%u] * uv;\n"
			"        }\n"
			"        vUv%u = uv;\n"
			"    }\n",
			i * 4,
			i, (int)SimpleTexGenObjectLinear,
			i, i, i, i, i,
			i, (int)SimpleTexGenEyeLinear,
			i, i, i, i, i,
			i, (int)SimpleTexGenSphereMap, i,
			i, (int)SimpleTexGenReflectionMap,
			i,
			i);
		src += line;
	}
	src += "}\n";
	return src;
}


/// TexEnv interpreter, the same packing as CMaterial::CTexEnv (see the GL3 mega
/// pixel program for the reference implementation).
static const std::string s_FragmentShader =
	std::string("#version 450\n")
	+ "layout(location = 0) in vec4 vColor;\n"
	"layout(location = 1) in vec4 vUv0;\n"
	"layout(location = 2) in vec4 vUv1;\n"
	"layout(location = 3) in vec4 vUv2;\n"
	"layout(location = 4) in vec4 vUv3;\n"
	"layout(location = 5) in float vFogDepth;\n"
	"layout(binding = 0) uniform sampler2D tex0;\n"
	"layout(binding = 1) uniform sampler2D tex1;\n"
	"layout(binding = 2) uniform sampler2D tex2;\n"
	"layout(binding = 3) uniform sampler2D tex3;\n"
	+ std::string(s_UniformBlockGLSL)
	+ "layout(location = 0) out vec4 fragColor;\n"
	"vec4 getTexEnvSrc(int src, vec4 texel, vec4 previous, vec4 diffuse, vec4 cnst) {\n"
	"  return (src == 0) ? texel : (src == 1) ? previous : (src == 2) ? diffuse : cnst;\n"
	"}\n"
	"vec3 getArgRGB(int src, int op, vec4 texel, vec4 previous, vec4 diffuse, vec4 cnst) {\n"
	"  vec4 s = getTexEnvSrc(src, texel, previous, diffuse, cnst);\n"
	"  return (op == 0) ? s.rgb : (op == 1) ? (vec3(1.0) - s.rgb) : (op == 2) ? s.aaa : (vec3(1.0) - s.aaa);\n"
	"}\n"
	"float getArgA(int src, int op, vec4 texel, vec4 previous, vec4 diffuse, vec4 cnst) {\n"
	"  vec4 s = getTexEnvSrc(src, texel, previous, diffuse, cnst);\n"
	"  return (op == 0) ? s.a : (op == 1) ? (1.0 - s.a) : s.a;\n"
	"}\n"
	"vec3 combRGB(int op, vec3 a0, vec3 a1, vec3 a2, vec4 texel, vec4 prev, vec4 diff, vec4 cnst) {\n"
	"  if (op == 0) return a0;\n"
	"  if (op == 1) return a0 * a1;\n"
	"  if (op == 2) return a0 + a1;\n"
	"  if (op == 3) return a0 + a1 - vec3(0.5);\n"
	"  if (op == 4) { float s = texel.a; return a0 * s + a1 * (1.0 - s); }\n"
	"  if (op == 5) { float s = prev.a;  return a0 * s + a1 * (1.0 - s); }\n"
	"  if (op == 6) { float s = diff.a;  return a0 * s + a1 * (1.0 - s); }\n"
	"  if (op == 7) { float s = cnst.a;  return a0 * s + a1 * (1.0 - s); }\n"
	"  if (op == 9) return a0 * a1 + a2;\n"
	"  return a0;\n"
	"}\n"
	"float combA(int op, float a0, float a1, float a2, vec4 texel, vec4 prev, vec4 diff, vec4 cnst) {\n"
	"  if (op == 0) return a0;\n"
	"  if (op == 1) return a0 * a1;\n"
	"  if (op == 2) return a0 + a1;\n"
	"  if (op == 3) return a0 + a1 - 0.5;\n"
	"  if (op == 4) { float s = texel.a; return a0 * s + a1 * (1.0 - s); }\n"
	"  if (op == 5) { float s = prev.a;  return a0 * s + a1 * (1.0 - s); }\n"
	"  if (op == 6) { float s = diff.a;  return a0 * s + a1 * (1.0 - s); }\n"
	"  if (op == 7) { float s = cnst.a;  return a0 * s + a1 * (1.0 - s); }\n"
	"  if (op == 9) return a0 * a1 + a2;\n"
	"  return a0;\n"
	"}\n"
	"vec4 applyTexEnv(uint env, vec4 texel, vec4 prev, vec4 diff, vec4 cnst) {\n"
	"  int opRGB   = int(env & 0xFu);\n"
	"  int src0RGB = int((env >>  4) & 0x3u);\n"
	"  int op0RGB  = int((env >>  6) & 0x3u);\n"
	"  int src1RGB = int((env >>  8) & 0x3u);\n"
	"  int op1RGB  = int((env >> 10) & 0x3u);\n"
	"  int src2RGB = int((env >> 12) & 0x3u);\n"
	"  int op2RGB  = int((env >> 14) & 0x3u);\n"
	"  int opA     = int((env >> 16) & 0xFu);\n"
	"  int src0A   = int((env >> 20) & 0x3u);\n"
	"  int op0A    = int((env >> 22) & 0x3u);\n"
	"  int src1A   = int((env >> 24) & 0x3u);\n"
	"  int op1A    = int((env >> 26) & 0x3u);\n"
	"  int src2A   = int((env >> 28) & 0x3u);\n"
	"  int op2A    = int((env >> 30) & 0x3u);\n"
	"  vec3 a0r = getArgRGB(src0RGB, op0RGB, texel, prev, diff, cnst);\n"
	"  vec3 a1r = getArgRGB(src1RGB, op1RGB, texel, prev, diff, cnst);\n"
	"  vec3 a2r = getArgRGB(src2RGB, op2RGB, texel, prev, diff, cnst);\n"
	"  float a0a = getArgA(src0A, op0A, texel, prev, diff, cnst);\n"
	"  float a1a = getArgA(src1A, op1A, texel, prev, diff, cnst);\n"
	"  float a2a = getArgA(src2A, op2A, texel, prev, diff, cnst);\n"
	"  return vec4(combRGB(opRGB, a0r, a1r, a2r, texel, prev, diff, cnst),\n"
	"              combA(opA, a0a, a1a, a2a, texel, prev, diff, cnst));\n"
	"}\n"
	"// Texgen and the user texture matrices are projective: divide by q.\n"
	"vec2 uvOf(vec4 uv) { return uv.xy / (uv.w != 0.0 ? uv.w : 1.0); }\n"
	"// Render targets are stored top-down (Vulkan row 0 is the top), while NeL\n"
	"// texture coordinates have V = 0 at the bottom: flip V for those stages.\n"
	"vec2 uvOfStage(vec4 uv, uint flip) { vec2 st = uvOf(uv); return (flip != 0u) ? vec2(st.x, 1.0 - st.y) : st; }\n"
	"// Same fog equation and eye-space depth as the mega pixel program.\n"
	"vec4 applyFog(vec4 col) {\n"
	"    float z = vFogDepth;\n"
	"    float fogFactor;\n"
	"    if (nl.fogMode == 1) fogFactor = clamp(exp(-nl.fogDensity * z), 0.0, 1.0);\n"
	"    else if (nl.fogMode == 2) fogFactor = clamp(exp(-nl.fogDensity * nl.fogDensity * z * z), 0.0, 1.0);\n"
	"    else fogFactor = clamp((nl.fogParams.y - z) / (nl.fogParams.y - nl.fogParams.x), 0.0, 1.0);\n"
	"    vec4 fColor = mix(nl.fogColor, col, fogFactor);\n"
	"    fColor.a = col.a;\n"
	"    return fColor;\n"
	"}\n"
	"void main() {\n"
	"    vec4 frag = vColor;\n"
	"    if ((nl.flags & 1) != 0) frag = applyTexEnv(nl.texEnv0, texture(tex0, uvOfStage(vUv0, nl.rtFlipMask & 1u)), frag, vColor, nl.texEnvConstant[0]);\n"
	"    if ((nl.flags & 2) != 0) frag = applyTexEnv(nl.texEnv1, texture(tex1, uvOfStage(vUv1, nl.rtFlipMask & 2u)), frag, vColor, nl.texEnvConstant[1]);\n"
	"    if ((nl.flags & 4) != 0) frag = applyTexEnv(nl.texEnv2, texture(tex2, uvOfStage(vUv2, nl.rtFlipMask & 4u)), frag, vColor, nl.texEnvConstant[2]);\n"
	"    if ((nl.flags & 8) != 0) frag = applyTexEnv(nl.texEnv3, texture(tex3, uvOfStage(vUv3, nl.rtFlipMask & 8u)), frag, vColor, nl.texEnvConstant[3]);\n"
	"    if ((nl.flags & 16) != 0 && frag.a < nl.alphaRef) discard;\n"
	"    if (nl.fogEnabled != 0) frag = applyFog(frag);\n"
	"    fragColor = frag;\n"
	"}\n";

struct SPushConstants
{
	float	MVP[16];
	float	Color[4];
	float	AlphaRef;
	sint32	Flags;
	uint32	TexEnv[4];
	uint32	UvRouting;
};

// ---------------------------------------------------------------------------
// Shader compilation
// ---------------------------------------------------------------------------

bool CDriverVulkan::compileShader(const char *glsl, VkShaderModule *outModule, bool vertex)
{
	static bool initialized = false;
	if (!initialized)
	{
		glslang::InitializeProcess();
		initialized = true;
	}

	EShLanguage stage = vertex ? EShLangVertex : EShLangFragment;
	glslang::TShader shader(stage);
	shader.setStrings(&glsl, 1);
	shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
	shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);

	if (!shader.parse(GetDefaultResources(), 450, false, EShMsgDefault))
	{
		nlwarning("Vulkan: shader compile failed: %s", shader.getInfoLog());
		// Temporary developer aid: keep the rejected source for inspection.
		if (getenv("NLVK_VP_DUMP"))
		{
			static int dumpCount = 0;
			char path[256];
			snprintf(path, sizeof(path), "%s/fail%d.glsl", getenv("NLVK_VP_DUMP"), dumpCount++);
			FILE *f = fopen(path, "wb");
			if (f) { fwrite(glsl, 1, strlen(glsl), f); fclose(f); }
		}
		return false;
	}

	glslang::TProgram program;
	program.addShader(&shader);
	if (!program.link(EShMsgDefault))
	{
		nlwarning("Vulkan: shader link failed: %s", program.getInfoLog());
		return false;
	}

	vector<uint32> spirv;
	glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);

	VkShaderModuleCreateInfo smci;
	memset(&smci, 0, sizeof(smci));
	smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	smci.codeSize = spirv.size() * sizeof(uint32);
	smci.pCode = &spirv[0];
	if (vkCreateShaderModule(_Device, &smci, NULL, outModule) != VK_SUCCESS)
	{
		nlwarning("Vulkan: vkCreateShaderModule failed");
		return false;
	}
	return true;
}

VkShaderModule CDriverVulkan::createShaderModule(const std::vector<uint32> &code)
{
	VkShaderModule module = VK_NULL_HANDLE;
	VkShaderModuleCreateInfo smci;
	memset(&smci, 0, sizeof(smci));
	smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	smci.codeSize = code.size() * sizeof(uint32);
	smci.pCode = &code[0];
	vkCreateShaderModule(_Device, &smci, NULL, &module);
	return module;
}

// ---------------------------------------------------------------------------
// Vertex layout
// ---------------------------------------------------------------------------

static VkFormat vertexFormatForType(CVertexBuffer::TType type, uint &numComponents)
{
	switch (type)
	{
	case CVertexBuffer::Float1: numComponents = 1; return VK_FORMAT_R32_SFLOAT;
	case CVertexBuffer::Float2: numComponents = 2; return VK_FORMAT_R32G32_SFLOAT;
	case CVertexBuffer::Float3: numComponents = 3; return VK_FORMAT_R32G32B32_SFLOAT;
	case CVertexBuffer::Float4: numComponents = 4; return VK_FORMAT_R32G32B32A32_SFLOAT;
	case CVertexBuffer::UChar4: numComponents = 4; return VK_FORMAT_R8G8B8A8_UNORM;
	case CVertexBuffer::Short1: numComponents = 1; return VK_FORMAT_R16_SNORM;
	case CVertexBuffer::Short2: numComponents = 2; return VK_FORMAT_R16G16_SNORM;
	case CVertexBuffer::Short3: numComponents = 3; return VK_FORMAT_R16G16B16_SNORM;
	case CVertexBuffer::Short4: numComponents = 4; return VK_FORMAT_R16G16B16A16_SNORM;
	default:
		numComponents = 4;
		return VK_FORMAT_R32G32B32A32_SFLOAT;
	}
}

/// Attribute locations used by the engine-side vertex programs (same as GL3).
static const uint s_AttributeLocation[CVertexBuffer::NumValue] =
{
	0,	// Position
	2,	// Normal
	8,	// TexCoord0
	9,	// TexCoord1
	10,	// TexCoord2
	11,	// TexCoord3
	12,	// TexCoord4
	13,	// TexCoord5
	14,	// TexCoord6
	15,	// TexCoord7
	3,	// PrimaryColor
	4,	// SecondaryColor
	1,	// Weight
	6,	// PaletteSkin
	5,	// Fog
	7,	// Tangent
};

static uint32 makeVertexLayoutHash(CVertexBuffer &vb)
{
	uint32 hash = vb.getVertexFormat();
	for (uint i = 0; i < CVertexBuffer::NumValue; ++i)
		hash = hash * 33 + (uint32)vb.getValueType(i);
	return hash;
}

static void buildVertexLayout(CVertexBuffer &vb, vector<VkVertexInputAttributeDescription> &attributes)
{
	attributes.clear();
	uint16 format = vb.getVertexFormat();
	for (uint i = 0; i < CVertexBuffer::NumValue; ++i)
	{
		if (!(format & (1 << i)))
			continue;
		VkVertexInputAttributeDescription attribute;
		memset(&attribute, 0, sizeof(attribute));
		attribute.location = s_AttributeLocation[i];
		attribute.binding = 0;
		attribute.offset = vb.getValueOffEx((CVertexBuffer::TValue)i);
		uint components = 0;
		attribute.format = vertexFormatForType(vb.getValueType(i), components);
		// GPU skinning stores raw bone indices in UChar4. GL3 binds that
		// attribute as an integer attribute (GL_FALSE normalize), so the shader
		// reads 0..255 in a vec4; the Vulkan equivalent is a scaled format,
		// which converts the raw bytes to float without normalizing them.
		if (i == CVertexBuffer::PaletteSkin && vb.getValueType(i) == CVertexBuffer::UChar4)
			attribute.format = VK_FORMAT_R8G8B8A8_USCALED;
		// Drop components that the format does not provide.
		(void)components;
		attributes.push_back(attribute);
	}
}

// ---------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------

static VkBlendFactor toVkBlend(CMaterial::TBlend blend)
{
	switch (blend)
	{
	case CMaterial::one: return VK_BLEND_FACTOR_ONE;
	case CMaterial::zero: return VK_BLEND_FACTOR_ZERO;
	case CMaterial::srcalpha: return VK_BLEND_FACTOR_SRC_ALPHA;
	case CMaterial::invsrcalpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case CMaterial::srccolor: return VK_BLEND_FACTOR_SRC_COLOR;
	case CMaterial::invsrccolor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case CMaterial::blendConstantColor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
	case CMaterial::blendConstantInvColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
	case CMaterial::blendConstantAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
	case CMaterial::blendConstantInvAlpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
	default: return VK_BLEND_FACTOR_ONE;
	}
}

static VkCompareOp toVkCompare(CMaterial::ZFunc func)
{
	switch (func)
	{
	case CMaterial::always: return VK_COMPARE_OP_ALWAYS;
	case CMaterial::never: return VK_COMPARE_OP_NEVER;
	case CMaterial::less: return VK_COMPARE_OP_LESS;
	case CMaterial::lessequal: return VK_COMPARE_OP_LESS_OR_EQUAL;
	case CMaterial::equal: return VK_COMPARE_OP_EQUAL;
	case CMaterial::notequal: return VK_COMPARE_OP_NOT_EQUAL;
	case CMaterial::greater: return VK_COMPARE_OP_GREATER;
	case CMaterial::greaterequal: return VK_COMPARE_OP_GREATER_OR_EQUAL;
	default: return VK_COMPARE_OP_LESS_OR_EQUAL;
	}
}

VkPipeline CDriverVulkan::getPipeline(const CPipelineKey &key)
{
	map<CPipelineKey, CPipelineVulkan>::iterator it = _PipelineCache.find(key);
	if (it != _PipelineCache.end())
		return it->second.Pipeline;

	// The simple vertex shader source depends on exactly these vertex flags:
	// PrimaryColorFlag picks the colour source of the unlit path, NormalFlag
	// picks the lit branch (and the reflection vector), and TexCoord0..3 pick
	// the UVs selectUv() can return. The cache key must cover all of them: with
	// only PrimaryColor|TexCoord0, a variant compiled for a vertex format
	// without a normal was reused for a lit draw that has one, so that draw
	// silently took the unlit path and painted its material colour. That is
	// what turned the untextured, lit sky/fog dome white in Prime Roots (the
	// dome is a lit material with a black diffuse, so the lit branch is black).
	const uint32 simpleVertexFlags =
		CVertexBuffer::PrimaryColorFlag | CVertexBuffer::NormalFlag |
		CVertexBuffer::TexCoord0Flag | CVertexBuffer::TexCoord1Flag |
		CVertexBuffer::TexCoord2Flag | CVertexBuffer::TexCoord3Flag;
	uint32 shaderFlags = key.VertexFlags & simpleVertexFlags;
	VkShaderModule vertexShader;
	VkShaderModule fragmentShader;
	VkPipelineLayout pipelineLayout = _PipelineLayout;
	if (key.Mega)
	{
		if (key.UserVP != 0)
		{
			if (key.UserVPInsert)
			{
				std::map<uint64, VkShaderModule>::iterator insertIt = _InsertVPShaders.find(
					insertVertexShaderKey(key.UserVP, 1, 0));
				vertexShader = insertIt != _InsertVPShaders.end() ? insertIt->second : VK_NULL_HANDLE;
				if (vertexShader == VK_NULL_HANDLE)
				{
					nlwarning("Vulkan: insert vertex program %u has no mega VP variant", key.UserVP);
					return VK_NULL_HANDLE;
				}
			}
			else
			{
				std::map<uint32, VkShaderModule>::iterator vpIt = _UserVPShaders.find(key.UserVP);
				vertexShader = vpIt != _UserVPShaders.end() ? vpIt->second : VK_NULL_HANDLE;
				if (vertexShader == VK_NULL_HANDLE)
				{
					nlwarning("Vulkan: user vertex program %u is no longer compiled", key.UserVP);
					return VK_NULL_HANDLE;
				}
			}
		}
		else
		{
			vertexShader = getMegaVertexShader();
		}
		if (key.WaterPP != 0)
		{
			// Water replaces the mega pixel stage as well; it reads the water
			// vertex program's texture coordinates and the mega descriptor set.
			fragmentShader = getWaterPixelShader(key.WaterCalcRefl != 0, key.WaterDiffuse != 0);
			if (fragmentShader == VK_NULL_HANDLE)
			{
				nlwarning("Vulkan: no water pixel program for this variant");
				return VK_NULL_HANDLE;
			}
		}
		else if (key.UserPP != 0)
		{
			// Active user pixel program (FXAA): it replaces the mega pixel
			// stage and reads the mega descriptor set for its UBO and samplers.
			std::map<uint32, VkShaderModule>::iterator ppIt = _UserPPShaders.find(key.UserPP);
			fragmentShader = ppIt != _UserPPShaders.end() ? ppIt->second : VK_NULL_HANDLE;
			if (fragmentShader == VK_NULL_HANDLE)
			{
				nlwarning("Vulkan: user pixel program %u is no longer compiled", key.UserPP);
				return VK_NULL_HANDLE;
			}
		}
		else
		{
			fragmentShader = getMegaPixelShader(key.MegaCube != 0, key.MegaSpecular != 0);
		}
		pipelineLayout = _MegaPipelineLayout;
	}
	else
	{
		vertexShader = _VertexShaderVariants[shaderFlags];
		if (vertexShader == VK_NULL_HANDLE)
		{
			std::string source = buildVertexShader(key.VertexFlags);
			if (!compileShader(source.c_str(), &vertexShader, true))
				return VK_NULL_HANDLE;
			_VertexShaderVariants[shaderFlags] = vertexShader;
		}
		fragmentShader = _FragmentShader;
		if (fragmentShader == VK_NULL_HANDLE)
		{
			if (!compileShader(s_FragmentShader.c_str(), &fragmentShader, false))
				return VK_NULL_HANDLE;
			_FragmentShader = fragmentShader;
		}
	}
	if (vertexShader == VK_NULL_HANDLE || fragmentShader == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	CPipelineKey k = key;

	VkPipelineShaderStageCreateInfo stages[2];
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertexShader;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragmentShader;
	stages[1].pName = "main";

	vector<VkVertexInputAttributeDescription> attributes;
	map<uint32, vector<VkVertexInputAttributeDescription> >::iterator layoutIt = _VertexLayouts.find(k.VertexFormat);
	if (layoutIt == _VertexLayouts.end())
	{
		// The layout is recreated from the current vertex buffer on first use.
		if (!_CurrentVB)
		{
			nlwarning("Vulkan: pipeline requested without a vertex buffer");
			return VK_NULL_HANDLE;
		}
		buildVertexLayout(*_CurrentVB, attributes);
		_VertexLayouts[k.VertexFormat] = attributes;
	}
	else
	{
		attributes = layoutIt->second;
	}

	if (key.Mega)
	{
		// The mega vertex program declares all 16 standard attribute locations.
		// Without the vertexAttributeRobustness/maintenance9 features, every
		// declared location must have an attribute description. The unused ones
		// get a single float read at offset 0; the shader never consumes them
		// (nlVertexFormat guards the code paths).
		bool present[16];
		memset(present, 0, sizeof(present));
		for (size_t i = 0; i < attributes.size(); ++i)
			if (attributes[i].location < 16)
				present[attributes[i].location] = true;
		for (uint location = 0; location < 16; ++location)
		{
			if (present[location])
				continue;
			VkVertexInputAttributeDescription attribute;
			memset(&attribute, 0, sizeof(attribute));
			attribute.location = location;
			attribute.binding = 0;
			attribute.format = VK_FORMAT_R32_SFLOAT;
			attribute.offset = 0;
			attributes.push_back(attribute);
		}
	}

	VkVertexInputBindingDescription binding;
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0;
	binding.stride = k.VertexStride;
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkPipelineVertexInputStateCreateInfo vertexInput;
	memset(&vertexInput, 0, sizeof(vertexInput));
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInput.vertexBindingDescriptionCount = 1;
	vertexInput.pVertexBindingDescriptions = &binding;
	vertexInput.vertexAttributeDescriptionCount = (uint32)attributes.size();
	vertexInput.pVertexAttributeDescriptions = attributes.empty() ? NULL : &attributes[0];

	VkPipelineInputAssemblyStateCreateInfo inputAssembly;
	memset(&inputAssembly, 0, sizeof(inputAssembly));
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = (VkPrimitiveTopology)k.Topology;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	VkPipelineViewportStateCreateInfo viewportState;
	memset(&viewportState, 0, sizeof(viewportState));
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo raster;
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.depthClampEnable = VK_FALSE;
	raster.rasterizerDiscardEnable = VK_FALSE;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.lineWidth = 1.f;
	raster.cullMode = (VkCullModeFlags)k.CullMode;
	raster.frontFace = (VkFrontFace)k.FrontFace;
	raster.depthBiasEnable = VK_FALSE;

	VkPipelineMultisampleStateCreateInfo multisample;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil;
	memset(&depthStencil, 0, sizeof(depthStencil));
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = k.DepthTest ? VK_TRUE : VK_FALSE;
	depthStencil.depthWriteEnable = k.DepthWrite ? VK_TRUE : VK_FALSE;
	depthStencil.depthCompareOp = (VkCompareOp)k.DepthCompare;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;

	VkPipelineColorBlendAttachmentState blendAttachment;
	memset(&blendAttachment, 0, sizeof(blendAttachment));
	blendAttachment.blendEnable = k.BlendEnable ? VK_TRUE : VK_FALSE;
	blendAttachment.srcColorBlendFactor = (VkBlendFactor)k.SrcBlend;
	blendAttachment.dstColorBlendFactor = (VkBlendFactor)k.DstBlend;
	blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	blendAttachment.srcAlphaBlendFactor = (VkBlendFactor)k.SrcBlend;
	blendAttachment.dstAlphaBlendFactor = (VkBlendFactor)k.DstBlend;
	blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	blendAttachment.colorWriteMask = k.ColorWriteMask ? k.ColorWriteMask : 0xF;

	VkPipelineColorBlendStateCreateInfo colorBlend;
	memset(&colorBlend, 0, sizeof(colorBlend));
	colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlend.attachmentCount = 1;
	colorBlend.pAttachments = &blendAttachment;

	VkDynamicState dynamicStates[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
	VkPipelineDynamicStateCreateInfo dynamicState;
	memset(&dynamicState, 0, sizeof(dynamicState));
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 3;
	dynamicState.pDynamicStates = dynamicStates;

	VkGraphicsPipelineCreateInfo pci;
	memset(&pci, 0, sizeof(pci));
	pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pci.stageCount = 2;
	pci.pStages = stages;
	pci.pVertexInputState = &vertexInput;
	pci.pInputAssemblyState = &inputAssembly;
	pci.pViewportState = &viewportState;
	pci.pRasterizationState = &raster;
	pci.pMultisampleState = &multisample;
	pci.pDepthStencilState = &depthStencil;
	pci.pColorBlendState = &colorBlend;
	pci.pDynamicState = &dynamicState;
	pci.layout = pipelineLayout;
	pci.renderPass = key.Target ? _OffscreenRenderPass : _RenderPass;
	pci.subpass = 0;

	VkPipeline pipeline = VK_NULL_HANDLE;
	const uint64 pipelineStart = _StatsEnabled ? nowNs() : 0;
	VkResult res = vkCreateGraphicsPipelines(_Device, _VkPipelineCache, 1, &pci, NULL, &pipeline);
	if (_StatsEnabled)
	{
		const uint64 took = nowNs() - pipelineStart;
		_StatsPipelineCreateNs += took;
		++_StatsPipelineCreates;
		if (took > 20 * 1000 * 1000)
			nlinfo("Vulkan: pipeline compile took %.1f ms (mega %d, target %d, vertex flags 0x%x)",
				(double)took / 1e6, (int)key.Mega, (int)key.Target, (uint)key.VertexFlags);
	}
	if (res != VK_SUCCESS)
	{
		nlwarning("Vulkan: vkCreateGraphicsPipelines failed (%d)", (int)res);
		if (key.Mega && key.UserVP == 0)
			_MegaEnabled = false;
		return VK_NULL_HANDLE;
	}

	CPipelineVulkan entry;
	entry.Pipeline = pipeline;
	_PipelineCache[k] = entry;
	return pipeline;
}

/// Debug-only fullscreen triangle, used to validate the frame plumbing
/// (render pass, viewport, pipeline and push constants) without the engine.
static const char *s_TestVertexShader =
	"#version 450\n"
	"layout(push_constant) uniform Push {\n"
	"    mat4 mvp;\n"
	"    vec4 color;\n"
	"    vec4 uvTransform;\n"
	"    float alphaRef;\n"
	"    int flags;\n"
	"} pc;\n"
	"layout(location = 0) out vec4 vColor;\n"
	"void main() {\n"
	"    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
	"    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);\n"
	"    vColor = pc.color;\n"
	"}\n";

void CDriverVulkan::drawTestTriangle(VkCommandBuffer cmd)
{
	if (_TestPipeline == VK_NULL_HANDLE)
	{
		VkShaderModule vs = VK_NULL_HANDLE;
		if (!compileShader(s_TestVertexShader, &vs, true))
			return;
		VkPipelineShaderStageCreateInfo stages[2];
		memset(stages, 0, sizeof(stages));
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vs;
		stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = _FragmentShader;
		stages[1].pName = "main";

		VkPipelineVertexInputStateCreateInfo vertexInput;
		memset(&vertexInput, 0, sizeof(vertexInput));
		vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

		VkPipelineInputAssemblyStateCreateInfo inputAssembly;
		memset(&inputAssembly, 0, sizeof(inputAssembly));
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineViewportStateCreateInfo viewportState;
		memset(&viewportState, 0, sizeof(viewportState));
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo raster;
		memset(&raster, 0, sizeof(raster));
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.lineWidth = 1.f;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_CLOCKWISE;

		VkPipelineMultisampleStateCreateInfo multisample;
		memset(&multisample, 0, sizeof(multisample));
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineDepthStencilStateCreateInfo depthStencil;
		memset(&depthStencil, 0, sizeof(depthStencil));
		depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

		VkPipelineColorBlendAttachmentState blendAttachment;
		memset(&blendAttachment, 0, sizeof(blendAttachment));
		blendAttachment.blendEnable = VK_FALSE;
		blendAttachment.colorWriteMask = 0xF;

		VkPipelineColorBlendStateCreateInfo colorBlend;
		memset(&colorBlend, 0, sizeof(colorBlend));
		colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlend.attachmentCount = 1;
		colorBlend.pAttachments = &blendAttachment;

		VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState;
		memset(&dynamicState, 0, sizeof(dynamicState));
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount = 2;
		dynamicState.pDynamicStates = dynamicStates;

		VkGraphicsPipelineCreateInfo pci;
		memset(&pci, 0, sizeof(pci));
		pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pci.stageCount = 2;
		pci.pStages = stages;
		pci.pVertexInputState = &vertexInput;
		pci.pInputAssemblyState = &inputAssembly;
		pci.pViewportState = &viewportState;
		pci.pRasterizationState = &raster;
		pci.pMultisampleState = &multisample;
		pci.pDepthStencilState = &depthStencil;
		pci.pColorBlendState = &colorBlend;
		pci.pDynamicState = &dynamicState;
		pci.layout = _PipelineLayout;
		pci.renderPass = _RenderPass;
		const uint64 testStart = _StatsEnabled ? nowNs() : 0;
		const VkResult testRes = vkCreateGraphicsPipelines(_Device, _VkPipelineCache, 1, &pci, NULL, &_TestPipeline);
		if (_StatsEnabled)
		{
			_StatsPipelineCreateNs += nowNs() - testStart;
			++_StatsPipelineCreates;
		}
		if (testRes != VK_SUCCESS)
		{
			nlwarning("Vulkan: test pipeline creation failed");
			return;
		}
	}

	SPushConstants push;
	memset(&push, 0, sizeof(push));
	push.Color[0] = 1.f;
	push.Color[1] = 0.f;
	push.Color[2] = 1.f;
	push.Color[3] = 1.f;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _TestPipeline);
	vkCmdPushConstants(cmd, _PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	vkCmdDraw(cmd, 3, 1, 0, 0);
}

// ---------------------------------------------------------------------------
// Vertex buffers
// ---------------------------------------------------------------------------

CVBDrvInfosVulkan::CVBDrvInfosVulkan(CDriverVulkan *drv, ItVBDrvInfoPtrList it, CVertexBuffer *vb)
	: IVBDrvInfos(drv, it, vb)
	, _Dynamic(false)
	, _SlotCount(1)
	, _Slot(0)
	, _LockFrame(~0ull)
	, _Drawn(false)
	, _DrawBuffer(VK_NULL_HANDLE)
	, _DrawOffset(0)
	, _Size(0)
{
	for (uint i = 0; i < MaxSlots; ++i)
	{
		_VertexBuffer[i] = VK_NULL_HANDLE;
		_Memory[i] = VK_NULL_HANDLE;
		_Mapped[i] = NULL;
		_SlotFrame[i] = ~0ull;
	}

	// Buffers the engine rewrites while a previous draw or frame can still be
	// reading them need more than one copy. Streams are rewritten every frame
	// (their contents are undefined after swapBuffers), so they live in the
	// per-frame vertex ring; FullRewrite buffers keep their contents between
	// rewrites and use one persisted buffer per frame in flight.
	switch (vb ? vb->getBufferUsage() : CVertexBuffer::CpuReadWrite)
	{
	case CVertexBuffer::FullStream:
	case CVertexBuffer::SmallStream:
		_Dynamic = true;
		_SlotCount = 0;
		break;
	case CVertexBuffer::FullRewrite:
		_Dynamic = false;
		_SlotCount = MaxSlots;
		break;
	default:
		_Dynamic = false;
		_SlotCount = 1;
		break;
	}
}

CVBDrvInfosVulkan::~CVBDrvInfosVulkan()
{
	if (VertexBufferPtr)
		VertexBufferPtr->setLocation(CVertexBuffer::NotResident);
	CDriverVulkan *driver = static_cast<CDriverVulkan *>(_Driver);
	if (driver && driver->getDevice())
	{
		// The buffers may still be referenced by a recorded or in-flight frame.
		for (uint i = 0; i < _SlotCount; ++i)
			if (_VertexBuffer[i])
				driver->deferDestroyBuffer(_VertexBuffer[i], _Memory[i]);
	}
}

void CVBDrvInfosVulkan::ensureCapacity(uint32 size)
{
	if (size <= _Size)
		return;
	_Size = size;

	// Ring-backed buffers only need the size: their storage is carved out of
	// the driver's vertex ring on every lock.
	if (_Dynamic)
		return;

	CDriverVulkan *driver = static_cast<CDriverVulkan *>(_Driver);
	VkDevice device = driver->getDevice();
	for (uint slot = 0; slot < _SlotCount; ++slot)
	{
		if (_VertexBuffer[slot])
		{
			// The buffer may still be referenced by a recorded or in-flight
			// frame: destroy it at the next frame boundary, never immediately.
			driver->deferDestroyBuffer(_VertexBuffer[slot], _Memory[slot]);
			_VertexBuffer[slot] = VK_NULL_HANDLE;
			_Memory[slot] = VK_NULL_HANDLE;
			_Mapped[slot] = NULL;
		}
	}

	for (uint slot = 0; slot < _SlotCount; ++slot)
	{
		VkBufferCreateInfo bci;
		memset(&bci, 0, sizeof(bci));
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = size;
		bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(device, &bci, NULL, &_VertexBuffer[slot]) != VK_SUCCESS)
		{
			nlwarning("Vulkan: vertex buffer creation failed (%u bytes)", size);
			_VertexBuffer[slot] = VK_NULL_HANDLE;
			continue;
		}
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(device, _VertexBuffer[slot], &req);
		VkMemoryAllocateInfo mai;
		memset(&mai, 0, sizeof(mai));
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = driver->findMemoryType(req.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (vkAllocateMemory(device, &mai, NULL, &_Memory[slot]) != VK_SUCCESS)
		{
			nlwarning("Vulkan: vertex buffer memory allocation failed (%u bytes)", size);
			vkDestroyBuffer(device, _VertexBuffer[slot], NULL);
			_VertexBuffer[slot] = VK_NULL_HANDLE;
			continue;
		}
		vkBindBufferMemory(device, _VertexBuffer[slot], _Memory[slot], 0);
		vkMapMemory(device, _Memory[slot], 0, VK_WHOLE_SIZE, 0, &_Mapped[slot]);
	}
	_Slot = 0;
	for (uint i = 0; i < MaxSlots; ++i)
		_SlotFrame[i] = ~0ull;
	_LockFrame = ~0ull;
	_Drawn = false;
	_DrawBuffer = _VertexBuffer[_Slot];
	_DrawOffset = 0;
}

uint8 *CVBDrvInfosVulkan::lock(uint begin, uint end, bool readOnly)
{
	CDriverVulkan *driver = static_cast<CDriverVulkan *>(_Driver);
	if (_Dynamic)
	{
		if (readOnly)
		{
			if (!driver->vertexRingMapped())
				return NULL;
			return (uint8 *)driver->vertexRingMapped() + _DrawOffset + begin;
		}
		const uint32 offset = driver->allocVertexRing(_Size);
		if (offset == 0xFFFFFFFFu)
			return NULL;
		_DrawBuffer = driver->vertexRingBuffer();
		_DrawOffset = offset;
		return (uint8 *)driver->vertexRingMapped() + offset + begin;
	}

	// FullRewrite buffers are rewritten now and then. Alternate between the
	// per-frame copies so the copy a running frame reads is not the one the
	// CPU is about to overwrite, and keep drawing from the copy last written.
	const uint64 now = driver ? driver->getSwapBufferCounter() : 0;
	if (!readOnly && _SlotCount > 1 && (now != _LockFrame || _Drawn))
	{
		// Move to the other copy when this one was drawn with since its last
		// write, or when a frame that can still be reading it is in flight.
		if (_Drawn || (_SlotFrame[_Slot] != ~0ull && _SlotFrame[_Slot] + 2 > now))
			_Slot = (_Slot + 1) % _SlotCount;
		_LockFrame = now;
		_SlotFrame[_Slot] = now;
	}
	_Drawn = false;
	_DrawBuffer = _VertexBuffer[_Slot];
	_DrawOffset = 0;
	return (uint8 *)_Mapped[_Slot] + begin;
}

void CVBDrvInfosVulkan::unlock(uint begin, uint end)
{
	// Host coherent memory: nothing to do.
}

bool CDriverVulkan::setupVertexBuffer(CVertexBuffer &vb)
{
	const bool touched = (vb.getTouchFlags() & (CVertexBuffer::TouchedReserve | CVertexBuffer::TouchedVertexFormat)) != 0;
	if (touched || vb.DrvInfos == NULL)
	{
		if (vb.DrvInfos)
		{
			delete vb.DrvInfos;
			vb.DrvInfos = NULL;
		}
		if (vb.getNumVertices() == 0)
			return true;

		ItVBDrvInfoPtrList it = _VBDrvInfos.insert(_VBDrvInfos.end(), (NL3D::IVBDrvInfos *)NULL);
		CVBDrvInfosVulkan *info = new CVBDrvInfosVulkan(this, it, &vb);
		*it = vb.DrvInfos = info;

		const uint size = vb.capacity() * vb.getVertexSize();
		info->ensureCapacity(size);
		vb.setLocation(CVertexBuffer::AGPResident);
	}
	return true;
}

bool CDriverVulkan::activeVertexBuffer(CVertexBuffer &vb)
{
	if (!setupVertexBuffer(vb))
		return false;
	if (vb.getNumVertices() == 0)
		return true;
	vb.fillBuffer();
	_CurrentVB = &vb;
	return true;
}

bool CDriverVulkan::activeIndexBuffer(CIndexBuffer &ib)
{
	CIndexBufferReadWrite access;
	ib.lock(access);
	_CurrentIB = &ib;
	_IndexValues = (const uint8 *)access.getPtr();
	_IndexFormat = (access.getFormat() == CIndexBuffer::Indices16) ? 16 : 32;
	return true;
}

// ---------------------------------------------------------------------------
// Per-frame vertex upload ring
// ---------------------------------------------------------------------------

uint32 CDriverVulkan::allocVertexRing(uint32 size)
{
	const uint32 required = _VertexRingOffset + size;
	const bool grow = (_VertexRing == VK_NULL_HANDLE ||
		required > _VertexRingFrameBase + _VertexRingHalf);
	if (grow)
	{
		// Each frame has its own half: a previous frame may still be reading
		// the vertices it uploaded at the same offsets.
		uint32 newSize = _VertexRingSize ? _VertexRingSize : 4 * 1024 * 1024u;
		while (newSize < size * 2 || newSize <= _VertexRingSize)
			newSize *= 2;
		VkBufferCreateInfo bci;
		memset(&bci, 0, sizeof(bci));
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = newSize;
		bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkBuffer buffer = VK_NULL_HANDLE;
		if (vkCreateBuffer(_Device, &bci, NULL, &buffer) != VK_SUCCESS)
			return 0xFFFFFFFFu;
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(_Device, buffer, &req);
		VkMemoryAllocateInfo mai;
		memset(&mai, 0, sizeof(mai));
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		VkDeviceMemory memory = VK_NULL_HANDLE;
		if (vkAllocateMemory(_Device, &mai, NULL, &memory) != VK_SUCCESS)
		{
			vkDestroyBuffer(_Device, buffer, NULL);
			return 0xFFFFFFFFu;
		}
		vkBindBufferMemory(_Device, buffer, memory, 0);
		void *mapped = NULL;
		if (vkMapMemory(_Device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
		{
			vkDestroyBuffer(_Device, buffer, NULL);
			vkFreeMemory(_Device, memory, NULL);
			return 0xFFFFFFFFu;
		}
		// Draws already recorded in this frame reference the old buffer; it is
		// released once no in-flight frame can use it.
		if (_VertexRing)
			deferDestroyBuffer(_VertexRing, _VertexRingMemory);
		_VertexRing = buffer;
		_VertexRingMemory = memory;
		_VertexRingMapped = mapped;
		_VertexRingSize = newSize;
		_VertexRingHalf = newSize / 2;
		_VertexRingFrameBase = _FrameIndex * _VertexRingHalf;
		_VertexRingOffset = _VertexRingFrameBase;
		nlinfo("Vulkan: vertex ring grown to %u bytes", newSize);
	}
	const uint32 offset = (_VertexRingOffset + 3) & ~3u;
	_VertexRingOffset = offset + size;
	return offset;
}

// ---------------------------------------------------------------------------
// Per-frame index upload ring
// ---------------------------------------------------------------------------

VkBuffer CDriverVulkan::getIndexUploadBuffer(const void *data, uint32 size, uint32 stride)
{
	(void)stride;
	const uint32 required = _IndexRingOffset + size;
	if (_IndexRing == VK_NULL_HANDLE || required > _IndexRingFrameBase + _IndexRingHalf)
	{
		// Each frame has its own half: the previous frame may still be reading
		// the indices it copied at the same offsets.
		uint32 newSize = _IndexRingSize ? _IndexRingSize : 2 * 1024 * 1024u;
		while (newSize < size * 2 || newSize <= _IndexRingSize)
			newSize *= 2;
		VkBufferCreateInfo bci;
		memset(&bci, 0, sizeof(bci));
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = newSize;
		bci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkBuffer buffer = VK_NULL_HANDLE;
		if (vkCreateBuffer(_Device, &bci, NULL, &buffer) != VK_SUCCESS)
			return VK_NULL_HANDLE;
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(_Device, buffer, &req);
		VkMemoryAllocateInfo mai;
		memset(&mai, 0, sizeof(mai));
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		VkDeviceMemory memory = VK_NULL_HANDLE;
		if (vkAllocateMemory(_Device, &mai, NULL, &memory) != VK_SUCCESS)
		{
			vkDestroyBuffer(_Device, buffer, NULL);
			return VK_NULL_HANDLE;
		}
		vkBindBufferMemory(_Device, buffer, memory, 0);
		void *mapped = NULL;
		if (vkMapMemory(_Device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
		{
			vkDestroyBuffer(_Device, buffer, NULL);
			vkFreeMemory(_Device, memory, NULL);
			return VK_NULL_HANDLE;
		}
		// The old ring is still referenced by the command buffer being
		// recorded (and possibly the other in-flight frame); release it at the
		// next frame boundary.
		if (_IndexRing)
			deferDestroyBuffer(_IndexRing, _IndexRingMemory);
		_IndexRing = buffer;
		_IndexRingMemory = memory;
		_IndexRingMapped = mapped;
		_IndexRingSize = newSize;
		_IndexRingHalf = newSize / 2;
		_IndexRingFrameBase = _FrameIndex * _IndexRingHalf;
		_IndexRingOffset = _IndexRingFrameBase;
		nlinfo("Vulkan: index ring grown to %u bytes", newSize);
	}
	// Always 4-byte align: 16- and 32-bit index draws share this ring, and
	// vkCmdBindIndexBuffer needs the offset to be a multiple of the index size.
	_IndexRingOffset = (_IndexRingOffset + 3) & ~3u;
	memcpy((uint8 *)_IndexRingMapped + _IndexRingOffset, data, size);
	_IndexRingDrawOffset = _IndexRingOffset;
	_IndexRingOffset += (size + 3) & ~3u;
	return _IndexRing;
}

// ---------------------------------------------------------------------------
// Materials and draw state
// ---------------------------------------------------------------------------

void CDriverVulkan::updateMaterialState(CMaterial &mat)
{
	_BlendEnabled = (mat.getFlags() & IDRV_MAT_BLEND) != 0;
	_SrcBlend = toVkBlend(mat.getSrcBlend());
	_DstBlend = toVkBlend(mat.getDstBlend());
	// Additive blended passes must not add the fog colour on top of the scene:
	// with a fully fogged fragment they would otherwise add fogColor and wash
	// out (or tint) everything behind them. GL3 overrides the fog colour with
	// black for these draws.
	_FogColorOverrideBlack = _BlendEnabled && _DstBlend == VK_BLEND_FACTOR_ONE && _FogEnabled;
	_DepthWriteEnabled = (mat.getFlags() & IDRV_MAT_ZWRITE) != 0;
	_DepthTestEnabled = true;
	_DepthFunc = toVkCompare(mat.getZFunc());
	_MaterialTwoSided = (mat.getFlags() & IDRV_MAT_DOUBLE_SIDED) != 0;
	_MaterialLighting = (mat.getFlags() & IDRV_MAT_LIGHTING) != 0;
	_MaterialLightedVertexColor = (mat.getFlags() & IDRV_MAT_LIGHTED_VERTEX_COLOR) != 0;
	_MaterialAlphaTest = (mat.getFlags() & IDRV_MAT_ALPHA_TEST) != 0;
	_MaterialAlphaRef = mat.getAlphaTestThreshold();
	// Same conversion as the GL3 driver: getZBias() is in NDC units, the
	// viewport depth range is what actually moves the geometry.
	_MaterialZBias = mat.getZBias() * _OODeltaZ;
	_MaterialShader = mat.getShader();
	_MaterialColor = mat.getColor();
	// Lightmapped and specular materials ignore the material colour: the
	// result comes from the textures (and the lightmaps for LightMap).
	if (_MaterialShader == CMaterial::LightMap || _MaterialShader == CMaterial::Specular)
		_MaterialColor = CRGBA(255, 255, 255, _MaterialColor.A);
	for (uint stage = 0; stage < 4; ++stage)
		_TexEnv[stage] = mat.getTexEnvMode(stage);
	_CurrentMaterial = &mat;

	// The water pixel program reads the bump maps' normalisation factors and
	// their texels as unsigned bytes. CTextureBump generates either a signed
	// DsDt bitmap or an RGBA one depending on the requested upload format, so
	// ask for RGBA before the first generation, exactly like GL3's
	// setupWaterPass(). Without this the bitmap stayed DsDt, the RGBA upload
	// could not read it, and the water lost its bump detail (and with it the
	// fine ripple pattern that hides the coarse screen-grid interpolation).
	if (mat.getShader() == CMaterial::Water)
	{
		for (uint stage = 0; stage < 2; ++stage)
		{
			ITexture *texture = mat.getTexture(stage);
			if (texture && texture->isBumpMap())
			{
				texture->setUploadFormat(ITexture::RGBA8888);
				NLMISC::safe_cast<CTextureBump *>(texture)->getNormalizationFactor();
			}
		}
	}

	for (uint stage = 0; stage < 4; ++stage)
		_MappedUV[stage] = 0xFF;
	for (uint stage = 0; stage < IDRV_PROGRAM_MAXSAMPLERS; ++stage)
	{
		ITexture *texture = (stage < IDRV_MAT_MAXTEXTURES) ? mat.getTexture(stage) : NULL;
		// Fast path: this stage still holds the image of that texture and the
		// engine has not touched it since. setupTextureEx() would rebuild the
		// share name (two allocations) and look it up under the driver lock
		// for every draw, which the profile shows as the driver's main cost.
		if (texture && texture == _StageTextureSrc[stage] &&
			_StageTextures[stage] && _StageTextures[stage] == _StageTextureVk[stage] &&
			_StageTextureGen[stage] == _TextureGeneration && !texture->touched())
			continue;
		_StageTextures[stage] = NULL;
		_StageTextureSrc[stage] = NULL;
		_StageTextureVk[stage] = NULL;
		if (!texture)
			continue;
		bool allUploaded = false;
		if (setupTextureEx(*texture, true, allUploaded, false))
		{
			_StageTextures[stage] = getTextureVulkan(*texture);
			if (_StageTextures[stage])
			{
				_StageTextureSrc[stage] = texture;
				_StageTextureVk[stage] = _StageTextures[stage];
				_StageTextureGen[stage] = _TextureGeneration;
			}
			if (_UsedTextureMemorySumEnabled)
			{
				CTextureDrvInfosVulkan *info = findTextureDrvInfos(*texture);
				if (info)
					_TextureUsed.insert(info);
			}
		}
		else if (texture->getWidth() == 0 || texture->getHeight() == 0)
		{
			// The material references a texture that has no image: the file is
			// missing (or could not be decoded). GL binds it anyway and an
			// incomplete texture samples as opaque black there. Dropping the
			// stage instead would make the shader use the vertex/material
			// colour, which is white on the untextured landscape and sky
			// materials and turns a missing texture into a bright patch.
			if (getBlackTexture() != VK_NULL_HANDLE)
			{
				_StageTextures[stage] = &_BlackTexture;
				_StageTextureSrc[stage] = texture;
				_StageTextureVk[stage] = &_BlackTexture;
				_StageTextureGen[stage] = _TextureGeneration;
			}
		}
	}
	_TexturesDirty = true;
}

bool CDriverVulkan::setupMaterial(CMaterial &mat)
{
	updateMaterialState(mat);
	// The depth range is part of the dynamic viewport state in Vulkan. It is
	// re-issued for every material because the material zBias changes it.
	if (_FrameOpen)
		applyDepthRange(_CommandBuffers[_FrameIndex]);
	return true;
}

sint CDriverVulkan::beginMaterialMultiPass()
{
	bool cube = false;
	// The simple path keeps drawing one pass. Only the mega path implements the
	// lightmap passes, so leaving a LightMap material on the simple path keeps
	// its previous single-pass behaviour.
	if (!_MegaEnabled || !_CurrentMaterial || _CurrentMaterial->getShader() != CMaterial::LightMap
		|| !canUseMegaPath(cube))
		return 1;

	computeLightMapInfos(*_CurrentMaterial);
	if (!_LightMapLogged)
	{
		_LightMapLogged = true;
		nlinfo("Vulkan: LightMap mega path active: %u lightmaps in %u pass(es), %u per pass",
			_NLightMaps, _NLightMapPass, _NLightMapPerPass);
	}
	memcpy(_BaseStageTextures, _StageTextures, sizeof(_BaseStageTextures));
	_MegaMultiPassActive = true;
	return (sint)std::max(_NLightMapPass, (uint)1);
}

void CDriverVulkan::setupMaterialPass(uint pass)
{
	if (!_MegaMultiPassActive || !_MegaEnabled || !_CurrentMaterial)
		return;

	CMaterial &mat = *_CurrentMaterial;
	_MegaPassOverrideActive = true;
	_MegaPassDiffuse = 1.f;
	_MegaPassLightMapScale = 1.f;
	memset(_MegaPassConstants, 0, sizeof(_MegaPassConstants));
	memset(_MegaPassSelfIllumination, 0, sizeof(_MegaPassSelfIllumination));
	_MegaPassZeroLightFactors = false;
	_MegaPassFogBlack = false;

	for (uint stage = 0; stage < IDRV_PROGRAM_MAXSAMPLERS; ++stage)
		_StageTextures[stage] = NULL;

	// No usable lightmap: draw the material texture once, with the dynamic
	// lighting kept as-is (no lightmap colour to add).
	if (_NLightMaps == 0)
	{
		ITexture *texture = mat.getTexture(0);
		if (texture)
		{
			bool allUploaded = false;
			if (setupTextureEx(*texture, true, allUploaded, false))
				_StageTextures[0] = getTextureVulkan(*texture);
		}
		// Keep dynamic light only: white diffuse, no specular, no lightmap scale.
		_MegaPassLightMapScale = 1.f;
		return;
	}

	nlassert(pass < _NLightMapPass);

	// Stage layout, same as GL3: 0..nstages-2 are this pass' lightmaps,
	// nstages-1 is the material texture. Lightmap factors become the TexEnv
	// constants so the mega pixel program can accumulate them.
	const uint lmapId = pass * _NLightMapPerPass;
	const uint nstages = std::min(_NLightMapPerPass, _NLightMaps - lmapId) + 1;
	uint r = 0, g = 0, b = 0;
	for (uint sa = 0; sa < nstages - 1; ++sa)
	{
		const CMaterial::CLightMap &lm = mat._LightMaps[_LightMapLUT[lmapId + sa]];
		r += ((uint32)lm.Factor.R * ((uint32)lm.LMCAmbient.R + (lm.LMCAmbient.R >> 7))) >> 8;
		g += ((uint32)lm.Factor.G * ((uint32)lm.LMCAmbient.G + (lm.LMCAmbient.G >> 7))) >> 8;
		b += ((uint32)lm.Factor.B * ((uint32)lm.LMCAmbient.B + (lm.LMCAmbient.B >> 7))) >> 8;
	}
	const CRGBA ambient((uint8)std::min(r, 255u), (uint8)std::min(g, 255u), (uint8)std::min(b, 255u), 255);
	_MegaPassSelfIllumination[0] = ambient.R / 255.f;
	_MegaPassSelfIllumination[1] = ambient.G / 255.f;
	_MegaPassSelfIllumination[2] = ambient.B / 255.f;
	_MegaPassSelfIllumination[3] = 0.f;
	// Additive passes must not add the dynamic light again.
	_MegaPassZeroLightFactors = (pass > 0);
	_MegaPassFogBlack = (pass > 0 && _FogEnabled);

	uint nextLightMap = lmapId;
	for (uint stage = 0; stage < IDRV_PROGRAM_MAXSAMPLERS; ++stage)
	{
		if (stage < nstages - 1)
		{
			const CMaterial::CLightMap &lm = mat._LightMaps[_LightMapLUT[nextLightMap]];
			ITexture *texture = lm.Texture;
			if (texture)
			{
				bool allUploaded = false;
				if (setupTextureEx(*texture, true, allUploaded, false))
					_StageTextures[stage] = getTextureVulkan(*texture);
			}
			// The factor is modulated by the lightmap compression diffuse term,
			// matching GL3 (LMC 8-bit lightmaps).
			CRGBA factor = lm.Factor;
			factor.R = (uint8)(((uint32)factor.R * ((uint32)lm.LMCDiffuse.R + (lm.LMCDiffuse.R >> 7))) >> 8);
			factor.G = (uint8)(((uint32)factor.G * ((uint32)lm.LMCDiffuse.G + (lm.LMCDiffuse.G >> 7))) >> 8);
			factor.B = (uint8)(((uint32)factor.B * ((uint32)lm.LMCDiffuse.B + (lm.LMCDiffuse.B >> 7))) >> 8);
			factor.A = 255;
			_MegaPassConstants[stage][0] = factor.R / 255.f;
			_MegaPassConstants[stage][1] = factor.G / 255.f;
			_MegaPassConstants[stage][2] = factor.B / 255.f;
			_MegaPassConstants[stage][3] = factor.A / 255.f;
			++nextLightMap;
		}
		else if (stage < nstages)
		{
			ITexture *texture = mat.getTexture(0);
			if (texture)
			{
				bool allUploaded = false;
				if (setupTextureEx(*texture, true, allUploaded, false))
					_StageTextures[stage] = getTextureVulkan(*texture);
			}
		}
	}

	// x2 lightmap mode (legacy GL_RGB_SCALE=2): the vertex lighting is halved
	// and the whole lightmap sum is scaled by two in the pixel program.
	_MegaPassLightMapScale = mat._LightMapsMulx2 ? 2.f : 1.f;
	_MegaPassDiffuse = 1.f / _MegaPassLightMapScale;
}

void CDriverVulkan::endMaterialMultiPass()
{
	if (!_MegaMultiPassActive)
		return;
	memcpy(_StageTextures, _BaseStageTextures, sizeof(_BaseStageTextures));
	_MegaMultiPassActive = false;
	_MegaPassOverrideActive = false;
	_MegaPassZeroLightFactors = false;
	_MegaPassFogBlack = false;
	memset(_MegaPassConstants, 0, sizeof(_MegaPassConstants));
	memset(_MegaPassSelfIllumination, 0, sizeof(_MegaPassSelfIllumination));
}

void CDriverVulkan::startSpecularBatch()
{
}

void CDriverVulkan::endSpecularBatch()
{
}

// ---------------------------------------------------------------------------
// Push constants and descriptor sets
// ---------------------------------------------------------------------------

bool CDriverVulkan::commitPushConstants(const CPipelineKey &key, VkCommandBuffer cmd, CVertexBuffer &vb)
{
	SSimpleUniforms u;
	memset(&u, 0, sizeof(u));

	CMatrix mvp = _GLProjMat * _ChangeBasis * _ModelViewMatrix;
	memcpy(u.MVP, mvp.get(), sizeof(u.MVP));

	const float *mv = _ModelViewMatrix.get();
	u.NormalMatrix[0] = mv[0]; u.NormalMatrix[1] = mv[1]; u.NormalMatrix[2] = mv[2]; u.NormalMatrix[3] = 0.f;
	u.NormalMatrix[4] = mv[4]; u.NormalMatrix[5] = mv[5]; u.NormalMatrix[6] = mv[6]; u.NormalMatrix[7] = 0.f;
	u.NormalMatrix[8] = mv[8]; u.NormalMatrix[9] = mv[9]; u.NormalMatrix[10] = mv[10]; u.NormalMatrix[11] = 0.f;

	u.Color[0] = _MaterialColor.R / 255.f;
	u.Color[1] = _MaterialColor.G / 255.f;
	u.Color[2] = _MaterialColor.B / 255.f;
	u.Color[3] = _MaterialColor.A / 255.f;

	// Same split as the mega vertex program: lit draws use the diffuse colour,
	// unlit draws use the material colour.
	if (_CurrentMaterial)
	{
		const CRGBA diffuse = _CurrentMaterial->getDiffuse();
		u.Diffuse[0] = diffuse.R / 255.f;
		u.Diffuse[1] = diffuse.G / 255.f;
		u.Diffuse[2] = diffuse.B / 255.f;
		u.Diffuse[3] = diffuse.A / 255.f;
	}
	else
	{
		u.Diffuse[0] = u.Color[0];
		u.Diffuse[1] = u.Color[1];
		u.Diffuse[2] = u.Color[2];
		u.Diffuse[3] = u.Color[3];
	}

	// Self-illumination staged exactly like the mega object UBO: the global
	// ambient plus the per-light ambient (light table ambient for this object
	// in table mode) modulates the material ambient, then the material
	// emissive is added. The vertex program adds this term after the lights,
	// so it must not be multiplied by the material diffuse.
	CRGBAF selfIllumination(_AmbientColor);
	if (_CurrentMaterial)
	{
		if (_LightTableMode)
			selfIllumination += CRGBAF(_ObjectAmbient);
		else
			for (uint i = 0; i < 8; ++i)
				if (_Lights[i].Enabled)
					selfIllumination += CRGBAF(_Lights[i].Ambient);
		selfIllumination *= CRGBAF(_CurrentMaterial->getAmbient());
		if (_CurrentMaterial->getShader() != CMaterial::LightMap)
			selfIllumination += CRGBAF(_CurrentMaterial->getEmissive());
	}
	u.Ambient[0] = selfIllumination.R;
	u.Ambient[1] = selfIllumination.G;
	u.Ambient[2] = selfIllumination.B;
	u.Ambient[3] = 1.f;

	u.AlphaRef = _MaterialAlphaRef;

	int flags = 0;
	for (uint stage = 0; stage < 4; ++stage)
		if (_StageTextures[stage])
			flags |= (1 << stage);
	if (_MaterialAlphaTest)
		flags |= 16;
	if (_MaterialLighting)
		flags |= 32;
	if (_MaterialLightedVertexColor)
		flags |= 64;
	u.Flags = flags;

	for (uint stage = 0; stage < 4; ++stage)
	{
		u.TexEnv[stage] = _TexEnv[stage];
		if (_CurrentMaterial)
		{
			const CRGBA &constant = _CurrentMaterial->_TexEnvs[stage].ConstantColor;
			u.TexEnvConstant[stage][0] = constant.R / 255.f;
			u.TexEnvConstant[stage][1] = constant.G / 255.f;
			u.TexEnvConstant[stage][2] = constant.B / 255.f;
			u.TexEnvConstant[stage][3] = constant.A / 255.f;
		}
	}

	u.UvRouting = 0;
	const uint8 *routing = vb.getUVRouting();
	for (uint stage = 0; stage < 4; ++stage)
	{
		uint8 route = routing ? routing[stage] : (uint8)stage;
		if (_MappedUV[stage] < 4)
			route = _MappedUV[stage];
		u.UvRouting |= ((uint32)(route & 0xF)) << (stage * 4);
	}

	// A render target image is sampled with V = 0 at the top, while NeL texture
	// coordinates put V = 0 at the bottom. Every stage bound to a target is
	// flipped in the pixel shader (the bloom passes and the final scene blit
	// would otherwise be upside down).
	u.RtFlipMask = 0;
	for (uint stage = 0; stage < 4; ++stage)
	{
		const ITexture *texture = _CurrentMaterial->getTexture(stage);
		// Only NeL offscreen textures. CTextureMem render targets (shadow maps
		// and their blur textures) go through the shadow map manager's copy
		// pass, which already accounts for the GL bottom-up convention; flipping
		// them here moves the projected shadow off the receiver.
		if (texture && texture->getRenderTarget() && texture->isOffscreenTexture())
			u.RtFlipMask |= (1u << stage);
	}

	// Resolve up to four lights for this object (std140: separate vec4 arrays).
	uint lightCount = 0;
	if (_LightTableMode && _LightCount > 0)
	{
		for (uint i = 0; i < _LightCount && lightCount < 4; ++i)
		{
			sint16 index = _LightIndices[i];
			if (index < 0 || (uint)index >= _LightTableSize)
				continue;
			const SLightState &light = _LightTable[index];
			// Same expansion as the mega object UBO: 0..255 -> 0..1 in 1/256 steps.
			float factor = (float)(_LightFactors[i] + (_LightFactors[i] >> 7)) / 256.f;
			// The shader lights eye-space normals, so transform the world-space
			// light data exactly like the mega vertex program's viewMatrix does.
			const CVector lightEye = light.Mode == CLight::DirectionalLight
				? _ViewMatrix.mulVector(light.DirOrPos)
				: _ViewMatrix.mulPoint(light.DirOrPos);
			u.LightDirOrPos[lightCount][0] = lightEye.x;
			u.LightDirOrPos[lightCount][1] = lightEye.y;
			u.LightDirOrPos[lightCount][2] = lightEye.z;
			u.LightDirOrPos[lightCount][3] = light.Mode == 0 ? 0.f : 1.f;
			u.LightDiffuse[lightCount][0] = (light.Diffuse.R / 255.f) * factor;
			u.LightDiffuse[lightCount][1] = (light.Diffuse.G / 255.f) * factor;
			u.LightDiffuse[lightCount][2] = (light.Diffuse.B / 255.f) * factor;
			u.LightDiffuse[lightCount][3] = 1.f;
			++lightCount;
		}
	}
	else
	{
		for (uint i = 0; i < 8 && lightCount < 4; ++i)
		{
			if (!_Lights[i].Enabled)
				continue;
			const CVector lightEye = _Lights[i].Mode == CLight::DirectionalLight
				? _ViewMatrix.mulVector(_Lights[i].DirOrPos)
				: _ViewMatrix.mulPoint(_Lights[i].DirOrPos);
			u.LightDirOrPos[lightCount][0] = lightEye.x;
			u.LightDirOrPos[lightCount][1] = lightEye.y;
			u.LightDirOrPos[lightCount][2] = lightEye.z;
			u.LightDirOrPos[lightCount][3] = _Lights[i].Mode == 0 ? 0.f : 1.f;
			u.LightDiffuse[lightCount][0] = _Lights[i].Diffuse.R / 255.f;
			u.LightDiffuse[lightCount][1] = _Lights[i].Diffuse.G / 255.f;
			u.LightDiffuse[lightCount][2] = _Lights[i].Diffuse.B / 255.f;
			u.LightDiffuse[lightCount][3] = 1.f;
			++lightCount;
		}
	}
	u.LightCount = (int)lightCount;

	// Eye-space position (texgen, fog) and the reflection lookup matrix.
	memcpy(u.ModelView, _ModelViewMatrix.get(), sizeof(u.ModelView));
	memcpy(u.SpecularTexMtx, stageMegaCameraUBO().specularTexMtx, sizeof(u.SpecularTexMtx));

	// Texture coordinate generation and user texture matrices. The shadow
	// receiver is the main user of object-space generation: it draws meshes
	// with no vertex UVs and projects the shadow map through a user matrix.
	const uint uintTexMatMask = (uint)(_CurrentMaterial->getFlags() & IDRV_MAT_USER_TEX_MAT_ALL) >> IDRV_MAT_USER_TEX_FIRST_BIT;
	static const float identityPlane[4][4] =
	{
		{ 1.f, 0.f, 0.f, 0.f },
		{ 0.f, 1.f, 0.f, 0.f },
		{ 0.f, 0.f, 1.f, 0.f },
		{ 0.f, 0.f, 0.f, 1.f },
	};
	for (uint stage = 0; stage < 4; ++stage)
	{
		sint32 texGenMode = -1;
		const ITexture *texture = _CurrentMaterial->getTexture(stage);
		if (texture && _CurrentMaterial->getTexCoordGen(stage))
		{
			switch (_CurrentMaterial->getTexCoordGenMode(stage))
			{
			case CMaterial::TexCoordGenReflect:
				texGenMode = texture->isTextureCube() ? SimpleTexGenReflectionMap : SimpleTexGenSphereMap;
				break;
			case CMaterial::TexCoordGenObjectSpace:
				texGenMode = SimpleTexGenObjectLinear;
				break;
			case CMaterial::TexCoordGenEyeSpace:
				texGenMode = SimpleTexGenEyeLinear;
				break;
			}
		}
		u.TexGenMode[stage] = texGenMode;
		memcpy(u.TexGenPlane[stage], identityPlane, sizeof(identityPlane));
		if (uintTexMatMask & (1u << stage))
			memcpy(u.TexMatrix[stage], _CurrentMaterial->getUserTexMat(stage).get(), 16 * sizeof(float));
		else
			memcpy(u.TexMatrix[stage], CMatrix::Identity.get(), 16 * sizeof(float));
	}

	// Fog, with the same additive-blend black override as the mega path.
	CRGBA fogColor = _FogColorOverrideBlack ? CRGBA(0, 0, 0, 0) : _FogColor;
	u.FogColor[0] = fogColor.R / 255.f;
	u.FogColor[1] = fogColor.G / 255.f;
	u.FogColor[2] = fogColor.B / 255.f;
	u.FogColor[3] = fogColor.A / 255.f;
	u.FogParams[0] = _FogStart;
	u.FogParams[1] = _FogEnd;
	u.FogDensity = _FogDensity;
	u.FogMode = (sint32)_FogMode;
	u.FogEnabled = _FogEnabled ? 1 : 0;

	// Upload into the per-frame uniform ring.
	const uint32 slot = (sizeof(u) + 15) & ~15u;
	if (_UniformRingOffset + slot > _UniformRingFrameBase + _UniformRingHalf)
	{
		if (!growUniformRing(slot))
			return false;
	}
	uint8 *dst = (uint8 *)_UniformRingMapped + _UniformRingOffset;
	memcpy(dst, &u, sizeof(u));

	VkDescriptorBufferInfo bufferInfo;
	bufferInfo.buffer = _UniformRing;
	bufferInfo.offset = _UniformRingOffset;
	bufferInfo.range = sizeof(u);
	_UniformRingOffset += slot;

	VkDescriptorSet descriptorSet = allocDescriptorSet(_DescriptorSetLayout);
	if (descriptorSet == VK_NULL_HANDLE)
		return false;
	if (_StatsEnabled)
		++_StatsDescriptorSets;

	VkDescriptorImageInfo infos[4];
	for (uint stage = 0; stage < 4; ++stage)
	{
		CTextureVulkan *tex = _StageTextures[stage];
		// The simple fragment shader declares every stage as sampler2D. A cube
		// texture on a draw that the mega path cannot take (a non-stage-1 cube)
		// would be an incompatible descriptor; bind the white 2D texture there.
		if (tex && tex->Cube)
			tex = NULL;
		infos[stage].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		infos[stage].imageView = tex ? tex->View : getWhiteTexture();
		infos[stage].sampler = tex ? tex->Sampler : _WhiteTexture.Sampler;
	}
	VkWriteDescriptorSet writes[5];
	memset(writes, 0, sizeof(writes));
	for (uint stage = 0; stage < 4; ++stage)
	{
		writes[stage].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[stage].dstSet = descriptorSet;
		writes[stage].dstBinding = stage;
		writes[stage].descriptorCount = 1;
		writes[stage].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[stage].pImageInfo = &infos[stage];
	}
	writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[4].dstSet = descriptorSet;
	writes[4].dstBinding = 4;
	writes[4].descriptorCount = 1;
	writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[4].pBufferInfo = &bufferInfo;
	vkUpdateDescriptorSets(_Device, 5, writes, 0, NULL);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _PipelineLayout, 0, 1, &descriptorSet, 0, NULL);
	setBlendConstants(cmd);
	return true;
}

// ---------------------------------------------------------------------------
// Draws
// ---------------------------------------------------------------------------

/// vkCmdSetBlendConstants() only when the value differs from the one already
/// set in this command buffer; most consecutive draws share it.
void CDriverVulkan::setBlendConstants(VkCommandBuffer cmd)
{
	const float value[4] = {
		_BlendConstantColor.R / 255.f,
		_BlendConstantColor.G / 255.f,
		_BlendConstantColor.B / 255.f,
		_BlendConstantColor.A / 255.f,
	};
	if (_BlendConstantsSet && memcmp(value, _BlendConstants, sizeof(value)) == 0)
		return;
	vkCmdSetBlendConstants(cmd, value);
	memcpy(_BlendConstants, value, sizeof(value));
	_BlendConstantsSet = true;
}

/// vkCmdBindPipeline() only when it differs from the pipeline already bound in
/// this command buffer. Pipelines are render-pass compatible by construction
/// (the key carries the target), so the skip is always valid.
void CDriverVulkan::bindPipeline(VkCommandBuffer cmd, VkPipeline pipeline)
{
	if (pipeline == _LastPipeline)
		return;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	_LastPipeline = pipeline;
	if (_StatsEnabled)
		++_StatsPipelineBinds;
}

bool CDriverVulkan::drawArrays(CMaterial &mat, uint32 firstVertex, uint32 count, VkPrimitiveTopology topo)
{
	if (!_CurrentVB || count == 0)
		return false;
	if (!_FrameOpen && !beginFrame())
		return false;

	CVBDrvInfosVulkan *info = dynamic_cast<CVBDrvInfosVulkan *>((IVBDrvInfos *)_CurrentVB->DrvInfos);
	if (!info || !info->currentBuffer())
		return false;

	CPipelineKey key;
	key.VertexFormat = makeVertexLayoutHash(*_CurrentVB);
	key.VertexFlags = _CurrentVB->getVertexFormat();
	key.VertexStride = _CurrentVB->getVertexSize();
	key.Topology = (uint32)topo;
	key.BlendEnable = _BlendEnabled ? 1 : 0;
	key.SrcBlend = _SrcBlend;
	key.DstBlend = _DstBlend;
	key.DepthTest = _DepthTestEnabled ? 1 : 0;
	key.DepthWrite = _DepthWriteEnabled ? 1 : 0;
	key.DepthCompare = _DepthFunc;
	key.CullMode = noCullEnabled() ? VK_CULL_MODE_NONE : (_MaterialTwoSided ? VK_CULL_MODE_NONE : (_CullMode == CW ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT));
	key.FrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	key.ColorWriteMask = _ClearColorMask;
	key.AlphaTest = _MaterialAlphaTest ? 1 : 0;
	key.HasTexture0 = _StageTextures[0] ? 1 : 0;
	fillMegaPipelineKey(key);

	VkPipeline pipeline = getPipeline(key);
	if (pipeline == VK_NULL_HANDLE)
		return false;

	VkCommandBuffer cmd = _CommandBuffers[_FrameIndex];
	bindPipeline(cmd, pipeline);
	if (_StatsEnabled)
	{
		for (uint stage = 0; stage < 4; ++stage)
			if (_StageTextures[stage])
				++_StatsTextureBinds;
		countVertexFormat(key.VertexFlags, key.VertexStride);
	}
	if (key.Mega)
	{
		if (!bindMegaDescriptors(cmd, *_CurrentVB))
			return false;
	}
	else
	{
		if (!commitPushConstants(key, cmd, *_CurrentVB))
			return false;
	}

	VkDeviceSize offset = info->currentOffset();
	VkBuffer vb = info->currentBuffer();
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	vkCmdDraw(cmd, count, 1, firstVertex, 0);
	info->_Drawn = true;
	++_ArrayDrawCount;
	return true;
}

bool CDriverVulkan::drawIndexed(CMaterial &mat, uint32 firstIndex, uint32 count, VkPrimitiveTopology topo, uint32 indexStride)
{
	if (!_CurrentVB || !_IndexValues || count == 0)
		return false;
	if (!_FrameOpen && !beginFrame())
		return false;

	const uint64 statsStart = _StatsEnabled ? nowNs() : 0;

	CVBDrvInfosVulkan *info = dynamic_cast<CVBDrvInfosVulkan *>((IVBDrvInfos *)_CurrentVB->DrvInfos);
	if (!info || !info->currentBuffer())
		return false;

	uint32 bytes = count * indexStride;
	if (bytes == 0)
		return false;
	VkBuffer indexBuffer = getIndexUploadBuffer(_IndexValues + (uint64)firstIndex * indexStride, bytes, indexStride);
	if (indexBuffer == VK_NULL_HANDLE)
		return false;

	const uint64 statsIndexed = _StatsEnabled ? nowNs() : 0;

	CPipelineKey key;
	key.VertexFormat = makeVertexLayoutHash(*_CurrentVB);
	key.VertexFlags = _CurrentVB->getVertexFormat();
	key.VertexStride = _CurrentVB->getVertexSize();
	key.Topology = (uint32)topo;
	key.BlendEnable = _BlendEnabled ? 1 : 0;
	key.SrcBlend = _SrcBlend;
	key.DstBlend = _DstBlend;
	key.DepthTest = _DepthTestEnabled ? 1 : 0;
	key.DepthWrite = _DepthWriteEnabled ? 1 : 0;
	key.DepthCompare = _DepthFunc;
	key.CullMode = noCullEnabled() ? VK_CULL_MODE_NONE : (_MaterialTwoSided ? VK_CULL_MODE_NONE : (_CullMode == CW ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT));
	key.FrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	key.ColorWriteMask = _ClearColorMask;
	key.AlphaTest = _MaterialAlphaTest ? 1 : 0;
	key.HasTexture0 = _StageTextures[0] ? 1 : 0;
	fillMegaPipelineKey(key);
	const uint64 statsKeyEnd = _StatsEnabled ? nowNs() : 0;

	if (_IndexedDrawCount == 0)
	{
		nlinfo("Vulkan: first indexed draw: count=%u stride=%u vflags=0x%x mat flags=0x%x diffuse=(%u,%u,%u,%u) tex0=%p blend=%d zfunc=%d",
			count, _CurrentVB->getVertexSize(), _CurrentVB->getVertexFormat(), mat.getFlags(),
			mat.getDiffuse().R, mat.getDiffuse().G, mat.getDiffuse().B, mat.getDiffuse().A,
			(void *)mat.getTexture(0), (int)_BlendEnabled, (int)mat.getZFunc());
		nlinfo("Vulkan: mvp row0 = %f %f %f %f", _ModelViewMatrix.get()[0], _ModelViewMatrix.get()[1], _ModelViewMatrix.get()[2], _ModelViewMatrix.get()[3]);
		nlinfo("Vulkan: proj = %f %f %f %f", _GLProjMat.get()[0], _GLProjMat.get()[1], _GLProjMat.get()[2], _GLProjMat.get()[3]);
	}

	VkPipeline pipeline = getPipeline(key);
	if (pipeline == VK_NULL_HANDLE)
		return false;
	const uint64 statsPipelineEnd = _StatsEnabled ? nowNs() : 0;

	VkCommandBuffer cmd = _CommandBuffers[_FrameIndex];
	bindPipeline(cmd, pipeline);
	if (_StatsEnabled)
	{
		for (uint stage = 0; stage < 4; ++stage)
			if (_StageTextures[stage])
				++_StatsTextureBinds;
		countVertexFormat(key.VertexFlags, key.VertexStride);
	}
	if (key.Mega)
	{
		if (!bindMegaDescriptors(cmd, *_CurrentVB))
			return false;
	}
	else
	{
		if (!commitPushConstants(key, cmd, *_CurrentVB))
			return false;
	}
	const uint64 statsBindEnd = _StatsEnabled ? nowNs() : 0;

	VkDeviceSize offset = info->currentOffset();
	VkBuffer vb = info->currentBuffer();
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	vkCmdBindIndexBuffer(cmd, indexBuffer, _IndexRingDrawOffset,
		indexStride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(cmd, count, 1, 0, 0, 0);
	info->_Drawn = true;
	++_IndexedDrawCount;
	if (_StatsEnabled)
	{
		const uint64 statsEnd = nowNs();
		++_StatsDraws;
		_StatsIndexNs += statsIndexed - statsStart;
		_StatsDescriptorNs += statsEnd - statsIndexed;
		_StatsKeyNs += statsKeyEnd - statsIndexed;
		_StatsPipelineNs += statsPipelineEnd - statsKeyEnd;
		_StatsBindNs += statsBindEnd - statsPipelineEnd;
		_StatsSubmitNs += statsEnd - statsBindEnd;
	}
	return true;
}

void CDriverVulkan::mapTextureStageToUV(uint stage, uint uvId)
{
	if (stage < 4)
		_MappedUV[stage] = (uint8)uvId;
}

bool CDriverVulkan::renderTriangles(CMaterial &mat, uint32 firstIndex, uint32 nlines)
{
	if (!setupMaterial(mat))
		return false;
	sint passes = beginMaterialMultiPass();
	bool ok = true;
	for (sint pass = 0; pass < passes; ++pass)
	{
		setupMaterialPass((uint)pass);
		if (!drawIndexed(mat, firstIndex, nlines * 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, _IndexFormat / 8))
		{
			ok = false;
			break;
		}
	}
	endMaterialMultiPass();
	return ok;
}

bool CDriverVulkan::renderLines(CMaterial &mat, uint32 firstIndex, uint32 nlines)
{
	if (!setupMaterial(mat))
		return false;
	sint passes = beginMaterialMultiPass();
	bool ok = true;
	for (sint pass = 0; pass < passes; ++pass)
	{
		setupMaterialPass((uint)pass);
		if (!drawIndexed(mat, firstIndex, nlines * 2, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, _IndexFormat / 8))
		{
			ok = false;
			break;
		}
	}
	endMaterialMultiPass();
	return ok;
}

bool CDriverVulkan::renderSimpleTriangles(uint32 firstIndex, uint32 ntris)
{
	return drawIndexed(*_CurrentMaterial, firstIndex, ntris * 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, _IndexFormat / 8);
}

bool CDriverVulkan::renderRawTriangles(CMaterial &mat, uint32 startIndex, uint32 numTris)
{
	if (!setupMaterial(mat))
		return false;
	sint passes = beginMaterialMultiPass();
	bool ok = true;
	for (sint pass = 0; pass < passes; ++pass)
	{
		setupMaterialPass((uint)pass);
		// The engine's index is a triangle index (GL3 passes startIndex*3 to
		// glDrawArrays); CWaterModel::fillVBHard starts at startTri*vtx*3.
		if (!drawArrays(mat, startIndex * 3, numTris * 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST))
		{
			ok = false;
			break;
		}
	}
	endMaterialMultiPass();
	return ok;
}

bool CDriverVulkan::renderRawLines(CMaterial &mat, uint32 startIndex, uint32 numLines)
{
	if (!setupMaterial(mat))
		return false;
	sint passes = beginMaterialMultiPass();
	bool ok = true;
	for (sint pass = 0; pass < passes; ++pass)
	{
		setupMaterialPass((uint)pass);
		// Line index, like GL3's startIndex << 1.
		if (!drawArrays(mat, startIndex * 2, numLines * 2, VK_PRIMITIVE_TOPOLOGY_LINE_LIST))
		{
			ok = false;
			break;
		}
	}
	endMaterialMultiPass();
	return ok;
}

bool CDriverVulkan::renderRawPoints(CMaterial &mat, uint32 startIndex, uint32 numPoints)
{
	if (!setupMaterial(mat))
		return false;
	sint passes = beginMaterialMultiPass();
	bool ok = true;
	for (sint pass = 0; pass < passes; ++pass)
	{
		setupMaterialPass((uint)pass);
		if (!drawArrays(mat, startIndex, numPoints, VK_PRIMITIVE_TOPOLOGY_POINT_LIST))
		{
			ok = false;
			break;
		}
	}
	endMaterialMultiPass();
	return ok;
}

bool CDriverVulkan::renderRawQuads(CMaterial &mat, uint32 startVertex, uint32 numQuads)
{
	if (numQuads == 0)
		return true;
	if (!setupMaterial(mat))
		return false;

	// Same quad index pattern as the GL3 driver: (0,1,2) and (0,2,3).
	const uint32 QUAD_BATCH_SIZE = 2048;
	static uint16 s_QuadIndices[QUAD_BATCH_SIZE * 6];
	static bool initialized = false;
	if (!initialized)
	{
		for (uint32 k = 0; k < QUAD_BATCH_SIZE; ++k)
		{
			s_QuadIndices[k * 6 + 0] = (uint16)(k * 4 + 0);
			s_QuadIndices[k * 6 + 1] = (uint16)(k * 4 + 1);
			s_QuadIndices[k * 6 + 2] = (uint16)(k * 4 + 2);
			s_QuadIndices[k * 6 + 3] = (uint16)(k * 4 + 0);
			s_QuadIndices[k * 6 + 4] = (uint16)(k * 4 + 2);
			s_QuadIndices[k * 6 + 5] = (uint16)(k * 4 + 3);
		}
		initialized = true;
	}

	if (!_CurrentVB)
		return false;
	if (!_FrameOpen && !beginFrame())
		return false;
	CVBDrvInfosVulkan *info = dynamic_cast<CVBDrvInfosVulkan *>((IVBDrvInfos *)_CurrentVB->DrvInfos);
	if (!info || !info->currentBuffer())
		return false;

	sint passes = beginMaterialMultiPass();
	bool ok = true;
	for (sint pass = 0; pass < passes && ok; ++pass)
	{
		setupMaterialPass((uint)pass);
		uint32 remaining = numQuads;
		uint32 base = 0;
		while (remaining > 0)
		{
			uint32 batch = std::min(remaining, QUAD_BATCH_SIZE);
			uint32 bytes = batch * 6 * sizeof(uint16);
			VkBuffer indexBuffer = getIndexUploadBuffer(s_QuadIndices, bytes, sizeof(uint16));
			if (indexBuffer == VK_NULL_HANDLE)
			{
				ok = false;
				break;
			}

			CPipelineKey key;
			key.VertexFormat = makeVertexLayoutHash(*_CurrentVB);
			key.VertexFlags = _CurrentVB->getVertexFormat();
			key.VertexStride = _CurrentVB->getVertexSize();
			key.Topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			key.BlendEnable = _BlendEnabled ? 1 : 0;
			key.SrcBlend = _SrcBlend;
			key.DstBlend = _DstBlend;
			key.DepthTest = _DepthTestEnabled ? 1 : 0;
			key.DepthWrite = _DepthWriteEnabled ? 1 : 0;
			key.DepthCompare = _DepthFunc;
			key.CullMode = noCullEnabled() ? VK_CULL_MODE_NONE : (_MaterialTwoSided ? VK_CULL_MODE_NONE : (_CullMode == CW ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT));
			key.FrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
			key.ColorWriteMask = _ClearColorMask;
			key.AlphaTest = _MaterialAlphaTest ? 1 : 0;
			key.HasTexture0 = _StageTextures[0] ? 1 : 0;
			fillMegaPipelineKey(key);

			VkPipeline pipeline = getPipeline(key);
			if (pipeline == VK_NULL_HANDLE)
			{
				ok = false;
				break;
			}

			VkCommandBuffer cmd = _CommandBuffers[_FrameIndex];
			bindPipeline(cmd, pipeline);
			if (key.Mega)
			{
				if (!bindMegaDescriptors(cmd, *_CurrentVB))
				{
					ok = false;
					break;
				}
			}
			else
			{
				if (!commitPushConstants(key, cmd, *_CurrentVB))
				{
					ok = false;
					break;
				}
			}
			VkDeviceSize offset = info->currentOffset();
			VkBuffer vb = info->currentBuffer();
			vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
			vkCmdBindIndexBuffer(cmd, indexBuffer, _IndexRingDrawOffset, VK_INDEX_TYPE_UINT16);
			info->_Drawn = true;

			// The quad index pattern addresses vertices relative to the batch start.
			sint32 firstVertex = (sint32)(startVertex + base);
			vkCmdDrawIndexed(cmd, batch * 6, 1, 0, firstVertex, 0);
			++_IndexedDrawCount;

			base += batch * 4;
			remaining -= batch;
		}
	}
	endMaterialMultiPass();
	return ok;
}

bool CDriverVulkan::renderTrianglesWithIndexOffset(CMaterial &mat, uint32 firstIndex, uint32 ntris, uint indexOffset)
{
	if (!setupMaterial(mat))
		return false;
	sint passes = beginMaterialMultiPass();
	bool ok = true;
	for (sint pass = 0; pass < passes; ++pass)
	{
		setupMaterialPass((uint)pass);
		if (!drawIndexed(mat, firstIndex, ntris * 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, _IndexFormat / 8))
		{
			ok = false;
			break;
		}
	}
	endMaterialMultiPass();
	return ok;
}

bool CDriverVulkan::renderLinesWithIndexOffset(CMaterial &mat, uint32 firstIndex, uint32 nlines, uint indexOffset)
{
	if (!setupMaterial(mat))
		return false;
	sint passes = beginMaterialMultiPass();
	bool ok = true;
	for (sint pass = 0; pass < passes; ++pass)
	{
		setupMaterialPass((uint)pass);
		if (!drawIndexed(mat, firstIndex, nlines * 2, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, _IndexFormat / 8))
		{
			ok = false;
			break;
		}
	}
	endMaterialMultiPass();
	return ok;
}

bool CDriverVulkan::renderSimpleTrianglesWithIndexOffset(uint32 firstIndex, uint32 ntris, uint indexOffset)
{
	return drawIndexed(*_CurrentMaterial, firstIndex, ntris * 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, _IndexFormat / 8);
}

// ---------------------------------------------------------------------------
// Lights
// ---------------------------------------------------------------------------

/// Pack a CLight into the std140 layout the mega shaders read from NlLightTable.
static void packLightToEntry(CLightTableUBOEntry &e, const CLight &light)
{
	const CLight::TLightMode mode = light.getMode();
	e.mode = (sint32)mode;

	if (mode == CLight::DirectionalLight)
	{
		const CVector dir = light.getDirection();
		e.dirOrPos[0] = dir.x;
		e.dirOrPos[1] = dir.y;
		e.dirOrPos[2] = dir.z;
	}
	else
	{
		const CVector pos = light.getPosition();
		e.dirOrPos[0] = pos.x;
		e.dirOrPos[1] = pos.y;
		e.dirOrPos[2] = pos.z;
	}

	const CRGBAF diff(light.getDiffuse());
	e.diffuse[0] = diff.R;
	e.diffuse[1] = diff.G;
	e.diffuse[2] = diff.B;
	e.diffuse[3] = diff.A;

	const CRGBAF spec(light.getSpecular());
	e.specular[0] = spec.R;
	e.specular[1] = spec.G;
	e.specular[2] = spec.B;
	e.specular[3] = spec.A;

	e.constAttn = light.getConstantAttenuation();
	e.linAttn = light.getLinearAttenuation();
	e.quadAttn = light.getQuadraticAttenuation();
	e.spotExp = light.getExponent();

	const CVector spotDir = light.getDirection();
	e.spotDir[0] = spotDir.x;
	e.spotDir[1] = spotDir.y;
	e.spotDir[2] = spotDir.z;
	e.spotCutoff = cosf(light.getCutoff());

	const CRGBAF amb(light.getAmbiant());
	e.ambient[0] = amb.R;
	e.ambient[1] = amb.G;
	e.ambient[2] = amb.B;
	e.ambient[3] = amb.A;
}

void CDriverVulkan::setLight(uint8 num, const CLight &light)
{
	_MegaLightTableValid = false;
	if (num < 8)
	{
		_Lights[num].Mode = (uint8)light.getMode();
		_Lights[num].DirOrPos = (_Lights[num].Mode == CLight::DirectionalLight) ? light.getDirection() : light.getPosition();
		_Lights[num].Diffuse = light.getDiffuse();
		_Lights[num].Ambient = light.getAmbiant();
		_Lights[num].Enabled = true;
		packLightToEntry(_Lights[num].Entry, light);
	}
}

void CDriverVulkan::enableLight(uint8 num, bool enable)
{
	if (num < 8)
	{
		_Lights[num].Enabled = enable;
	}
}

void CDriverVulkan::setAmbientColor(CRGBA color)
{
	_AmbientColor = color;
}

void CDriverVulkan::setPerPixelLightingLight(CRGBA diffuse, CRGBA specular, float shininess)
{
}

void CDriverVulkan::setLightMapDynamicLight(bool enable, const CLight &light)
{
}

void CDriverVulkan::enableLightTableMode(bool enable)
{
	_MegaLightTableValid = false;
	_LightTableMode = enable;
}

void CDriverVulkan::setLightTableSize(uint count)
{
	_MegaLightTableValid = false;
	_LightTableSize = std::min(count, 128u);
}

void CDriverVulkan::setLightTableEntry(uint index, const CLight &light)
{
	_MegaLightTableValid = false;
	if (index < 128)
	{
		_LightTable[index].Mode = (uint8)light.getMode();
		_LightTable[index].DirOrPos = (_LightTable[index].Mode == CLight::DirectionalLight) ? light.getDirection() : light.getPosition();
		_LightTable[index].Diffuse = light.getDiffuse();
		_LightTable[index].Ambient = light.getAmbiant();
		_LightTable[index].Enabled = true;
		packLightToEntry(_LightTable[index].Entry, light);
	}
}

void CDriverVulkan::setLights(const sint16 *tableIndices, const uint8 *factors, uint numLights, uint numPerPixelLights, CRGBA ambient)
{
	// Clear all slots first: the mega shaders read eight packed indices.
	for (uint i = 0; i < 8; ++i)
	{
		_LightIndices[i] = -1;
		_LightFactors[i] = 0;
	}
	_LightCount = std::min(numLights, 8u);
	_NumPerPixelLights = std::min(numPerPixelLights, _LightCount);
	for (uint i = 0; i < _LightCount; ++i)
	{
		_LightIndices[i] = tableIndices[i];
		_LightFactors[i] = factors[i];
	}
	_ObjectAmbient = ambient;
}

// ---------------------------------------------------------------------------
// Matrices / viewport / scissor
// ---------------------------------------------------------------------------

void CDriverVulkan::setFrustum(float left, float right, float bottom, float top, float znear, float zfar, bool perspective)
{
	if (perspective)
		_GLProjMat.frustum(left, right, bottom, top, znear, zfar);
	else
		_GLProjMat.ortho(left, right, bottom, top, znear, zfar);
	if (zfar != znear)
		_OODeltaZ = 1.f / (zfar - znear);
	_MegaCameraDirty = true;
}

void CDriverVulkan::setFrustumMatrix(CMatrix &frustumMatrix)
{
	_GLProjMat = frustumMatrix;
	_MegaCameraDirty = true;
}

CMatrix CDriverVulkan::getFrustumMatrix()
{
	return _GLProjMat;
}

void CDriverVulkan::setupViewMatrix(const CMatrix &mtx)
{
	_ViewMatrix = mtx;
	_ModelViewMatrix = mtx * _ModelMatrix;
	_MegaCameraDirty = true;
}

void CDriverVulkan::setupViewMatrixEx(const CMatrix &mtx, const CVector &cameraPos)
{
	_ViewMatrix = mtx;
	_ModelViewMatrix = mtx * _ModelMatrix;
	_MegaCameraDirty = true;
}

void CDriverVulkan::setupModelMatrix(const CMatrix &mtx)
{
	_ModelMatrix = mtx;
	_ModelViewMatrix = _ViewMatrix * mtx;
}

CMatrix CDriverVulkan::getViewMatrix() const
{
	return _ViewMatrix;
}

void CDriverVulkan::setupViewport(const CViewport &viewport)
{
	_Viewport = viewport;
	if (_FrameOpen)
	{
		VkViewport vk = computeViewport(currentTargetExtent());
		vkCmdSetViewport(_CommandBuffers[_FrameIndex], 0, 1, &vk);
	}
}

void CDriverVulkan::getViewport(CViewport &viewport)
{
	viewport = _Viewport;
}

void CDriverVulkan::setupScissor(const CScissor &scissor)
{
	_Scissor = scissor;
	if (_FrameOpen)
	{
		VkExtent2D extent = currentTargetExtent();
		VkRect2D rect;
		rect.offset.x = (sint32)(scissor.X * extent.width);
		rect.offset.y = (sint32)(extent.height - (scissor.Y + scissor.Height) * extent.height);
		rect.extent.width = (uint32)(scissor.Width * extent.width);
		rect.extent.height = (uint32)(scissor.Height * extent.height);
		vkCmdSetScissor(_CommandBuffers[_FrameIndex], 0, 1, &rect);
	}
}

// ---------------------------------------------------------------------------
// Simple state
// ---------------------------------------------------------------------------

void CDriverVulkan::setCullMode(IDriver::TCullMode cullMode)
{
	_CullMode = cullMode;
}

IDriver::TCullMode CDriverVulkan::getCullMode() const
{
	return _CullMode;
}

void CDriverVulkan::setColorMask(bool bRed, bool bGreen, bool bBlue, bool bAlpha)
{
	_ClearColorMask = (bRed ? VK_COLOR_COMPONENT_R_BIT : 0) |
		(bGreen ? VK_COLOR_COMPONENT_G_BIT : 0) |
		(bBlue ? VK_COLOR_COMPONENT_B_BIT : 0) |
		(bAlpha ? VK_COLOR_COMPONENT_A_BIT : 0);
}

void CDriverVulkan::setBlendConstantColor(CRGBA col)
{
	_BlendConstantColor = col;
}

CRGBA CDriverVulkan::getBlendConstantColor() const
{
	return _BlendConstantColor;
}

bool CDriverVulkan::fogEnabled()
{
	return _FogEnabled;
}

void CDriverVulkan::enableFog(bool enable)
{
	_FogEnabled = enable;
}

void CDriverVulkan::setupFog(float start, float end, CRGBA color)
{
	_FogStart = start;
	_FogEnd = end;
	_FogColor = color;
	_MegaCameraDirty = true;
}

void CDriverVulkan::setupFogMode(IDriver::TFogMode mode, float density)
{
	_FogMode = mode;
	_FogDensity = density;
	_MegaCameraDirty = true;
}

float CDriverVulkan::getFogStart() const { return _FogStart; }
float CDriverVulkan::getFogEnd() const { return _FogEnd; }
CRGBA CDriverVulkan::getFogColor() const { return _FogColor; }
IDriver::TFogMode CDriverVulkan::getFogMode() const { return _FogMode; }
float CDriverVulkan::getFogDensity() const { return _FogDensity; }

} /* namespace NLVULKAN */
