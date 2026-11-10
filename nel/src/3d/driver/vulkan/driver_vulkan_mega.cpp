// NeL - MMORPG Framework <https://wiki.ryzom.dev/>
// Copyright (C) 2025  Jan BOON (Kaetemi) <jan.boon@kaetemi.be>
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
 * Megashader GLSL generators, Vulkan edition.
 *
 * Ported from the OpenGL3 driver. The split/fold design is unchanged:
 *
 *   SPLITS (separate shader variants):
 *   - fogOrPpl (on/off): Controls the ecPos/worldPos varyings and the PP fog
 *     and per-pixel lighting code. fogOrPpl=0 for UI/sky.
 *   - cube (PP only): sampler1 is a samplerCube for the Specular material.
 *   - specular (PP only): whether the PP adds the vertex specular term.
 *
 *   FOLDS (uniform-controlled branching -- zero GPU cost):
 *   - VertexFormat: All 16 in attributes declared; nlVertexFormat guards use.
 *   - Lighting on/off: uniform, skips the whole light block.
 *   - LightMode[8]: from the light table UBO (directional/point/spot).
 *   - TexGenMode[4]: from the object UBO; the Vulkan driver does not enable
 *     texgen yet, so stages stay on the attribute passthrough path.
 *   - Shader type, alpha test, texenv, fog mode, texture active mask: uniforms.
 *
 * Differences from the GL3 generator:
 *   - #version 450 instead of #version 330 + ARB_separate_shader_objects.
 *   - All varyings carry layout(location), which Vulkan requires both stages
 *     to agree on. The gl_PerVertex redeclaration is not needed and is omitted.
 *   - UBO declarations are prepended with layout(set = 0, binding = N) and
 *     samplers with their own bindings (NL_VK_MEGA_BINDING_SAMPLER_BASE).
 *   - gl_Position.z is remapped from [-w, w] to [0, w] because the pipeline
 *     uses the Vulkan clip depth range while the engine's projection matrix
 *     is the OpenGL one.
 */

#include "stdvulkan.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

#include "nel/3d/light.h"
#include "nel/3d/vertex_buffer.h"

#include "driver_vulkan_mega.h"

namespace NLVULKAN {

using namespace NL3D;

namespace /* anonymous */ {

// Vertex attribute slots, same order as the GL3 TAttribOffset enum.
enum TMegaAttrib
{
	Position,
	Weight,
	Normal,
	PrimaryColor,
	SecondaryColor,
	Fog,
	PaletteSkin,
	Tangent,
	TexCoord0,
	TexCoord1,
	TexCoord2,
	TexCoord3,
	TexCoord4,
	TexCoord5,
	TexCoord6,
	TexCoord7,
	NumMegaAttribs
};

static const uint16 s_MegaVertexFlags[NumMegaAttribs] =
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

static const char *s_MegaAttribNames[NumMegaAttribs] =
{
	"position",
	"weight",
	"normal",
	"primaryColor",
	"secondaryColor",
	"fog",
	"paletteSkin",
	"tangent",
	"texCoord0",
	"texCoord1",
	"texCoord2",
	"texCoord3",
	"texCoord4",
	"texCoord5",
	"texCoord6",
	"texCoord7"
};

// Varying locations for VP/PP interface matching. VB-sourced varyings use
// their attribute slot as the location; these slots are never VB varyings.
static const int VaryingLocationEcPos = Position; // = 0, Position is never output as a varying
static const int VaryingLocationVertexColor = Weight; // = 1, vertex color for PPL modulation
static const int VaryingLocationDiffuseColor = PrimaryColor; // = 3, PrimaryColor is always skipped
static const int VaryingLocationSpecularColor = SecondaryColor; // = 4, SecondaryColor is always skipped
static const int VaryingLocationWorldPos = Fog; // = 5, world-space position for PPL

// TexGen modes, same values as the GL3 driver (GL constants).
static const sint TexGenDisabled = -1;
static const sint TexGenReflectionMap = 0; // GL_REFLECTION_MAP_ARB
static const sint TexGenSphereMap = 1; // GL_SPHERE_MAP
static const sint TexGenObjectLinear = 2; // GL_OBJECT_LINEAR
static const sint TexGenEyeLinear = 3; // GL_EYE_LINEAR

// Packed accessors for light indices/factors in the object UBO.
static const char *s_LightIdxAccess[8] = {
	"nlLightIndices01.x", "nlLightIndices01.y", "nlLightIndices01.z", "nlLightIndices01.w",
	"nlLightIndices45.x", "nlLightIndices45.y", "nlLightIndices45.z", "nlLightIndices45.w"
};
static const char *s_LightFacAccess[8] = {
	"nlLightFactors01.x", "nlLightFactors01.y", "nlLightFactors01.z", "nlLightFactors01.w",
	"nlLightFactors45.x", "nlLightFactors45.y", "nlLightFactors45.z", "nlLightFactors45.w"
};
static const char *s_TexGenAccess[4] = {
	"nlTexGenMode.x", "nlTexGenMode.y", "nlTexGenMode.z", "nlTexGenMode.w"
};

static bool identifierIsUsed(const std::string &src, const char *name)
{
	const std::string n(name);
	size_t at = 0;
	while ((at = src.find(n, at)) != std::string::npos)
	{
		const bool leftOk = at == 0 || !(isalnum((unsigned char)src[at - 1]) || src[at - 1] == '_');
		const size_t after = at + n.size();
		const bool rightOk = after >= src.size() || !(isalnum((unsigned char)src[after]) || src[after] == '_');
		if (leftOk && rightOk)
			return true;
		at = after;
	}
	return false;
}

/** Declare exactly the varyings the generated body reads.

	Vulkan requires matching locations in both stages, so every declaration
	carries layout(location = N). The marker line left by the generator is
	replaced by the declaration list.
*/
static std::string emitUsedVaryings(const std::string &src)
{
	const char *marker = "//@@NLVKVARYINGS@@";
	const size_t mPos = src.find(marker);
	if (mPos == std::string::npos)
		return src;

	// The body is everything that is not the marker line; asking about identifiers
	// from the whole source would match the declarations themselves once inserted.
	const size_t lineEnd = src.find('\n', mPos + strlen(marker));
	const std::string body = src.substr(0, mPos) + src.substr(lineEnd + 1);

	std::string decls;
	for (int i = Weight; i < NumMegaAttribs; ++i)
	{
		if (i == PrimaryColor || i == SecondaryColor)
			continue; // never varyings: the VP may skip them
		const char *name = s_MegaAttribNames[i];
		if (!identifierIsUsed(body, name))
			continue;
		decls += "layout(location = " + NLMISC::toString(i) + ") smooth in vec4 " + name + ";\n";
	}
	// The three PPL/fog slots replace generic ones above; declare them only when read.
	struct { const char *name; int location; } specials[] = {
		{ "ecPos",       VaryingLocationEcPos },
		{ "vertexColor", VaryingLocationVertexColor },
		{ "worldPos",    VaryingLocationWorldPos }
	};
	for (uint i = 0; i < sizeof(specials) / sizeof(specials[0]); ++i)
	{
		if (!identifierIsUsed(body, specials[i].name))
			continue;
		decls += "layout(location = " + NLMISC::toString(specials[i].location) + ") smooth in vec4 " + specials[i].name + ";\n";
	}
	struct { const char *name; int location; } colors[] = {
		{ "diffuseColor",  VaryingLocationDiffuseColor },
		{ "specularColor", VaryingLocationSpecularColor }
	};
	for (uint i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i)
	{
		if (!identifierIsUsed(body, colors[i].name))
			continue;
		decls += "layout(location = " + NLMISC::toString(colors[i].location) + ") smooth in vec4 " + colors[i].name + ";\n";
	}

	std::string out = src;
	out.replace(mPos, lineEnd - mPos, decls);
	return out;
}

} /* anonymous namespace */

// ---------------------------------------------------------------------------
// UBO block declarations
// ---------------------------------------------------------------------------

std::string buildVulkanLightTableHeader(sint maxLights)
{
	// Light table UBO: shared across all objects, uploaded once when lights change.
	std::string result =
		"struct NlLightInfo {\n"
		"    vec3  dirOrPos;\n"
		"    int   mode;\n"        // 0=directional, 1=point, 2=spot
		"    vec4  diffuse;\n"
		"    vec4  specular;\n"
		"    float constAttn;\n"
		"    float linAttn;\n"
		"    float quadAttn;\n"
		"    float spotExp;\n"
		"    vec3  spotDir;\n"
		"    float spotCutoff;\n"  // cos(cutoff angle)
		"    vec4  ambient;\n"
		"};\n"
		"layout(std140, set = 0, binding = " + NLMISC::toString((int)NL_VK_MEGA_BINDING_LIGHT_TABLE) + ") uniform NlLightTable {\n"
		"    NlLightInfo nlLights[";
	result += NLMISC::toString(maxLights);
	result +=
		"];\n"
		"};\n";
	return result;
}

const char *VulkanCameraHeader =
	// Camera/global state UBO: shared across all objects, uploaded once per camera.
	"layout(std140, set = 0, binding = 0) uniform NlCamera {\n"
	"    mat4  viewMatrix;\n"
	"    vec4  fogColor;\n"
	"    vec3  pzbCameraPos;\n"
	"    float fogDensity;\n"
	"    vec2  fogParams;\n"
	"    int   nlFogMode;\n"
	"    int   nlClipPlaneMask;\n"
	"    vec4  clipPlane0;\n"
	"    vec4  clipPlane1;\n"
	"    vec4  clipPlane2;\n"
	"    vec4  clipPlane3;\n"
	"    vec4  clipPlane4;\n"
	"    vec4  clipPlane5;\n"
	"    vec3  cameraWorldPos;\n"
	"    float _nlCamPad0;\n"
	"    mat4  specularTexMtx;\n"
	"    mat4  inverseProjectionBasis;\n" // inv(Projection * ChangeBasis): clip-space -> NeL eye-space
	"};\n";

const char *VulkanObjectHeader =
	// Per-object UBO: matrices, light indices/factors, and per-draw-call state.
	"layout(std140, set = 0, binding = 2) uniform NlModel {\n"
	"    mat4  modelViewProjection;\n"
	"    mat4  modelView;\n"
	"    mat3  normalMatrix;\n"
	"    ivec4 nlLightIndices01;\n"
	"    ivec4 nlLightIndices45;\n"
	"    vec4  nlLightFactors01;\n"
	"    vec4  nlLightFactors45;\n"
	"    vec4  selfIllumination;\n"
	"    ivec4 nlUVRouting;\n"
	"    ivec4 nlTexGenMode;\n"
	"    int   nlLighting;\n"
	"    int   nlVertexColorLighted;\n"
	"    int   nlVertexFormat;\n"
	"    int   nlWorldSpaceNormal;\n"
	"    int   nlWorldSpacePosition;\n"
	"    int   nlNumPerPixelLights;\n"
	"    int   nlFogEnabled;\n"
	"    int   _nlObjPad0;\n"
	"};\n";

const char *VulkanMaterialHeader =
	// Per-material UBO: material colours, alpha test, shader type, texenv modes,
	// TexEnv constants, EMBM matrices, and texture matrices.
	"layout(std140, set = 0, binding = 3) uniform NlMaterial {\n"
	"    vec4  materialColor;\n"
	"    vec4  materialDiffuse;\n"
	"    vec4  materialSpecular;\n"
	"    float materialShininess;\n"
	"    float alphaRef;\n"
	"    int   nlShader;\n"
	"    int   nlTextureActive;\n"
	"    int   nlAlphaTest;\n"
	"    float nlLightMapScale;\n"
	"    int   _matPad0;\n"
	"    int   _matPad1;\n"
	"    uint  nlTexEnvMode0;\n"
	"    uint  nlTexEnvMode1;\n"
	"    uint  nlTexEnvMode2;\n"
	"    uint  nlTexEnvMode3;\n"
	"    vec4  constant0;\n"
	"    vec4  constant1;\n"
	"    vec4  constant2;\n"
	"    vec4  constant3;\n"
	"    vec4  constant4;\n"  // EMBM matrix stage 0 (Normal/UserColor) or lightmap factor 4 (LightMap)
	"    vec4  constant5;\n"  // EMBM matrix stage 1 or lightmap factor 5
	"    vec4  constant6;\n"  // EMBM matrix stage 2 or lightmap factor 6
	"    vec4  constant7;\n"  // EMBM matrix stage 3 or lightmap factor 7
	"    mat4  texMatrix0;\n"
	"    mat4  texMatrix1;\n"
	"    mat4  texMatrix2;\n"
	"    mat4  texMatrix3;\n"
	"};\n";

// ---------------------------------------------------------------------------
// Vertex program
// ---------------------------------------------------------------------------

void megavkVPGenerate(std::string &result, bool fogOrPpl)
{
	std::stringstream ss;
	ss << "#version 450" << std::endl;
	ss << "// Vulkan Megashader Vertex Program (fogOrPpl=" << (int)fogOrPpl << ")" << std::endl;
	ss << std::endl;

	// NlCamera, NlLightTable, NlModel and NlMaterial blocks are prepended by
	// the caller (assembleMegaSource in driver_vulkan_mega_render.cpp).

	// Megashader control constants (matching g_VertexFlags / CVertexBuffer flags)
	ss << "const int NL_VP_NORMAL_FLAG       = " << CVertexBuffer::NormalFlag << ";" << std::endl;
	ss << "const int NL_VP_PRIMARY_COLOR_FLAG = " << CVertexBuffer::PrimaryColorFlag << ";" << std::endl;
	ss << "const int NL_VP_SECONDARY_COLOR_FLAG = " << CVertexBuffer::SecondaryColorFlag << ";" << std::endl;
	for (int i = 0; i < IDRV_MAT_MAXTEXTURES; ++i)
		ss << "const int NL_VP_TEXCOORD" << i << "_FLAG = " << (int)s_MegaVertexFlags[TexCoord0 + i] << ";" << std::endl;
	ss << std::endl;

	// All 16 vertex inputs; unbound attributes read undefined values and are
	// guarded by nlVertexFormat.
	for (int i = Position; i < NumMegaAttribs; ++i)
		ss << "layout(location = " << i << ") in vec4 v" << s_MegaAttribNames[i] << ";" << std::endl;
	ss << std::endl;

	// All varyings output unconditionally. layout(location) is required in
	// Vulkan; the pixel program declares the subset it reads.
	for (int i = Weight; i < NumMegaAttribs; ++i)
	{
		if (i == PrimaryColor || i == SecondaryColor)
			continue;
		if (fogOrPpl && i == VaryingLocationVertexColor)
			continue; // Slot used by vertexColor
		if (fogOrPpl && i == VaryingLocationWorldPos)
			continue; // Slot used by worldPos
		ss << "layout(location = " << i << ") smooth out vec4 " << s_MegaAttribNames[i] << ";" << std::endl;
	}
	if (fogOrPpl)
	{
		ss << "layout(location = " << VaryingLocationEcPos << ") smooth out vec4 ecPos;" << std::endl;
		ss << "layout(location = " << VaryingLocationVertexColor << ") smooth out vec4 vertexColor;" << std::endl;
		ss << "layout(location = " << VaryingLocationWorldPos << ") smooth out vec4 worldPos;" << std::endl;
	}
	ss << "layout(location = " << VaryingLocationDiffuseColor << ") smooth out vec4 diffuseColor;" << std::endl;
	ss << "layout(location = " << VaryingLocationSpecularColor << ") smooth out vec4 specularColor;" << std::endl;
	ss << std::endl;

	// Light computation function (handles all modes via switch)
	ss << "void computeLight(int lightMode, vec3 dirOrPos, vec4 colDiff, vec4 colSpec, float shininess," << std::endl;
	ss << "                   float constAttn, float linAttn, float quadAttn," << std::endl;
	ss << "                   vec3 spotDir, float spotCutoff, float spotExp," << std::endl;
	ss << "                   vec3 normal3, vec3 ecPos3, vec3 eyeDir," << std::endl;
	ss << "                   inout vec4 lightDiffuse, inout vec4 lightSpecular)" << std::endl;
	ss << "{" << std::endl;
	ss << "  if (lightMode < 0) return;" << std::endl;
	ss << std::endl;
	ss << "  vec3 lightDir;" << std::endl;
	ss << "  float attnFactor = 1.0;" << std::endl;
	ss << std::endl;
	ss << "  if (lightMode == " << (int)CLight::DirectionalLight << ") {" << std::endl;
	ss << "    lightDir = normalize(mat3(viewMatrix) * dirOrPos);" << std::endl;
	ss << "  } else {" << std::endl;
	ss << "    // Point or spot light" << std::endl;
	ss << "    vec4 lightPos4 = viewMatrix * vec4(dirOrPos, 1.0);" << std::endl;
	ss << "    vec3 lightPos = lightPos4.xyz / lightPos4.w;" << std::endl;
	ss << "    vec3 lightVec = lightPos - ecPos3;" << std::endl;
	ss << "    float lightDistance = length(lightVec);" << std::endl;
	ss << "    lightDir = lightVec / lightDistance;" << std::endl;
	ss << "    float attenuation = constAttn + linAttn * lightDistance + quadAttn * lightDistance * lightDistance;" << std::endl;
	ss << "    attnFactor = 1.0 / attenuation;" << std::endl;
	ss << "    if (lightMode == " << (int)CLight::SpotLight << ") {" << std::endl;
	ss << "      vec3 spotDirView = normalize(mat3(viewMatrix) * spotDir);" << std::endl;
	ss << "      float spotDot = dot(-lightDir, spotDirView);" << std::endl;
	ss << "      attnFactor *= (spotDot >= spotCutoff) ? pow(spotDot, spotExp) : 0.0;" << std::endl;
	ss << "    }" << std::endl;
	ss << "  }" << std::endl;
	ss << std::endl;
	ss << "  float diffAngle = max(0.0, dot(lightDir, normal3));" << std::endl;
	ss << "  lightDiffuse += diffAngle * attnFactor * colDiff;" << std::endl;
	ss << std::endl;
	ss << "  vec3 halfVector = normalize(lightDir + eyeDir);" << std::endl;
	ss << "  float specAngle = max(0.0, dot(normal3, halfVector));" << std::endl;
	ss << "  float specPow = diffAngle > 0.0 ? pow(specAngle, shininess) : 0.0; // GL1.x LIT: no specular when surface faces away from light" << std::endl;
	ss << "  lightSpecular += specPow * attnFactor * colSpec;" << std::endl;
	ss << "}" << std::endl;
	ss << std::endl;

	// Main function
	ss << "void main(void)" << std::endl;
	ss << "{" << std::endl;
	ss << "  vec4 nlPos = v" << s_MegaAttribNames[Position] << ";" << std::endl;
	ss << "  vec3 nlNorm = v" << s_MegaAttribNames[Normal] << ".xyz;" << std::endl;
	ss << "  vec4 nlTC0 = v" << s_MegaAttribNames[TexCoord0] << ";" << std::endl;
	ss << std::endl;

	ss << "  gl_Position = modelViewProjection * nlPos;" << std::endl;
	ss << "  // NeL's projection matrix writes the OpenGL clip depth range [-w, w];" << std::endl;
	ss << "  // the Vulkan viewport expects [0, w]." << std::endl;
	ss << "  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;" << std::endl;
	ss << std::endl;

	// Eye-space position (always needed: lighting, texgen, fog)
	ss << "  vec4 ecPos4 = modelView * nlPos;" << std::endl;
	if (fogOrPpl)
	{
		ss << "  ecPos = ecPos4;" << std::endl;
		ss << "  if (nlWorldSpacePosition != 0)" << std::endl;
		ss << "    worldPos = vec4(transpose(mat3(viewMatrix)) * (ecPos4.xyz - viewMatrix[3].xyz * ecPos4.w), ecPos4.w);" << std::endl;
		ss << "  else" << std::endl;
		ss << "    worldPos = vec4(0.0);" << std::endl;
		// Default vertexColor to identity; overwritten when VertexColorLighted + PPL
		ss << "  vertexColor = vec4(1.0);" << std::endl;
	}
	ss << std::endl;

	// UV routing: build local array of VB texcoord inputs for indexed access
	ss << "  vec4 vtc[" << IDRV_MAT_MAXTEXTURES << "];" << std::endl;
	ss << "  vtc[0] = nlTC0;" << std::endl;
	for (int i = 1; i < IDRV_MAT_MAXTEXTURES; ++i)
		ss << "  vtc[" << i << "] = v" << s_MegaAttribNames[TexCoord0 + i] << ";" << std::endl;
	ss << std::endl;

	// Pass through all varyings (always normalize normals)
	for (int i = Weight; i < NumMegaAttribs; ++i)
	{
		if (i == PrimaryColor || i == SecondaryColor)
			continue;
		if (fogOrPpl && i == VaryingLocationVertexColor)
			continue; // Slot used by vertexColor
		if (fogOrPpl && i == VaryingLocationWorldPos)
			continue; // Slot used by worldPos
		if (i == Normal)
		{
			ss << "  if ((nlVertexFormat & NL_VP_NORMAL_FLAG) != 0) {" << std::endl;
			ss << "    if (nlWorldSpaceNormal != 0)" << std::endl;
			// World-space normal: eye-space via normalMatrix, then undo view rotation
			ss << "      " << s_MegaAttribNames[i] << " = vec4(transpose(mat3(viewMatrix)) * normalize(normalMatrix * nlNorm), 0.0);" << std::endl;
			ss << "    else" << std::endl;
			ss << "      " << s_MegaAttribNames[i] << " = vec4(normalize(nlNorm), 0.0);" << std::endl;
			ss << "  } else" << std::endl;
			ss << "    " << s_MegaAttribNames[i] << " = vec4(0.0, 0.0, 0.0, 0.0);" << std::endl;
		}
		else if (i == Tangent)
			ss << "  " << s_MegaAttribNames[i] << " = v" << s_MegaAttribNames[i] << ";" << std::endl;
		else if (i >= TexCoord0 && i <= TexCoord3)
			ss << "  " << s_MegaAttribNames[i] << " = texMatrix" << (i - TexCoord0) << " * vtc[nlUVRouting[" << (i - TexCoord0) << "]];" << std::endl;
		else
			ss << "  " << s_MegaAttribNames[i] << " = v" << s_MegaAttribNames[i] << ";" << std::endl;
	}
	ss << std::endl;

	// Lighting
	ss << "  vec4 diffuseVertex;" << std::endl;
	ss << "  vec4 specularVertex = vec4(0.0);" << std::endl;
	ss << "  bool doLighting = (nlLighting != 0) && ((nlVertexFormat & NL_VP_NORMAL_FLAG) != 0);" << std::endl;
	ss << std::endl;

	ss << "  if (doLighting) {" << std::endl;
	ss << "    vec3 normal3 = normalize(normalMatrix * nlNorm);" << std::endl;
	ss << "    vec3 ecPos3 = ecPos4.xyz / ecPos4.w;" << std::endl;
	ss << "    vec3 eyeDir = normalize(-ecPos3);" << std::endl;
	ss << "    diffuseVertex = vec4(0.0);" << std::endl;
	ss << "    specularVertex = vec4(0.0);" << std::endl;
	ss << "    vec4 effectiveDiffuse = (nlVertexColorLighted != 0) ? vec4(1.0) : materialDiffuse;" << std::endl;

	// Unrolled light table lookups. nlNumPerPixelLights guards the slots that
	// the pixel program consumes (PPL), the rest are vertex-lit.
	for (int i = 0; i < NL_VK_MEGA_MAX_LIGHT_TABLE && i < 8; ++i)
	{
		ss << "    if (" << i << " >= nlNumPerPixelLights) {" << std::endl;
		ss << "      int idx = " << s_LightIdxAccess[i] << ";" << std::endl;
		ss << "      if (idx >= 0) {" << std::endl;
		ss << "        NlLightInfo li = nlLights[idx];" << std::endl;
		ss << "        float factor = " << s_LightFacAccess[i] << ";" << std::endl;
		ss << "        vec3 adjDirOrPos;" << std::endl;
		ss << "        if (li.mode == " << (int)CLight::DirectionalLight << ")" << std::endl;
		ss << "          adjDirOrPos = -li.dirOrPos;" << std::endl;
		ss << "        else" << std::endl;
		ss << "          adjDirOrPos = li.dirOrPos - pzbCameraPos;" << std::endl;
		ss << "        computeLight(li.mode, adjDirOrPos," << std::endl;
		ss << "          li.diffuse * factor * effectiveDiffuse," << std::endl;
		ss << "          li.specular * factor * materialSpecular," << std::endl;
		ss << "          materialShininess," << std::endl;
		ss << "          li.constAttn, li.linAttn, li.quadAttn," << std::endl;
		ss << "          li.spotDir, li.spotCutoff, li.spotExp," << std::endl;
		ss << "          normal3, ecPos3, eyeDir, diffuseVertex, specularVertex);" << std::endl;
		ss << "      }" << std::endl;
		ss << "    }" << std::endl;
	}
	ss << "    diffuseVertex.a = 1.0;" << std::endl;
	ss << "    specularVertex.a = 1.0;" << std::endl;
	ss << std::endl;

	// Secondary color (lighted)
	ss << "    if ((nlVertexFormat & NL_VP_SECONDARY_COLOR_FLAG) != 0)" << std::endl;
	ss << "      specularVertex = specularVertex * vsecondaryColor;" << std::endl;
	ss << std::endl;

	// Add selfIllumination before vertex color multiply so it's also modulated
	// by vertex color (GL_COLOR_MATERIAL AMBIENT_AND_DIFFUSE behavior)
	ss << "    diffuseVertex.rgb = diffuseVertex.rgb + selfIllumination.rgb;" << std::endl;
	ss << std::endl;

	// Primary color x lighting interaction (GL_COLOR_MATERIAL behavior).
	if (fogOrPpl)
	{
		ss << "    if ((nlVertexFormat & NL_VP_PRIMARY_COLOR_FLAG) != 0 && nlVertexColorLighted != 0) {" << std::endl;
		ss << "      diffuseVertex = diffuseVertex * vprimaryColor;" << std::endl;
		ss << "      if (nlNumPerPixelLights > 0)" << std::endl;
		ss << "        vertexColor = vprimaryColor;" << std::endl;
		ss << "    }" << std::endl;
	}
	else
	{
		ss << "    if ((nlVertexFormat & NL_VP_PRIMARY_COLOR_FLAG) != 0 && nlVertexColorLighted != 0)" << std::endl;
		ss << "      diffuseVertex = diffuseVertex * vprimaryColor;" << std::endl;
	}
	// When lighting && !VertexColorLighted: vprimaryColor is ignored (matDiffuse pre-multiplied on CPU)

	ss << "  } else {" << std::endl;
	ss << "    // Unlit: vertex color replaces materialColor (fixed-function GL behavior)" << std::endl;
	ss << "    if ((nlVertexFormat & NL_VP_PRIMARY_COLOR_FLAG) != 0)" << std::endl;
	ss << "      diffuseVertex = vprimaryColor;" << std::endl;
	ss << "    else" << std::endl;
	ss << "      diffuseVertex = materialColor;" << std::endl;
	ss << "    if ((nlVertexFormat & NL_VP_SECONDARY_COLOR_FLAG) != 0)" << std::endl;
	ss << "      specularVertex = vsecondaryColor;" << std::endl;
	ss << "  }" << std::endl;
	ss << std::endl;

	// Combine diffuse (clamp before texture), specular passed separately (added post-texture in PP)
	ss << "  diffuseColor = clamp(diffuseVertex, 0.0, 1.0);" << std::endl;
	ss << "  specularColor = clamp(vec4(specularVertex.rgb * specularVertex.a, 0.0), 0.0, 1.0);" << std::endl;
	ss << std::endl;

	// Compute reflection vector for texgen (shared by reflection/sphere stages)
	ss << "  vec3 refl_r;" << std::endl;
	ss << "  if ((nlVertexFormat & NL_VP_NORMAL_FLAG) != 0) {" << std::endl;
	ss << "    vec3 refl_n = normalize(normalMatrix * nlNorm);" << std::endl;
	ss << "    vec3 refl_u = normalize(ecPos4.xyz);" << std::endl;
	ss << "    refl_r = reflect(refl_u, refl_n);" << std::endl;
	ss << "  } else {" << std::endl;
	ss << "    refl_r = vec3(0.0, 0.0, -1.0);" << std::endl;
	ss << "  }" << std::endl;
	ss << std::endl;

	// TexGen (unrolled per stage, uniform-switched)
	for (int i = 0; i < IDRV_MAT_MAXTEXTURES; ++i)
	{
		const char *tgmAccess = s_TexGenAccess[i];
		ss << "  if (" << tgmAccess << " == " << TexGenObjectLinear << ")" << std::endl;
		ss << "    texCoord" << i << " = texMatrix" << i << " * nlPos;" << std::endl;
		ss << "  else if (" << tgmAccess << " == " << TexGenEyeLinear << ")" << std::endl;
		ss << "    texCoord" << i << " = texMatrix" << i << " * ecPos4;" << std::endl;
		ss << "  else if (" << tgmAccess << " == " << TexGenReflectionMap << ")" << std::endl;
		ss << "    texCoord" << i << " = specularTexMtx * vec4(refl_r, 0.0);" << std::endl;
		ss << "  else if (" << tgmAccess << " == " << TexGenSphereMap << ") {" << std::endl;
		ss << "    float refl_m = 2.0 * sqrt(refl_r.x * refl_r.x + refl_r.y * refl_r.y + (refl_r.z + 1.0) * (refl_r.z + 1.0));" << std::endl;
		ss << "    texCoord" << i << " = texMatrix" << i << " * vec4(refl_r.x / refl_m + 0.5, refl_r.y / refl_m + 0.5, 0.0, 1.0);" << std::endl;
		ss << "  }" << std::endl;
		ss << "  // else: texCoord" << i << " already set from vertex attribute passthrough" << std::endl;
		ss << std::endl;
	}

	ss << "}" << std::endl;

	result = ss.str();
}

// ---------------------------------------------------------------------------
// Pixel program
// ---------------------------------------------------------------------------

void megavkPPGenerate(std::string &result, bool fogOrPpl, bool cube, bool specular)
{
	std::stringstream ss;
	ss << "#version 450" << std::endl;
	ss << "// Vulkan Megashader Pixel Program (fogOrPpl=" << (int)fogOrPpl << ", cube=" << (int)cube
	   << ", specular=" << (int)specular << ")" << std::endl;
	ss << std::endl;

	ss << "layout(location = 0) out vec4 fragColor;" << std::endl;
	ss << std::endl;

	// Varying inputs are emitted by the post-pass at the end of this function,
	// which declares exactly the ones the generated body reads.
	ss << "//@@NLVKVARYINGS@@" << std::endl;
	ss << std::endl;

	// Samplers
	for (int i = 0; i < (int)NL_VK_MEGA_SAMPLER_COUNT; ++i)
	{
		ss << "layout(set = 0, binding = " << (NL_VK_MEGA_BINDING_SAMPLER_BASE + i) << ") uniform ";
		if (cube && i == 1)
			ss << "samplerCube sampler" << i << ";" << std::endl;
		else
			ss << "sampler2D sampler" << i << ";" << std::endl;
	}
	ss << std::endl;

	// NlCamera, NlLightTable, NlModel and NlMaterial blocks are prepended by
	// the caller (assembleMegaSource in driver_vulkan_mega_render.cpp).
	// Material state (materialColor, alphaRef, nlShader, nlTextureActive,
	// nlAlphaTest, nlLightMapScale, texenv modes, constants and texture
	// matrices) comes from NlMaterial; nlVertexFormat, nlFogEnabled and
	// nlNumPerPixelLights come from NlModel; fog and camera data from NlCamera.

	// Vertex format flag constants for texcoord availability
	for (int i = 0; i < IDRV_MAT_MAXTEXTURES; ++i)
		ss << "const int NL_PP_TEXCOORD" << i << "_FLAG = " << (int)s_MegaVertexFlags[TexCoord0 + i] << ";" << std::endl;
	ss << std::endl;

	// --- TexEnv helper functions ---
	ss << "vec4 getTexEnvSrc(int src, vec4 texel, vec4 previous, vec4 diffuse, vec4 cnst) {" << std::endl;
	ss << "  return (src == 0) ? texel : (src == 1) ? previous : (src == 2) ? diffuse : cnst;" << std::endl;
	ss << "}" << std::endl;
	ss << std::endl;

	ss << "vec3 getArgRGB(int src, int op, vec4 texel, vec4 previous, vec4 diffuse, vec4 cnst) {" << std::endl;
	ss << "  vec4 s = getTexEnvSrc(src, texel, previous, diffuse, cnst);" << std::endl;
	ss << "  return (op == 0) ? s.rgb : (op == 1) ? (vec3(1.0) - s.rgb) : (op == 2) ? s.aaa : (vec3(1.0) - s.aaa);" << std::endl;
	ss << "}" << std::endl;
	ss << std::endl;

	ss << "float getArgA(int src, int op, vec4 texel, vec4 previous, vec4 diffuse, vec4 cnst) {" << std::endl;
	ss << "  vec4 s = getTexEnvSrc(src, texel, previous, diffuse, cnst);" << std::endl;
	ss << "  return (op == 0) ? s.a : (op == 1) ? (1.0 - s.a) : (op == 2) ? s.a : (1.0 - s.a);" << std::endl;
	ss << "}" << std::endl;
	ss << std::endl;

	ss << "vec3 combRGB(int op, vec3 a0, vec3 a1, vec3 a2, vec4 texel, vec4 prev, vec4 diff, vec4 cnst) {" << std::endl;
	ss << "  if (op == 0) return a0;" << std::endl; // Replace
	ss << "  if (op == 1) return a0 * a1;" << std::endl; // Modulate
	ss << "  if (op == 2) return a0 + a1;" << std::endl; // Add
	ss << "  if (op == 3) return a0 + a1 - vec3(0.5);" << std::endl; // AddSigned
	ss << "  float s;" << std::endl;
	ss << "  if (op == 4) { s = texel.a; return a0 * s + a1 * (1.0 - s); }" << std::endl; // InterpolateTexture
	ss << "  if (op == 5) { s = prev.a;  return a0 * s + a1 * (1.0 - s); }" << std::endl; // InterpolatePrevious
	ss << "  if (op == 6) { s = diff.a;  return a0 * s + a1 * (1.0 - s); }" << std::endl; // InterpolateDiffuse
	ss << "  if (op == 7) { s = cnst.a;  return a0 * s + a1 * (1.0 - s); }" << std::endl; // InterpolateConstant
	ss << "  if (op == 9) return a0 * a1 + a2;" << std::endl; // Mad
	ss << "  return a0;" << std::endl;
	ss << "}" << std::endl;
	ss << std::endl;

	ss << "float combA(int op, float a0, float a1, float a2, vec4 texel, vec4 prev, vec4 diff, vec4 cnst) {" << std::endl;
	ss << "  if (op == 0) return a0;" << std::endl;
	ss << "  if (op == 1) return a0 * a1;" << std::endl;
	ss << "  if (op == 2) return a0 + a1;" << std::endl;
	ss << "  if (op == 3) return a0 + a1 - 0.5;" << std::endl;
	ss << "  float s;" << std::endl;
	ss << "  if (op == 4) { s = texel.a; return a0 * s + a1 * (1.0 - s); }" << std::endl;
	ss << "  if (op == 5) { s = prev.a;  return a0 * s + a1 * (1.0 - s); }" << std::endl;
	ss << "  if (op == 6) { s = diff.a;  return a0 * s + a1 * (1.0 - s); }" << std::endl;
	ss << "  if (op == 7) { s = cnst.a;  return a0 * s + a1 * (1.0 - s); }" << std::endl;
	ss << "  if (op == 9) return a0 * a1 + a2;" << std::endl;
	ss << "  return a0;" << std::endl;
	ss << "}" << std::endl;
	ss << std::endl;

	// Apply one TexEnv stage (not EMBM -- caller handles EMBM separately)
	ss << "vec4 applyTexEnv(uint env, vec4 texel, vec4 prev, vec4 diff, vec4 cnst) {" << std::endl;
	ss << "  int opRGB   = int(env & 0xFu);" << std::endl;
	ss << "  int src0RGB = int((env >>  4) & 0x3u);" << std::endl;
	ss << "  int op0RGB  = int((env >>  6) & 0x3u);" << std::endl;
	ss << "  int src1RGB = int((env >>  8) & 0x3u);" << std::endl;
	ss << "  int op1RGB  = int((env >> 10) & 0x3u);" << std::endl;
	ss << "  int src2RGB = int((env >> 12) & 0x3u);" << std::endl;
	ss << "  int op2RGB  = int((env >> 14) & 0x3u);" << std::endl;
	ss << "  int opA     = int((env >> 16) & 0xFu);" << std::endl;
	ss << "  int src0A   = int((env >> 20) & 0x3u);" << std::endl;
	ss << "  int op0A    = int((env >> 22) & 0x3u);" << std::endl;
	ss << "  int src1A   = int((env >> 24) & 0x3u);" << std::endl;
	ss << "  int op1A    = int((env >> 26) & 0x3u);" << std::endl;
	ss << "  int src2A   = int((env >> 28) & 0x3u);" << std::endl;
	ss << "  int op2A    = int((env >> 30) & 0x3u);" << std::endl;
	ss << "  vec3 a0r = getArgRGB(src0RGB, op0RGB, texel, prev, diff, cnst);" << std::endl;
	ss << "  vec3 a1r = getArgRGB(src1RGB, op1RGB, texel, prev, diff, cnst);" << std::endl;
	ss << "  vec3 a2r = getArgRGB(src2RGB, op2RGB, texel, prev, diff, cnst);" << std::endl;
	ss << "  float a0a = getArgA(src0A, op0A, texel, prev, diff, cnst);" << std::endl;
	ss << "  float a1a = getArgA(src1A, op1A, texel, prev, diff, cnst);" << std::endl;
	ss << "  float a2a = getArgA(src2A, op2A, texel, prev, diff, cnst);" << std::endl;
	ss << "  return vec4(combRGB(opRGB, a0r, a1r, a2r, texel, prev, diff, cnst)," << std::endl;
	ss << "              combA(opA, a0a, a1a, a2a, texel, prev, diff, cnst));" << std::endl;
	ss << "}" << std::endl;
	ss << std::endl;

	// Fog function
	if (fogOrPpl)
	{
		ss << "vec4 applyFog(vec4 col) {" << std::endl;
		// NeL eye space is X=right, Y=INTO THE SCREEN, Z=up: the view matrix is built
		// with columns (right, viewDirection, up). So the fog depth is ecPos.y.
		ss << "  float z = abs(ecPos.y / ecPos.w);" << std::endl;
		ss << "  float fogFactor;" << std::endl;
		ss << "  if (nlFogMode == 1) fogFactor = clamp(exp(-fogDensity * z), 0.0, 1.0);" << std::endl;
		ss << "  else if (nlFogMode == 2) fogFactor = clamp(exp(-fogDensity * fogDensity * z * z), 0.0, 1.0);" << std::endl;
		ss << "  else fogFactor = clamp((fogParams.t - z) / (fogParams.t - fogParams.s), 0.0, 1.0);" << std::endl;
		ss << "  vec4 fColor = mix(fogColor, col, fogFactor);" << std::endl;
		ss << "  fColor.a = col.a;" << std::endl;
		ss << "  return fColor;" << std::endl;
		ss << "}" << std::endl;
		ss << std::endl;
	}

	// Per-pixel lighting function (world-space Blinn-Phong)
	if (fogOrPpl)
	{
		ss << "void computeLightPP(int lightMode, vec3 dirOrPos, vec4 colDiff, vec4 colSpec," << std::endl;
		ss << "                    float shininess, float constAttn, float linAttn, float quadAttn," << std::endl;
		ss << "                    vec3 spotDir, float spotCutoff, float spotExp," << std::endl;
		ss << "                    vec3 wsNormal, vec3 wsPos, vec3 eyeDir, vec3 pzbCamPos," << std::endl;
		ss << "                    inout vec4 pplDiffuse, inout vec4 pplSpecular)" << std::endl;
		ss << "{" << std::endl;
		ss << "  if (lightMode < 0) return;" << std::endl;
		ss << "  vec3 lightDir;" << std::endl;
		ss << "  float attnFactor = 1.0;" << std::endl;
		ss << "  if (lightMode == " << (int)CLight::DirectionalLight << ") {" << std::endl;
		ss << "    lightDir = normalize(-dirOrPos);" << std::endl;
		ss << "  } else {" << std::endl;
		ss << "    vec3 lightPosRel = dirOrPos - pzbCamPos;" << std::endl;
		ss << "    vec3 lightVec = lightPosRel - wsPos;" << std::endl;
		ss << "    float d = length(lightVec);" << std::endl;
		ss << "    lightDir = lightVec / d;" << std::endl;
		ss << "    attnFactor = 1.0 / (constAttn + linAttn * d + quadAttn * d * d);" << std::endl;
		ss << "    if (lightMode == " << (int)CLight::SpotLight << ") {" << std::endl;
		ss << "      float sd = dot(-lightDir, normalize(spotDir));" << std::endl;
		ss << "      attnFactor *= (sd >= spotCutoff) ? pow(sd, spotExp) : 0.0;" << std::endl;
		ss << "    }" << std::endl;
		ss << "  }" << std::endl;
		ss << "  float diff = max(0.0, dot(lightDir, wsNormal));" << std::endl;
		ss << "  pplDiffuse += diff * attnFactor * colDiff;" << std::endl;
		ss << "  vec3 h = normalize(lightDir + eyeDir);" << std::endl;
		ss << "  float spec = diff > 0.0 ? pow(max(0.0, dot(wsNormal, h)), shininess) : 0.0;" << std::endl;
		ss << "  pplSpecular += spec * attnFactor * colSpec;" << std::endl;
		ss << "}" << std::endl;
		ss << std::endl;
	}

	// --- main ---
	ss << "void main(void) {" << std::endl;

	ss << "  fragColor = diffuseColor;" << std::endl;
	ss << std::endl;

	// Per-pixel lighting accumulation
	if (fogOrPpl)
	{
		ss << "  vec4 pplSpecAccum = vec4(0.0);" << std::endl;
		// When VertexColorLighted, vertex color is the diffuse source (multiplied via vertexColor below).
		// Material diffuse must be skipped in the light equation, matching VP behavior (effectiveDiffuse = 1.0).
		ss << "  vec4 pplMatDiff = (nlVertexColorLighted != 0) ? vec4(1.0) : materialDiffuse;" << std::endl;
		ss << "  if (nlNumPerPixelLights > 0) {" << std::endl;
		ss << "    vec3 wsPos = worldPos.xyz / worldPos.w;" << std::endl;
		ss << "    vec3 wsNormal = normalize(normal.xyz);" << std::endl;
		// cameraWorldPos: precomputed on CPU as inverse view matrix position
		ss << "    vec3 eyeDir = normalize(cameraWorldPos - wsPos);" << std::endl;
		ss << "    vec4 pplDiff = vec4(0.0);" << std::endl;

		// Table PPL: light data from nlLights[idx] UBO
		for (int i = 0; i < 8; ++i)
		{
			ss << "    if (" << i << " < nlNumPerPixelLights) {" << std::endl;
			ss << "      int idx = " << s_LightIdxAccess[i] << ";" << std::endl;
			ss << "      if (idx >= 0) {" << std::endl;
			ss << "        NlLightInfo li = nlLights[idx];" << std::endl;
			ss << "        float factor = " << s_LightFacAccess[i] << ";" << std::endl;
			ss << "        computeLightPP(li.mode, li.dirOrPos," << std::endl;
			ss << "          li.diffuse * factor * pplMatDiff," << std::endl;
			ss << "          li.specular * factor * materialSpecular," << std::endl;
			ss << "          materialShininess," << std::endl;
			ss << "          li.constAttn, li.linAttn, li.quadAttn," << std::endl;
			ss << "          li.spotDir, li.spotCutoff, li.spotExp," << std::endl;
			ss << "          wsNormal, wsPos, eyeDir, pzbCameraPos," << std::endl;
			ss << "          pplDiff, pplSpecAccum);" << std::endl;
			ss << "      }" << std::endl;
			ss << "    }" << std::endl;
		}

		// vertexColor carries the vertex color when VertexColorLighted + PPL,
		// vec4(1) otherwise. VP already multiplied its lights by vertex color;
		// multiply only the PPL term here (distributes over addition).
		ss << "    fragColor.rgb = clamp(fragColor.rgb + pplDiff.rgb * vertexColor.rgb, 0.0, 1.0);" << std::endl;
		ss << "  }" << std::endl;
		ss << std::endl;
	}

	// Pre-sample all active textures
	for (int i = 0; i < (int)NL_VK_MEGA_SAMPLER_COUNT; ++i)
		ss << "  vec4 texel" << i << " = vec4(0.0);" << std::endl;
	ss << std::endl;

	// Sampling: shader-type-aware texcoord selection
	for (int i = 0; i < (int)NL_VK_MEGA_SAMPLER_COUNT; ++i)
	{
		ss << "  if ((nlTextureActive & " << (1 << i) << ") != 0) {" << std::endl;

		// UserColor: texel1 = texel0 (same texture sampled once)
		if (i == 1)
		{
			ss << "    if (nlShader == " << (int)CMaterial::UserColor << ") { texel1 = texel0; } else {" << std::endl;
		}

		// Determine texcoord
		if (i < (int)IDRV_MAT_MAXTEXTURES)
		{
			// Stages 0-3: Normal/UserColor/Specular use texCoordN, LightMap is special
			ss << "    vec2 tc;" << std::endl;
			if (i == 0)
			{
				// Stage 0: LightMap checks if more stages follow
				ss << "    if (nlShader == " << (int)CMaterial::LightMap << " && (nlTextureActive & " << (1 << (i + 1)) << ") != 0)" << std::endl;
				ss << "      tc = texCoord1.st;" << std::endl;
				ss << "    else" << std::endl;
				ss << "      tc = texCoord0.st;" << std::endl;
			}
			else
			{
				// Stage N: LightMap uses texCoord1 if more stages follow, else texCoord0
				ss << "    if (nlShader == " << (int)CMaterial::LightMap << ") {" << std::endl;
				if (i < (int)NL_VK_MEGA_SAMPLER_COUNT - 1)
					ss << "      tc = ((nlTextureActive & " << (1 << (i + 1)) << ") != 0) ? texCoord1.st : texCoord0.st;" << std::endl;
				else
					ss << "      tc = texCoord0.st;" << std::endl;
				ss << "    } else {" << std::endl;
				// Non-lightmap: use texCoordN, fallback to texCoord0
				ss << "      tc = ((nlVertexFormat & NL_PP_TEXCOORD" << std::min(i, (int)IDRV_MAT_MAXTEXTURES - 1) << "_FLAG) != 0) ? texCoord" << i << ".st : texCoord0.st;" << std::endl;
				ss << "    }" << std::endl;
			}
		}
		else
		{
			// Stages 4-7: only used by LightMap
			ss << "    vec2 tc;" << std::endl;
			if (i < (int)NL_VK_MEGA_SAMPLER_COUNT - 1)
				ss << "    tc = ((nlTextureActive & " << (1 << (i + 1)) << ") != 0) ? texCoord1.st : texCoord0.st;" << std::endl;
			else
				ss << "    tc = texCoord0.st;" << std::endl;
		}

		// Sample
		if (cube && i == 1)
			ss << "    texel" << i << " = texture(sampler" << i << ", texCoord" << i << ".stp);" << std::endl;
		else
			ss << "    texel" << i << " = texture(sampler" << i << ", tc);" << std::endl;

		if (i == 1) // Close UserColor else block
			ss << "    }" << std::endl;

		ss << "  }" << std::endl;
	}
	ss << std::endl;

	// --- Shader switch ---
	// Normal / UserColor: TexEnv interpreter
	ss << "  if (nlShader == " << (int)CMaterial::Normal << " || nlShader == " << (int)CMaterial::UserColor << ") {" << std::endl;
	ss << "    vec4 texEnvResult = fragColor;" << std::endl;
	ss << "    vec2 embmOfs = vec2(0.0);" << std::endl;
	ss << "    bool prevEMBM = false;" << std::endl;
	ss << std::endl;

	// Unrolled 4 TexEnv stages
	const char *envNames[] = { "nlTexEnvMode0", "nlTexEnvMode1", "nlTexEnvMode2", "nlTexEnvMode3" };
	const char *cnstNames[] = { "constant0", "constant1", "constant2", "constant3" };
	// EMBM matrices: folded into material UBO (constant4-7).
	// EMBM (Normal/UserColor) and lightmap factors (LightMap) are mutually exclusive by shader type
	const char *embmNames[] = { "constant4", "constant5", "constant6", "constant7" };

	for (int i = 0; i < (int)IDRV_MAT_MAXTEXTURES; ++i)
	{
		ss << "    // Stage " << i << std::endl;
		ss << "    if ((nlTextureActive & " << (1 << i) << ") != 0) {" << std::endl;

		// EMBM re-sample: if previous stage was EMBM, re-sample current texture with offset
		ss << "      vec4 t" << i << " = texel" << i << ";" << std::endl;
		ss << "      if (prevEMBM) {" << std::endl;
		ss << "        vec2 rtc = ((nlVertexFormat & NL_PP_TEXCOORD" << i << "_FLAG) != 0) ? texCoord" << i << ".st : texCoord0.st;" << std::endl;
		if (cube && i == 1)
			ss << "        t" << i << " = texture(sampler" << i << ", vec3(rtc + embmOfs, 0.0));" << std::endl;
		else
			ss << "        t" << i << " = texture(sampler" << i << ", rtc + embmOfs);" << std::endl;
		ss << "        prevEMBM = false;" << std::endl;
		ss << "      }" << std::endl;

		// Check for EMBM
		ss << "      int opRGB" << i << " = int(" << envNames[i] << " & 0xFu);" << std::endl;
		ss << "      if (opRGB" << i << " == " << (int)CMaterial::EMBM << ") {" << std::endl; // EMBM
		ss << "        embmOfs = vec2(dot(" << embmNames[i] << ".xy, t" << i << ".rg), dot(" << embmNames[i] << ".zw, t" << i << ".rg));" << std::endl;
		ss << "        prevEMBM = true;" << std::endl;
		ss << "      } else {" << std::endl;
		ss << "        texEnvResult = applyTexEnv(" << envNames[i] << ", t" << i << ", texEnvResult, fragColor, " << cnstNames[i] << ");" << std::endl;
		ss << "      }" << std::endl;

		ss << "    } else if (" << i << " == 0) {" << std::endl;
		ss << "      texEnvResult = fragColor;" << std::endl;
		ss << "    }" << std::endl;
	}
	ss << "    fragColor = texEnvResult;" << std::endl;
	ss << std::endl;

	// Specular
	ss << "  } else if (nlShader == " << (int)CMaterial::Specular << ") {" << std::endl;
	ss << "    if ((nlTextureActive & 1) != 0) {" << std::endl;
	ss << "      vec3 specop0 = texel0.rgb * fragColor.rgb;" << std::endl;
	ss << "      if ((nlTextureActive & 2) != 0)" << std::endl;
	ss << "        fragColor = vec4(texel1.rgb * texel0.a + specop0, fragColor.a);" << std::endl;
	ss << "      else" << std::endl;
	ss << "        fragColor = vec4(specop0, fragColor.a);" << std::endl;
	ss << "    } else if ((nlTextureActive & 2) != 0) {" << std::endl;
	ss << "      fragColor = vec4(texel1.rgb + fragColor.rgb, 1.0);" << std::endl;
	ss << "    }" << std::endl;
	ss << std::endl;

	// LightMap
	ss << "  } else if (nlShader == " << (int)CMaterial::LightMap << ") {" << std::endl;
	// Find last active stage and accumulate lightmaps
	ss << "    int lastStage = -1;" << std::endl;
	for (int i = (int)NL_VK_MEGA_SAMPLER_COUNT - 1; i >= 0; --i)
		ss << "    if (lastStage < 0 && (nlTextureActive & " << (1 << i) << ") != 0) lastStage = " << i << ";" << std::endl;
	ss << "    if (lastStage <= 0) {" << std::endl;
	ss << "      if (lastStage == 0) fragColor = texel0 * nlLightMapScale * fragColor;" << std::endl;
	ss << "    } else {" << std::endl;
	ss << "      vec4 lmAccum = vec4(0.0);" << std::endl;
	for (int i = 0; i < (int)NL_VK_MEGA_SAMPLER_COUNT; ++i)
	{
		ss << "      if (" << i << " < lastStage && (nlTextureActive & " << (1 << i) << ") != 0)" << std::endl;
		ss << "        lmAccum += texel" << i << " * constant" << i << ";" << std::endl;
	}
	// Last stage is diffuse texture
	for (int i = 0; i < (int)NL_VK_MEGA_SAMPLER_COUNT; ++i)
	{
		ss << "      if (lastStage == " << i << ") {" << std::endl;
		ss << "        fragColor.rgb = texel" << i << ".rgb * nlLightMapScale * (fragColor.rgb + lmAccum.rgb);" << std::endl;
		ss << "        fragColor.a = texel" << i << ".a;" << std::endl;
		ss << "      }" << std::endl;
	}
	ss << "    }" << std::endl;

	ss << "  }" << std::endl;
	ss << std::endl;

	// Add specular post-texture (matches legacy GL_COLOR_SUM / GL_SEPARATE_SPECULAR_COLOR)
	if (specular)
		ss << "  fragColor.rgb += specularColor.rgb;" << std::endl;
	if (fogOrPpl)
		ss << "  fragColor.rgb += pplSpecAccum.rgb;" << std::endl;
	ss << std::endl;

	// Alpha test
	ss << "  if (nlAlphaTest != 0 && fragColor.a <= alphaRef) discard;" << std::endl;
	ss << std::endl;

	// Fog (runtime-gated: fogOrPpl=1 always has the function, but only applies when fog is enabled)
	if (fogOrPpl)
		ss << "  if (nlFogEnabled != 0) fragColor = applyFog(fragColor);" << std::endl;

	ss << "}" << std::endl;

	result = emitUsedVaryings(ss.str());
}

} /* namespace NLVULKAN */

/* end of file */
