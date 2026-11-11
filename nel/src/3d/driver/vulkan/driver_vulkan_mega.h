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
 * Megashader support for the Vulkan driver.
 *
 * The GLSL generators and the UBO layouts are ported from the OpenGL3 driver:
 *   driver_opengl3_vertex_mega_program.cpp
 *   driver_opengl3_pixel_mega_program.cpp
 *   driver_opengl3_uniform_buffer.cpp (block declarations)
 *   driver_opengl3.h (UBO data layouts)
 *
 * The generated source is Vulkan GLSL: #version 450, explicit
 * layout(set, binding) on every UBO and sampler, explicit layout(location)
 * on every inter-stage varying, and a final gl_Position.z remap from the GL
 * clip range [-1, 1] to the Vulkan range [0, 1].
 */

#ifndef NL_DRIVER_VULKAN_MEGA_H
#define NL_DRIVER_VULKAN_MEGA_H

#include <nel/misc/types_nl.h>
#include <nel/misc/debug.h>
#include <nel/3d/driver.h>
#include <nel/3d/program.h>

#include <string>
#include <map>

namespace NLVULKAN {

// Descriptor set 0 binding indices. These match the GL3 builtin/user binding
// scheme so the generated code can be compared side by side, with the samplers
// moved out of the UBO range (GL had no sampler bindings to share).
const uint NL_VK_MEGA_BINDING_CAMERA = 0;
const uint NL_VK_MEGA_BINDING_LIGHT_TABLE = 1;
const uint NL_VK_MEGA_BINDING_MODEL = 2;
const uint NL_VK_MEGA_BINDING_MATERIAL = 3;
const uint NL_VK_MEGA_BINDING_USER_VERTEX_PROGRAM = 4;
const uint NL_VK_MEGA_BINDING_USER_PIXEL_PROGRAM = 5;
const uint NL_VK_MEGA_BINDING_USER_SKELETON = 6;
const uint NL_VK_MEGA_BINDING_SAMPLER_BASE = 8;
const uint NL_VK_MEGA_SAMPLER_COUNT = NL3D::IDRV_PROGRAM_MAXSAMPLERS; // 8
const uint NL_VK_MEGA_MAX_LIGHT_TABLE = 128;
// Same upper bound as the GL3 driver's NL3D_DRV_MAX_LIGHTMAP.
const uint NL_VK_MAX_LIGHTMAP = 256;

// ***************************************************************************
// Camera UBO data layout (std140, 352 bytes, matches GLSL NlCamera block)
struct CCameraUBOData
{
	float viewMatrix[16];    // 64
	float fogColor[4];       // 16
	float pzbCameraPos[3];   // 12
	float fogDensity;        //  4
	float fogParams[2];      //  8
	sint32 fogMode;          //  4
	sint32 clipPlaneMask;    //  4
	float clipPlane[6][4];   // 96
	float cameraWorldPos[3]; // 12  (inverse view translation: actual camera world position)
	float _pad0;             //  4
	float specularTexMtx[16]; // 64  (inverse view rotation: eye-space reflection -> world-space cubemap lookup)
	float inverseProjectionBasis[16]; // 64  inv(Projection * ChangeBasis): clip-space -> NeL eye-space
};                           // 352
nlctassert(sizeof(CCameraUBOData) == 352);

// ***************************************************************************
// CPU-side struct matching the std140 NlLightInfo layout (96 bytes)
struct CLightTableUBOEntry
{
	float dirOrPos[3];     // 0
	sint32 mode;           // 12
	float diffuse[4];      // 16
	float specular[4];     // 32
	float constAttn;       // 48
	float linAttn;         // 52
	float quadAttn;        // 56
	float spotExp;         // 60
	float spotDir[3];      // 64
	float spotCutoff;      // 76
	float ambient[4];      // 80
};                         // 96 bytes
nlctassert(sizeof(CLightTableUBOEntry) == 96);

// ***************************************************************************
// Per-Object UBO data layout (std140, 320 bytes, matches GLSL NlModel block)
struct CObjectUBOData
{
	float modelViewProjection[16]; // 64
	float modelView[16];           // 64
	float normalMatrix[12];        // 48 (3 cols x {x,y,z,pad} for std140 mat3)
	sint32 lightIndices01[4];      // 16
	sint32 lightIndices45[4];      // 16
	float lightFactors01[4];       // 16
	float lightFactors45[4];       // 16
	float selfIllumination[4];     // 16
	sint32 uvRouting[4];           // 16 (VB texcoord routing per stage)
	sint32 texGenMode[4];          // 16
	sint32 lighting;               // 4
	sint32 vertexColorLighted;     // 4
	sint32 vertexFormat;           // 4
	sint32 worldSpaceNormal;       // 4
	sint32 worldSpacePosition;     // 4
	sint32 numPerPixelLights;      // 4
	sint32 fogEnabled;             // 4
	sint32 rtFlipMask;             // 4 (V flip for render-target stages)
};                                 // 320
nlctassert(sizeof(CObjectUBOData) == 320);

// ***************************************************************************
// Per-Material UBO data layout (std140, 480 bytes, matches GLSL NlMaterial block)
struct CMaterialUBOData
{
	float materialColor[4];        // 16 (from materialSetup)
	float materialDiffuse[4];      // 16 (from materialSetup)
	float materialSpecular[4];     // 16 (from materialSetup)
	float materialShininess;       // 4 (from materialSetup)
	float alphaRef;                // 4 (from materialSetup)
	sint32 nlShader;               // 4 (from materialSetup)
	sint32 nlTextureActive;        // 4 (from materialSetup)
	sint32 nlAlphaTest;            // 4 (from materialSetup)
	float nlLightMapScale;         // 4 (default 1.0f, only used in lightmap material UBO)
	sint32 _pad[2];                // 8
	// TexEnv block (from setupNormalPass / setupLightmapPass)
	uint32 nlTexEnvMode[4];        // 16 (4 separate uint in GLSL, not an array -- avoids std140 vec4 padding)
	// constant[0-3]: TexEnv constant colors (Normal/UserColor shaders)
	// constant[4-7]: EMBM matrices (Normal/UserColor) OR lightmap factors (LightMap shader)
	//               -- mutually exclusive by shader type, so they share the same slots
	float constant[NL3D::IDRV_PROGRAM_MAXSAMPLERS][4]; // 128
	float texMatrix[4][16];        // 256 (texture matrices, VP -- mat4 stored column-major)
};                                 // 480
nlctassert(sizeof(CMaterialUBOData) == 480);

// GLSL headers prepended to the generated mega programs.
std::string buildVulkanLightTableHeader(sint maxLights);
extern const char *VulkanCameraHeader;
extern const char *VulkanObjectHeader;
extern const char *VulkanMaterialHeader;

/// Generate the Vulkan mega vertex program.
/// The port always uses the fully UBO-backed variant (camera, light table,
/// object and material blocks). fogOrPpl adds the ecPos/worldPos varyings that
/// the fog and per-pixel lighting code in the pixel program reads.
///
/// insertSource is a glsl3vi snippet (GPU skinning / MRM geomorph) that defines
/// nlPreTransform(inout vec4, inout vec3, inout vec4, inout vec4). When set, it
/// is spliced into the program and called on the vertex inputs before the
/// model-view-projection transform, exactly like the GL3 mega generator.
void megavkVPGenerate(std::string &result, bool fogOrPpl, const char *insertSource = NULL);

/// Generate the Vulkan mega pixel program.
/// fogOrPpl adds fog and per-pixel lighting, cube makes sampler1 a samplerCube
/// (the Specular material), specular adds the vertex specular term.
void megavkPPGenerate(std::string &result, bool fogOrPpl, bool cube, bool specular);

/// Builtin UBO blocks plus the user blocks described by a vertex program's
/// UniformBufferFormats (implemented in driver_vulkan_vp.cpp). Returns false
/// when a block uses a feature the generator does not support.
std::string buildVertexProgramUBOHeaders(
	const std::map<sint, NLMISC::CSmartPtr<NL3D::CUniformBufferFormat> > &formats, bool &ok);

/// Complete shader source from a generated body: its #version line, the builtin
/// UBO blocks, the optional user VP blocks, then the rest of the body.
bool assembleMegaSource(const std::string &body,
	const std::map<sint, NLMISC::CSmartPtr<NL3D::CUniformBufferFormat> > *userUBOs,
	std::string &outSource);

} /* namespace NLVULKAN */

#endif /* NL_DRIVER_VULKAN_MEGA_H */
