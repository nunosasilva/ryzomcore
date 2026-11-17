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
 * Mega shader GPU resources for the Vulkan driver.
 *
 * Creates the descriptor set layout and pipeline layout that match the
 * bindings injected into the generated GLSL, and stages the same UBO data
 * that the GL3 driver hands to its mega shaders:
 *   binding 0 NlCamera       (per camera: view, fog, clip planes)
 *   binding 1 NlLightTable   (shared light table, once per frame)
 *   binding 2 NlModel        (per draw: matrices, light indices, controls)
 *   binding 3 NlMaterial     (per material: colours, texenv, tex matrices)
 *   binding 4-6              user VP/PP/skeleton UBOs (unused for now)
 *   binding 8-15             sampler0-7
 */

#include "stdvulkan.h"
#include "driver_vulkan.h"

#include "nel/3d/light.h"
#include "nel/misc/file.h"
#include "nel/misc/string_conversion.h"
#include "nel/3d/texture_bump.h"

#include <algorithm>
#include <set>

using namespace std;
using namespace NLMISC;
using namespace NL3D;

namespace NLVULKAN {

static void convColor(CRGBA col, float out[4])
{
	out[0] = col.R / 255.f;
	out[1] = col.G / 255.f;
	out[2] = col.B / 255.f;
	out[3] = col.A / 255.f;
}

/// Same mapping as the GL3 driver: only the shaders the driver implements are
/// passed through, everything else renders as a plain textured material.
static CMaterial::TShader getSupportedShader(CMaterial::TShader shader)
{
	switch (shader)
	{
	case CMaterial::Normal:
	case CMaterial::UserColor:
	case CMaterial::LightMap:
	case CMaterial::Specular:
		return shader;
	default:
		return CMaterial::Normal;
	}
}

bool CDriverVulkan::canUseMegaPath(bool &cube)
{
	cube = false;
	if (!_MegaResourcesReady || !_CurrentMaterial)
		return false;
	// NLVK_MEGA=1 (debug A/B): skip the material-class filter. The shader
	// generator maps the classes it does not implement to a plain textured
	// material, like the GL3 driver's getSupportedShader().
	if (!_MegaForceAll)
	{
		switch (_CurrentMaterial->getShader())
		{
		case CMaterial::Normal:
		case CMaterial::UserColor:
		case CMaterial::Specular:
			break;
		// LightMap materials are drawn by the multi-pass staging implemented
		// in beginMaterialMultiPass()/setupMaterialPass().
		case CMaterial::LightMap:
			break;
		// Water uses the engine's water vertex program and the driver's water
		// pixel program (see getWaterPixelShader()).
		case CMaterial::Water:
			break;
		default:
			return false;
		}
	}

	// The mega pixel program declares sampler1 as samplerCube for the Specular
	// reflection stage and every other sampler as sampler2D. A cube texture on
	// another stage has no pixel-program variant, so it still falls back to the
	// simple path (which binds it as a 2D fallback, see commitPushConstants).
	for (uint stage = 0; stage < IDRV_MAT_MAXTEXTURES; ++stage)
	{
		ITexture *tex = _CurrentMaterial->getTexture(stage);
		if (tex && tex->isTextureCube())
		{
			// A non-square cube was uploaded from its first face as a plain 2D
			// texture (see uploadTextureCubeVulkan), so it is not a cube image.
			if (stage != 1 || !_StageTextures[1] || !_StageTextures[1]->Cube)
				return false;
			cube = true;
		}
	}
	return true;
}

void CDriverVulkan::fillMegaPipelineKey(CPipelineKey &key)
{
	// Pipelines are render-pass specific: the swapchain pass, the offscreen
	// pass and the float shadow-mask pass have different colour formats, so the
	// target selector is part of the key. Every draw path finishes its key
	// here, hence the assignment here.
	key.Target = _RenderTargetInfo ? (_RenderTargetInfo->Format == _ShadowMaskFormat &&
		_OffscreenShadowRenderPass != VK_NULL_HANDLE ? 2 : 1) : 0;

	bool cube = false;
	const bool materialOk = canUseMegaPath(cube);
	// The mega path is the default for every draw whose material class and
	// texture bindings it supports (see canUseMegaPath()). It is also required
	// for the draws of an active user vertex program and for a user pixel
	// program (they need the mega descriptor layout for their UBO and
	// samplers). NLVK_MEGA=0 keeps those required draws on mega but returns
	// everything else to the simple shader.
	// Cube textures and Specular materials also require it: the simple shader has
	// neither a samplerCube nor the Specular blend equation, and applying the
	// material's texenv to a Specular reflection mask would zero the alpha and
	// make alpha-tested materials (the Karavan tents) disappear.
	const bool userVP = hasActiveUserVPS();
	const bool userPP = _ActiveUserPP != NULL;
	const bool specular = _CurrentMaterial && _CurrentMaterial->getShader() == CMaterial::Specular;
	// Water draws replace the mega pixel stage with the water pixel program. It
	// needs the water vertex program (the engine activates it together with the
	// material), otherwise the texture coordinates it reads do not exist.
	const bool water = userVP && _CurrentMaterial && _CurrentMaterial->getShader() == CMaterial::Water;
	if (materialOk && (_MegaEnabled || userVP || userPP || cube || specular))
	{
		key.Mega = 1;
		key.MegaCube = cube ? 1 : 0;
		key.UserVP = userVP ? _ActiveUserVP->Id : 0;
		key.UserVPInsert = userVP && _ActiveUserVP->IsInsertProgram ? 1 : 0;
		key.MegaSpecular = _VPSpecularOutput ? 1 : 0;
		key.UserPP = userPP ? _ActiveUserPP->Id : 0;
		key.WaterPP = water ? 1 : 0;
		key.WaterCalcRefl = water && _CurrentMaterial->isWaterCalcReflectivity() ? 1 : 0;
		key.WaterDiffuse = water && _CurrentMaterial->getTexture(3) != NULL ? 1 : 0;
		key.WaterPlanar = water && _CurrentMaterial->isWaterPlanarReflection() ? 1 : 0;
		_WaterPPVariant = key.WaterPP
			? (1u | (key.WaterCalcRefl << 1) | (key.WaterDiffuse << 2) | (key.WaterPlanar << 3))
			: 0u;
	}
	else
	{
		key.Mega = 0;
		key.MegaCube = 0;
		key.UserVP = 0;
		key.UserVPInsert = 0;
		key.MegaSpecular = 0;
		key.UserPP = 0;
		key.WaterPP = 0;
		key.WaterCalcRefl = 0;
		key.WaterDiffuse = 0;
		key.WaterPlanar = 0;
		key.SimpleCube0 = (_StageTextures[0] && _StageTextures[0]->Cube) ? 1 : 0;
		_WaterPPVariant = 0;
	}
}

bool CDriverVulkan::initMega()
{
	// Descriptor set layout. Binding 7 stays unused so the sampler block starts
	// at the same index as the GL3 sampler numbering.
	VkDescriptorSetLayoutBinding bindings[20];
	memset(bindings, 0, sizeof(bindings));
	uint count = 0;
	for (uint i = 0; i < 4; ++i)
	{
		bindings[count].binding = i;
		bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		bindings[count].descriptorCount = 1;
		bindings[count].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		++count;
	}
	for (uint i = NL_VK_MEGA_BINDING_USER_VERTEX_PROGRAM; i <= NL_VK_MEGA_BINDING_USER_SKELETON; ++i)
	{
		bindings[count].binding = i;
		bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		bindings[count].descriptorCount = 1;
		bindings[count].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		++count;
	}
	for (uint i = 0; i < NL_VK_MEGA_SAMPLER_COUNT; ++i)
	{
		bindings[count].binding = NL_VK_MEGA_BINDING_SAMPLER_BASE + i;
		bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[count].descriptorCount = 1;
		bindings[count].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		++count;
	}
	// Modern sun shadow block and the two cascade masks.
	bindings[count].binding = NL_VK_MEGA_BINDING_SHADOW;
	bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[count].descriptorCount = 1;
	bindings[count].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	++count;
	for (uint i = 0; i < 2; ++i)
	{
		bindings[count].binding = NL_VK_MEGA_BINDING_SHADOW_MASK0 + i;
		bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[count].descriptorCount = 1;
		bindings[count].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		++count;
	}
	// Caster ids of the same texels (nearest): the receiver skips its own
	// object's depth with them.
	for (uint i = 0; i < 2; ++i)
	{
		bindings[count].binding = NL_VK_MEGA_BINDING_SHADOW_ID0 + i;
		bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[count].descriptorCount = 1;
		bindings[count].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		++count;
	}

	VkDescriptorSetLayoutCreateInfo dslci;
	memset(&dslci, 0, sizeof(dslci));
	dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslci.bindingCount = count;
	dslci.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(_Device, &dslci, NULL, &_MegaDescriptorSetLayout) != VK_SUCCESS)
	{
		nlwarning("Vulkan: mega descriptor set layout creation failed");
		return false;
	}

	VkPipelineLayoutCreateInfo plci;
	memset(&plci, 0, sizeof(plci));
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &_MegaDescriptorSetLayout;
	if (vkCreatePipelineLayout(_Device, &plci, NULL, &_MegaPipelineLayout) != VK_SUCCESS)
	{
		nlwarning("Vulkan: mega pipeline layout creation failed");
		return false;
	}

	// Placeholder for the user UBO bindings (4-6): no engine UBOs are bound
	// yet, but the descriptors must be valid for the set to be usable.
	{
		VkBufferCreateInfo bci;
		memset(&bci, 0, sizeof(bci));
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = 256;
		bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(_Device, &bci, NULL, &_MegaUserBuffer) != VK_SUCCESS)
		{
			nlwarning("Vulkan: mega user UBO placeholder creation failed");
			return false;
		}
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(_Device, _MegaUserBuffer, &req);
		VkMemoryAllocateInfo mai;
		memset(&mai, 0, sizeof(mai));
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (vkAllocateMemory(_Device, &mai, NULL, &_MegaUserBufferMemory) != VK_SUCCESS)
		{
			nlwarning("Vulkan: mega user UBO placeholder allocation failed");
			return false;
		}
		vkBindBufferMemory(_Device, _MegaUserBuffer, _MegaUserBufferMemory, 0);
		void *mapped = NULL;
		vkMapMemory(_Device, _MegaUserBufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
		memset(mapped, 0, 256);
	}

	_MegaCameraDirty = true;
	_MegaResourcesReady = true;
	nlinfo("Vulkan: mega shader resources ready; user vertex programs enabled");
	return true;
}

void CDriverVulkan::shutdownMega()
{
	_ActiveUserVP = NULL;
	_NelvpActiveUB = NULL;
	for (uint i = 0; i < UBBindingCount; ++i)
		_BoundUserUB[i] = NULL;
	_MegaResourcesReady = false;
	// A new device gets a fresh compile attempt.
	_MegaVertexShaderFailed = false;
	if (!_Device)
	{
		_UserVPShaders.clear();
		_InsertVPShaders.clear();
		return;
	}
	// Program driver infos may outlive the device; clearing the map before the
	// device dies keeps their destructors from touching destroyed modules.
	for (std::map<uint32, VkShaderModule>::iterator it = _UserVPShaders.begin(); it != _UserVPShaders.end(); ++it)
		if (it->second)
			vkDestroyShaderModule(_Device, it->second, NULL);
	_UserVPShaders.clear();
	for (std::map<uint64, VkShaderModule>::iterator it = _InsertVPShaders.begin(); it != _InsertVPShaders.end(); ++it)
		if (it->second)
			vkDestroyShaderModule(_Device, it->second, NULL);
	_InsertVPShaders.clear();

	for (map<uint32, VkShaderModule>::iterator it = _MegaPixelShaderVariants.begin(); it != _MegaPixelShaderVariants.end(); ++it)
		if (it->second)
			vkDestroyShaderModule(_Device, it->second, NULL);
	_MegaPixelShaderVariants.clear();
	for (map<uint32, VkShaderModule>::iterator it = _WaterPixelShaderVariants.begin(); it != _WaterPixelShaderVariants.end(); ++it)
		if (it->second)
			vkDestroyShaderModule(_Device, it->second, NULL);
	_WaterPixelShaderVariants.clear();
	if (_MegaVertexShader)
	{
		vkDestroyShaderModule(_Device, _MegaVertexShader, NULL);
		_MegaVertexShader = VK_NULL_HANDLE;
	}
	if (_MegaUserBuffer)
	{
		vkDestroyBuffer(_Device, _MegaUserBuffer, NULL);
		_MegaUserBuffer = VK_NULL_HANDLE;
	}
	if (_MegaUserBufferMemory)
	{
		vkFreeMemory(_Device, _MegaUserBufferMemory, NULL);
		_MegaUserBufferMemory = VK_NULL_HANDLE;
	}
	if (_MegaPipelineLayout)
	{
		vkDestroyPipelineLayout(_Device, _MegaPipelineLayout, NULL);
		_MegaPipelineLayout = VK_NULL_HANDLE;
	}
	if (_MegaDescriptorSetLayout)
	{
		vkDestroyDescriptorSetLayout(_Device, _MegaDescriptorSetLayout, NULL);
		_MegaDescriptorSetLayout = VK_NULL_HANDLE;
	}
}

// ---------------------------------------------------------------------------
// Shader sources
// ---------------------------------------------------------------------------

/// Build a complete program from a generated body: keep its #version line on
/// top, inject the builtin UBO declarations, then append the rest. userUBOs is
/// the active vertex program's block map; insert programs declare NlMorph at
/// binding 4 and NlSkeleton at binding 6 through it.
bool assembleMegaSource(const std::string &body,
	const std::map<sint, NLMISC::CSmartPtr<CUniformBufferFormat> > *userUBOs,
	std::string &outSource)
{
	const size_t versionEnd = body.find('\n');
	std::string src = body.substr(0, versionEnd + 1);
	if (userUBOs && !userUBOs->empty())
	{
		bool ok = true;
		src += buildVertexProgramUBOHeaders(*userUBOs, ok);
		if (!ok)
			return false;
	}
	else
	{
		src += "// Builtin UBO blocks (bindings 0-3)\n";
		src += buildVulkanLightTableHeader((sint)NL_VK_MEGA_MAX_LIGHT_TABLE);
		src += VulkanCameraHeader;
		src += VulkanObjectHeader;
		src += VulkanMaterialHeader;
	}
	src += body.substr(versionEnd + 1);
	outSource = src;
	return true;
}

VkShaderModule CDriverVulkan::getMegaVertexShader()
{
	if (_MegaVertexShader != VK_NULL_HANDLE)
		return _MegaVertexShader;
	if (_MegaVertexShaderFailed)
		return VK_NULL_HANDLE;

	std::string body;
	megavkVPGenerate(body, true, NULL, _ShaderClipDistance);
	std::string source;
	if (!assembleMegaSource(body, NULL, source))
	{
		nlwarning("Vulkan: mega vertex program could not be assembled");
		_MegaEnabled = false;
		_MegaVertexShaderFailed = true;
		return VK_NULL_HANDLE;
	}
	VkShaderModule module = VK_NULL_HANDLE;
	if (!compileShader(source.c_str(), &module, true))
	{
		// Retrying the same compile on every draw would only repeat the failure.
		nlwarning("Vulkan: mega vertex program failed to compile, falling back to the simple shader");
		_MegaEnabled = false;
		_MegaVertexShaderFailed = true;
		return VK_NULL_HANDLE;
	}
	nlinfo("Vulkan: compiled mega vertex program");
	_MegaVertexShader = module;
	return module;
}

VkShaderModule CDriverVulkan::getMegaPixelShader(bool cube, bool specular)
{
	const uint32 key = (cube ? 1u : 0u) | (specular ? 2u : 0u);
	map<uint32, VkShaderModule>::iterator it = _MegaPixelShaderVariants.find(key);
	if (it != _MegaPixelShaderVariants.end())
		return it->second;

	std::string body;
	megavkPPGenerate(body, true, cube, specular);
	std::string source;
	if (!assembleMegaSource(body, NULL, source))
	{
		nlwarning("Vulkan: mega pixel program could not be assembled");
		_MegaEnabled = false;
		_MegaPixelShaderVariants[key] = VK_NULL_HANDLE;
		return VK_NULL_HANDLE;
	}
	VkShaderModule module = VK_NULL_HANDLE;
	if (!compileShader(source.c_str(), &module, false))
	{
		nlwarning("Vulkan: mega pixel program failed to compile, falling back to the simple shader");
		_MegaEnabled = false;
		_MegaPixelShaderVariants[key] = VK_NULL_HANDLE;
		return VK_NULL_HANDLE;
	}
	nlinfo("Vulkan: compiled mega pixel program (cube=%d, specular=%d)", cube ? 1 : 0, specular ? 1 : 0);
	// Development: keep the generated GLSL for inspection (NLVK_DUMP_SHADERS=1).
	if (getenv("NLVK_DUMP_SHADERS"))
	{
		std::string name = "tmp/shadervk/pixel_" + NLMISC::toString((uint)key) + ".glsl";
		COFile file;
		if (file.open(name, false, false))
		{
			file.serialBuffer((uint8 *)source.c_str(), (uint32)source.size());
			file.close();
		}
	}
	_MegaPixelShaderVariants[key] = module;
	return module;
}

VkShaderModule CDriverVulkan::getWaterPixelShader(bool calcReflectivity, bool diffuse, bool planar)
{
	const uint32 key = (calcReflectivity ? 1u : 0u) | (diffuse ? 2u : 0u) | (planar ? 4u : 0u);
	map<uint32, VkShaderModule>::iterator it = _WaterPixelShaderVariants.find(key);
	if (it != _WaterPixelShaderVariants.end())
		return it->second;

	std::string body;
	watervkPPGenerate(body, calcReflectivity, diffuse, planar);
	std::string source;
	if (!assembleMegaSource(body, NULL, source))
	{
		nlwarning("Vulkan: water pixel program could not be assembled");
		return VK_NULL_HANDLE;
	}
	VkShaderModule module = VK_NULL_HANDLE;
	if (!compileShader(source.c_str(), &module, false))
	{
		nlwarning("Vulkan: water pixel program failed to compile");
		return VK_NULL_HANDLE;
	}
	nlinfo("Vulkan: compiled water pixel program (calcReflectivity=%d, diffuse=%d, planar=%d)",
		calcReflectivity ? 1 : 0, diffuse ? 1 : 0, planar ? 1 : 0);
	_WaterPixelShaderVariants[key] = module;
	return module;
}

// ---------------------------------------------------------------------------
// UBO staging
// ---------------------------------------------------------------------------

/// Bump-map normalisation factor, with the 0.25 scale the GL3 water pass
/// applies (the bump textures store a scaled normal). Non-bump stages
/// contribute a neutral factor of 1.
static float bumpNormalizationFactor(ITexture *tex)
{
	if (!tex || !tex->isBumpMap())
		return 1.f;
	return NLMISC::safe_cast<CTextureBump *>(tex)->getNormalizationFactor();
}

const CCameraUBOData &CDriverVulkan::stageMegaCameraUBO()
{
	if (!_MegaCameraDirty)
		return _MegaCameraData;

	CCameraUBOData &data = _MegaCameraData;
	memset(&data, 0, sizeof(data));

	// The mega shaders expect the full engine view matrix. The engine may hand
	// us a PZB view with the translation in the camera position; using the full
	// matrix with a zero PZB position is equivalent and keeps both drivers on
	// the same data.
	memcpy(data.viewMatrix, _ViewMatrix.get(), 16 * sizeof(float));

	convColor(_FogColor, data.fogColor);
	data.pzbCameraPos[0] = 0.f;
	data.pzbCameraPos[1] = 0.f;
	data.pzbCameraPos[2] = 0.f;
	data.fogDensity = _FogDensity;
	data.fogParams[0] = _FogStart;
	data.fogParams[1] = _FogEnd;
	data.fogMode = (sint32)_FogMode;
	data.clipPlaneMask = clipPlaneMask();
	for (uint i = 0; i < MaxClipPlanes; ++i)
		memcpy(data.clipPlane[i], _ClipPlaneEye[i], 4 * sizeof(float));

	// Camera world position: inverse view translation (used by PPL).
	{
		CMatrix invView = _ViewMatrix;
		invView.invert();
		CVector cwp = invView.getPos();
		data.cameraWorldPos[0] = cwp.x;
		data.cameraWorldPos[1] = cwp.y;
		data.cameraWorldPos[2] = cwp.z;
	}

	// Inverse view rotation: eye-space reflection -> world-space cubemap lookup.
	{
		CMatrix specularTexMtx = _ViewMatrix;
		specularTexMtx.setPos(CVector::Null);
		specularTexMtx.invert();
		memcpy(data.specularTexMtx, specularTexMtx.get(), 16 * sizeof(float));
	}

	// inv(Projection * ChangeBasis): clip-space -> NeL eye-space, used by
	// converted user vertex programs to rebuild ecPos for fog.
	{
		CMatrix invProjBasis = _GLProjMat * _ChangeBasis;
		invProjBasis.invert();
		memcpy(data.inverseProjectionBasis, invProjBasis.get(), 16 * sizeof(float));
	}

	_MegaCameraDirty = false;
	return _MegaCameraData;
}

void CDriverVulkan::stageMegaLightTableUBO()
{
	// The table is uploaded through the per-frame uniform ring in
	// bindMegaDescriptors(): it is small enough (12 KB) and this keeps it on
	// the same host-visible buffer as the camera/object/material blocks.
	const uint count = _LightTableMode ? std::min(_LightTableSize, (uint)NL_VK_MEGA_MAX_LIGHT_TABLE) : 8;
	for (uint i = 0; i < NL_VK_MEGA_MAX_LIGHT_TABLE; ++i)
	{
		if (i < count && (_LightTableMode ? i < _LightTableSize : _Lights[i].Enabled))
			_MegaLightTableEntries[i] = _LightTableMode ? _LightTable[i].Entry : _Lights[i].Entry;
		else
		{
			memset(&_MegaLightTableEntries[i], 0, sizeof(_MegaLightTableEntries[i]));
			_MegaLightTableEntries[i].mode = -1;
		}
	}
}

void CDriverVulkan::stageMegaObjectUBO(CVertexBuffer &vb, CObjectUBOData &data)
{
	memset(&data, 0, sizeof(data));

	// Matrices
	CMatrix mvp = _GLProjMat * _ChangeBasis * _ModelViewMatrix;
	memcpy(data.modelViewProjection, mvp.get(), 16 * sizeof(float));
	memcpy(data.modelView, _ModelViewMatrix.get(), 16 * sizeof(float));

	// NormalMatrix (3x3 -> std140 mat3 = 3 columns of vec4, padded)
	const float *mv = _ModelViewMatrix.get();
	data.normalMatrix[0] = mv[0];
	data.normalMatrix[1] = mv[1];
	data.normalMatrix[2] = mv[2];
	data.normalMatrix[3] = 0.f;
	data.normalMatrix[4] = mv[4];
	data.normalMatrix[5] = mv[5];
	data.normalMatrix[6] = mv[6];
	data.normalMatrix[7] = 0.f;
	data.normalMatrix[8] = mv[8];
	data.normalMatrix[9] = mv[9];
	data.normalMatrix[10] = mv[10];
	data.normalMatrix[11] = 0.f;

	// Light indices and factors (packed into ivec4 / vec4)
	for (uint i = 0; i < 8; ++i)
	{
		sint32 lightIdx;
		float lightFactor;
		if (_LightTableMode)
		{
			lightIdx = (sint32)_LightIndices[i];
			lightFactor = (float)(_LightFactors[i] + (_LightFactors[i] >> 7)) / 256.f;
		}
		else
		{
			lightIdx = (i < 8 && _Lights[i].Enabled) ? (sint32)i : -1;
			lightFactor = 1.f;
		}
		if (i < 4)
		{
			data.lightIndices01[i] = lightIdx;
			data.lightFactors01[i] = lightFactor;
		}
		else
		{
			data.lightIndices45[i - 4] = lightIdx;
			data.lightFactors45[i - 4] = lightFactor;
		}
	}

	// Static lightmapped geometry: GL3's setupLightMapDynamicLighting() gives
	// these vertices only the engine's dynamic light (the player torch), or no
	// light at all, never the scene's lights: the lightmap already contains
	// them. Entry 0 of the dedicated table bound for these draws holds the
	// dynamic light (see bindMegaDescriptors()).
	if (isLightMapDraw())
	{
		const sint32 dynIndex = _LightMapDynamicLightEnabled ? 0 : -1;
		for (uint i = 0; i < 4; ++i)
		{
			data.lightIndices01[i] = (i == 0) ? dynIndex : -1;
			data.lightFactors01[i] = (i == 0 && dynIndex >= 0) ? 1.f : 0.f;
			data.lightIndices45[i] = -1;
			data.lightFactors45[i] = 0.f;
		}
	}

	// Self illumination (ambient + per-light ambient, modulated by the material
	// ambient, plus the material emissive)
	const CMaterial &mat = *_CurrentMaterial;
	CRGBAF selfIllumination(_AmbientColor);
	if (_LightTableMode)
		selfIllumination += CRGBAF(_ObjectAmbient);
	else
		for (uint i = 0; i < 8; ++i)
			if (_Lights[i].Enabled)
				selfIllumination += CRGBAF(_Lights[i].Ambient);
	selfIllumination *= CRGBAF(mat.getAmbient());
	if (getSupportedShader(mat.getShader()) != CMaterial::LightMap)
		selfIllumination += CRGBAF(mat.getEmissive());
	data.selfIllumination[0] = selfIllumination.R;
	data.selfIllumination[1] = selfIllumination.G;
	data.selfIllumination[2] = selfIllumination.B;
	data.selfIllumination[3] = 0.f;

	// UV routing (from the vertex buffer, with the driver stage -> UV mapping)
	const uint8 *uvRouting = vb.getUVRouting();
	for (uint i = 0; i < IDRV_MAT_MAXTEXTURES; ++i)
	{
		uint8 route = uvRouting ? uvRouting[i] : (uint8)i;
		if (_MappedUV[i] < IDRV_MAT_MAXTEXTURES)
			route = _MappedUV[i];
		data.uvRouting[i] = (sint32)route;
	}

	// Texture coordinate generation, same mode values as the GL3 driver. The
	// shadow receiver is the main user: it draws terrain triangles with no UVs
	// and projects the shadow map through the stage's user texture matrix.
	for (uint i = 0; i < IDRV_MAT_MAXTEXTURES; ++i)
	{
		data.texGenMode[i] = -1;
		const ITexture *tex = mat.getTexture(i);
		if (!tex || !mat.getTexCoordGen(i))
			continue;
		switch (mat.getTexCoordGenMode(i))
		{
		case CMaterial::TexCoordGenReflect:
			data.texGenMode[i] = tex->isTextureCube() ? 0 /* ReflectionMap */ : 1 /* SphereMap */;
			break;
		case CMaterial::TexCoordGenObjectSpace:
			data.texGenMode[i] = 2; // ObjectLinear
			break;
		case CMaterial::TexCoordGenEyeSpace:
			data.texGenMode[i] = 3; // EyeLinear
			break;
		}
	}

	// Render-target textures are stored top-down, while NeL texture coordinates
	// have V = 0 at the bottom. Tell the pixel program which stages to flip.
	// This covers every target, CTextureMem ones (shadow maps and their blur
	// textures, clouds) included: they are rendered through the same GL-like
	// viewport, so they are stored top-down too.
	data.rtFlipMask = 0;
	for (uint i = 0; i < IDRV_MAT_MAXTEXTURES; ++i)
	{
		const ITexture *tex = mat.getTexture(i);
		if (tex && tex->getRenderTarget())
			data.rtFlipMask |= (1 << i);
	}

	data.lighting = mat.isLighted() ? 1 : 0;
	data.vertexColorLighted = (mat.getFlags() & IDRV_MAT_LIGHTED_VERTEX_COLOR) ? 1 : 0;
	// The pixel program samples texCoordN for a stage only when the matching
	// vertex-format bit is set. Generated coordinates have no vertex attribute,
	// and a user vertex program can write texCoordN varyings the vertex buffer
	// does not describe (bloom's blur writes four offset coordinates), so both
	// sources are added to the buffer format here. GL3 does the same for texgen.
	uint16 vertexFormat = vb.getVertexFormat();
	for (uint i = 0; i < IDRV_MAT_MAXTEXTURES; ++i)
		if (data.texGenMode[i] >= 0)
			vertexFormat |= (uint16)(CVertexBuffer::TexCoord0Flag << i);
	if (_ActiveUserVP && _ActiveUserVP->WrittenTexCoords)
		vertexFormat |= _ActiveUserVP->WrittenTexCoords;
	data.vertexFormat = (sint32)vertexFormat;
	// Per-pixel lights need world-space position and normal; the vertex program
	// emits them when these flags are set (same contract as the GL3 driver).
	data.worldSpaceNormal = _NumPerPixelLights > 0 ? 1 : 0;
	data.worldSpacePosition = _NumPerPixelLights > 0 ? 1 : 0;
	data.numPerPixelLights = (sint32)_NumPerPixelLights;
	// Fog is enabled per draw from the engine state. Both paths (the mega
	// programs and the simple generated shader) use the same camera fog data
	// and eye-space depth, so they render identical fog.
	data.fogEnabled = _FogEnabled ? 1 : 0;
	if (_MegaPassOverrideActive)
	{
		// LightMap pass override, same contract as the GL3 driver: self
		// illumination carries the pass' LMC ambient, and the dynamic light is
		// only added in the first pass.
		memcpy(data.selfIllumination, _MegaPassSelfIllumination, sizeof(data.selfIllumination));
		if (_MegaPassZeroLightFactors)
		{
			memset(data.lightFactors01, 0, sizeof(data.lightFactors01));
			memset(data.lightFactors45, 0, sizeof(data.lightFactors45));
		}
		data.lighting = 1;
		data.vertexColorLighted = 0;
	}
}

void CDriverVulkan::stageMegaMaterialUBO(CMaterial &mat, CMaterialUBOData &data)
{
	memset(&data, 0, sizeof(data));

	const CMaterial::TShader shader = getSupportedShader(mat.getShader());
	convColor(mat.getColor(), data.materialColor);
	convColor(mat.getDiffuse(), data.materialDiffuse);
	convColor(mat.getSpecular(), data.materialSpecular);
	data.materialShininess = mat.getShininess();
	data.alphaRef = mat.getAlphaTestThreshold();
	data.nlShader = (sint32)shader;
	data.nlAlphaTest = (mat.getFlags() & IDRV_MAT_ALPHA_TEST) ? 1 : 0;
	data.nlLightMapScale = 1.f;
	data.nlShadowDepthPass = _ModernShadowDepthPass ? (_ModernShadowIdPass ? 2 : 1) : 0;

	uint32 textureActive = 0;
	for (uint stage = 0; stage < IDRV_MAT_MAXTEXTURES; ++stage)
	{
		if (_StageTextures[stage])
			textureActive |= (1 << stage);
	}
	data.nlTextureActive = (sint32)textureActive;

	for (uint stage = 0; stage < IDRV_MAT_MAXTEXTURES; ++stage)
	{
		data.nlTexEnvMode[stage] = mat.getTexEnvMode(stage);
		convColor(mat._TexEnvs[stage].ConstantColor, data.constant[stage]);
	}

	// constant[4-7] hold the EMBM matrices; this driver does not stage EMBM.
	for (uint stage = IDRV_MAT_MAXTEXTURES; stage < IDRV_PROGRAM_MAXSAMPLERS; ++stage)
		memset(data.constant[stage], 0, 4 * sizeof(float));

	// Texture matrices (identity unless the material uses a user matrix)
	const uint texMatMask = (mat.getFlags() & IDRV_MAT_USER_TEX_MAT_ALL) >> IDRV_MAT_USER_TEX_FIRST_BIT;
	for (uint stage = 0; stage < IDRV_MAT_MAXTEXTURES; ++stage)
	{
		if (texMatMask & (1 << stage))
			memcpy(data.texMatrix[stage], mat.getUserTexMat(stage).get(), 16 * sizeof(float));
		else
			memcpy(data.texMatrix[stage], CMatrix::Identity.get(), 16 * sizeof(float));
	}

	if (_MegaPassOverrideActive)
	{
		// Per-pass material overrides for LightMap, mirroring GL3's
		// MaterialUBO slots: no material diffuse/specular colour, the lightmap
		// factors live in constant[0..7], and texture matrices are identity.
		data.materialDiffuse[0] = _MegaPassDiffuse;
		data.materialDiffuse[1] = _MegaPassDiffuse;
		data.materialDiffuse[2] = _MegaPassDiffuse;
		data.materialDiffuse[3] = 1.f;
		memset(data.materialSpecular, 0, sizeof(data.materialSpecular));
		data.nlLightMapScale = _MegaPassLightMapScale;
		memcpy(data.constant, _MegaPassConstants, sizeof(data.constant));
		for (uint stage = 0; stage < IDRV_MAT_MAXTEXTURES; ++stage)
			memcpy(data.texMatrix[stage], CMatrix::Identity.get(), 16 * sizeof(float));
	}
}

uint32 CDriverVulkan::allocUniformBlock(const void *data, uint32 size)
{
	uint32 align = _DeviceProps.limits.minUniformBufferOffsetAlignment;
	if (align < 16)
		align = 16;
	uint32 offset = (_UniformRingOffset + align - 1) & ~(align - 1);
	if ((uint64)offset + size > (uint64)_UniformRingFrameBase + _UniformRingHalf)
	{
		// Wrapping would overwrite blocks already referenced by the command
		// buffer being recorded. Grow instead; the old buffer stays alive until
		// the next frame boundary (see growUniformRing()).
		if (!growUniformRing(size))
			return 0xFFFFFFFF;
		offset = (_UniformRingOffset + align - 1) & ~(align - 1);
	}
	memcpy((uint8 *)_UniformRingMapped + offset, data, size);
	_UniformRingOffset = offset + size;
	return offset;
}

/// Reserve room in this frame's uniform ring half for `bytes` more, spread over
/// `blocks` allocations, growing the ring once if it does not fit.
/// allocUniformBlock() grows on demand, but a growth between two of the mega
/// path's per-draw blocks would put the first in the old buffer and the rest in
/// the new one: the four blocks are bound as one group with their own offsets,
/// so they cannot straddle two rings (the draw would read a zero
/// camera/light table/object/material). Reserving the whole group first makes
/// that impossible, and grows at most once per group.
bool CDriverVulkan::reserveUniformBlock(uint32 bytes, uint32 blocks)
{
	// allocUniformBlock() aligns every block start, so the span the group can
	// occupy is `bytes` plus one alignment gap per block.
	uint32 align = _DeviceProps.limits.minUniformBufferOffsetAlignment;
	if (align < 16)
		align = 16;
	const uint64 span = (uint64)bytes + (uint64)blocks * align;
	const uint64 required = (uint64)_UniformRingOffset + span;
	if (required <= (uint64)_UniformRingFrameBase + _UniformRingHalf)
		return true;
	// The new ring's half is at least twice `required` (see nextRingSize()).
	return growUniformRing((uint32)span);
}

bool CDriverVulkan::growUniformRing(uint32 required)
{
	// Capped and 64-bit, like the vertex/index rings: a 2 GiB ring used to
	// overflow the doubling loop and hang the client (see nextRingSize()).
	const uint32 newSize = nextRingSize(_UniformRingSize, 4u * 1024u * 1024u, required, NL_VK_UNIFORM_RING_MAX);
	if (newSize == 0)
	{
		nlwarning("Vulkan: uniform ring would exceed %u MiB for a %u-byte upload (ring %u MiB)",
			NL_VK_UNIFORM_RING_MAX / (1024u * 1024u), required, _UniformRingSize / (1024u * 1024u));
		return false;
	}

	VkBufferCreateInfo bci;
	memset(&bci, 0, sizeof(bci));
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = newSize;
	bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer buffer = VK_NULL_HANDLE;
	if (vkCreateBuffer(_Device, &bci, NULL, &buffer) != VK_SUCCESS)
		return false;
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
		return false;
	}
	vkBindBufferMemory(_Device, buffer, memory, 0);
	void *mapped = NULL;
	if (vkMapMemory(_Device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
	{
		vkDestroyBuffer(_Device, buffer, NULL);
		vkFreeMemory(_Device, memory, NULL);
		return false;
	}

	if (_UniformRing)
		deferDestroyBuffer(_UniformRing, _UniformRingMemory);
	_UniformRing = buffer;
	_UniformRingMemory = memory;
	_UniformRingMapped = mapped;
	_UniformRingSize = newSize;
	_UniformRingHalf = newSize / 2;
	_UniformRingFrameBase = _FrameIndex * _UniformRingHalf;
	_UniformRingOffset = _UniformRingFrameBase;
	// Cached user-block offsets referred to the old buffer.
	memset(_UserUBStageCache, 0, sizeof(_UserUBStageCache));
	memset(_UserUBStageSize, 0, sizeof(_UserUBStageSize));
	// The frame's light table block lives in the old buffer too.
	_MegaLightTableValid = false;
	_MegaLightMapDynTableValid = false;
	nlinfo("Vulkan: uniform ring grown to %u bytes", newSize);
	return true;
}

// ---------------------------------------------------------------------------
// Per-draw descriptor set
// ---------------------------------------------------------------------------

bool CDriverVulkan::bindMegaDescriptors(VkCommandBuffer cmd, CVertexBuffer &vb)
{
	if (!_CurrentMaterial)
		return false;

	CObjectUBOData objectData;
	CMaterialUBOData materialData;
	stageMegaObjectUBO(vb, objectData);
	stageMegaMaterialUBO(*_CurrentMaterial, materialData);
	// The light table is scene state, not per-object: stage it once per frame
	// (or after a light change) instead of copying 12 KiB for every mega draw.
	// LightMap draws with an engine dynamic light bind a dedicated table that
	// holds only that light: their vertices must not see the scene lights.
	const bool lightMapDyn = isLightMapDraw() && _LightMapDynamicLightEnabled;
	const uint32 lightTableBytes = NL_VK_MEGA_MAX_LIGHT_TABLE * sizeof(CLightTableUBOEntry);

	// TEMP: one-shot diagnosis of what the sun contributes to each draw kind.
	{
		static std::set<uint32> seen;
		const uint32 key = ((uint32)_CurrentMaterial->getShader() << 9) |
			((uint32)objectData.numPerPixelLights & 0xFF) | (lightMapDyn ? 0x100u : 0u);
		if (seen.insert(key).second)
		{
			const int firstMode = _LightTableMode ? _LightTable[0].Mode : _Lights[0].Mode;
			const CRGBA d = _LightTableMode ? _LightTable[0].Diffuse : _Lights[0].Diffuse;
			nlinfo("SHADOWDIAG shader=%d numPPL=%u lmDyn=%d tblMode=%d tblSize=%u light0=(mode=%d rgb=%u,%u,%u)",
				(int)_CurrentMaterial->getShader(), (unsigned)objectData.numPerPixelLights,
				(int)lightMapDyn, (int)_LightTableMode, (unsigned)_LightTableSize,
				firstMode, (uint)d.R, (uint)d.G, (uint)d.B);
		}
	}

	// The four per-draw blocks are bound as one group with their own offsets, so
	// they must not straddle a ring growth: reserve the whole group (light
	// table included) before any of them is allocated. A growth here also
	// clears _MegaLightTableValid, which is why the light table is staged after
	// this point - it then lands in the ring the offsets refer to.
	if (!reserveUniformBlock(lightTableBytes + sizeof(CCameraUBOData) +
		sizeof(CObjectUBOData) + sizeof(CMaterialUBOData) + sizeof(CShadowUBOData), 5))
		return false;
	const VkBuffer ring = _UniformRing;
	if (lightMapDyn)
	{
		if (!_MegaLightMapDynTableValid)
		{
			CLightTableUBOEntry entries[NL_VK_MEGA_MAX_LIGHT_TABLE];
			for (uint i = 0; i < NL_VK_MEGA_MAX_LIGHT_TABLE; ++i)
			{
				memset(&entries[i], 0, sizeof(entries[i]));
				entries[i].mode = -1;
			}
			entries[0] = _LightMapDynLightEntry;
			_MegaLightMapDynTableOffset = allocUniformBlock(entries, sizeof(entries));
			if (_MegaLightMapDynTableOffset == 0xFFFFFFFF)
				return false;
			_MegaLightMapDynTableValid = true;
		}
	}
	else if (!_MegaLightTableValid)
	{
		stageMegaLightTableUBO();
		_MegaLightTableOffset = allocUniformBlock(_MegaLightTableEntries, sizeof(_MegaLightTableEntries));
		if (_MegaLightTableOffset == 0xFFFFFFFF)
			return false;
		_MegaLightTableValid = true;
	}
	CCameraUBOData cameraData = stageMegaCameraUBO();
	if (_MegaPassFogBlack || _FogColorOverrideBlack)
	{
		// Additive passes must not add fog again: LightMap passes > 0 and any
		// additive-blended material get a black fog colour (GL3 behaviour).
		cameraData.fogColor[0] = 0.f;
		cameraData.fogColor[1] = 0.f;
		cameraData.fogColor[2] = 0.f;
	}

	CShadowUBOData shadowData;
	memset(&shadowData, 0, sizeof(shadowData));
	if (_ModernShadowState.Enabled)
	{
		shadowData.params[0] = 1.f;
		// One mask texel in UV: the penumbra taps are offset by it.
		shadowData.params[1] = _ModernShadowState.MaskTexelUv;
		shadowData.params[2] = _ModernShadowState.SunStrength;
		// The object being drawn: the id pass writes it, the receiver compares
		// it against the id mask (0 = none, never matching a live object).
		shadowData.objectId = (float)_ModernShadowObjectId;
		memcpy(shadowData.worldToMask0, _ModernShadowState.WorldToMask[0], sizeof(shadowData.worldToMask0));
		memcpy(shadowData.worldToMask1, _ModernShadowState.WorldToMask[1], sizeof(shadowData.worldToMask1));
		shadowData.splits[0] = _ModernShadowState.CascadeSplits[0];
		shadowData.depthBias[0] = _ModernShadowState.DepthBias[0];
		shadowData.depthBias[1] = _ModernShadowState.DepthBias[1];
		shadowData.depthSoft[0] = _ModernShadowState.DepthSoftness[0];
		shadowData.depthSoft[1] = _ModernShadowState.DepthSoftness[1];
	}

	const uint32 cameraOffset = allocUniformBlock(&cameraData, sizeof(cameraData));
	const uint32 lightTableOffset = lightMapDyn ? _MegaLightMapDynTableOffset : _MegaLightTableOffset;
	const uint32 objectOffset = allocUniformBlock(&objectData, sizeof(objectData));
	const uint32 materialOffset = allocUniformBlock(&materialData, sizeof(materialData));
	const uint32 shadowOffset = allocUniformBlock(&shadowData, sizeof(shadowData));
	if (cameraOffset == 0xFFFFFFFF || lightTableOffset == 0xFFFFFFFF ||
		objectOffset == 0xFFFFFFFF || materialOffset == 0xFFFFFFFF ||
		shadowOffset == 0xFFFFFFFF)
	{
		nlwarning("Vulkan: mega uniform ring exhausted");
		return false;
	}

	// reserveUniformBlock() made room for all of them, so none of these four
	// allocations could have grown the ring: they are all in `ring`. Check it
	// rather than assume it - nlassert() is compiled out (types_nl.h defines
	// NL_NO_ASSERT unconditionally) and a growth here would leave the bindings
	// pointing at the new ring with offsets from the old one, so the draw would
	// read a zero camera/light table/object/material.
	if (_UniformRing != ring)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			nlwarning("Vulkan: mega uniform ring grew inside the per-draw group; dropping the draw");
		}
		return false;
	}

	// Bindings 0-3 point into `ring`, the buffer the offsets above were
	// allocated in - not necessarily _UniformRing any more: the water and user
	// program staging below may grow the ring, and the blocks already written
	// stay in the old one (which is kept alive until the frame is retired).
	VkDescriptorBufferInfo bufferInfos[8];
	memset(bufferInfos, 0, sizeof(bufferInfos));
	bufferInfos[0].buffer = ring;
	bufferInfos[0].offset = cameraOffset;
	bufferInfos[0].range = sizeof(cameraData);
	bufferInfos[1].buffer = ring;
	bufferInfos[1].offset = lightTableOffset;
	bufferInfos[1].range = lightTableBytes;
	bufferInfos[2].buffer = ring;
	bufferInfos[2].offset = objectOffset;
	bufferInfos[2].range = sizeof(objectData);
	bufferInfos[3].buffer = ring;
	bufferInfos[3].offset = materialOffset;
	bufferInfos[3].range = sizeof(materialData);
	bufferInfos[7].buffer = ring;
	bufferInfos[7].offset = shadowOffset;
	bufferInfos[7].range = sizeof(shadowData);
	for (uint i = 4; i < 7; ++i)
	{
		bufferInfos[i].buffer = _MegaUserBuffer;
		bufferInfos[i].offset = 0;
		bufferInfos[i].range = 256;
	}
	// A water draw stages the bump-map normalisation its pixel program reads
	// at the user pixel program binding (GL3 binds the same data as NlWater).
	if (_WaterPPVariant != 0)
	{
		CWaterUBOData waterData;
		memset(&waterData, 0, sizeof(waterData));
		// GL3 scales bump 0 by 0.25 and bump 1 not at all (the first map is
		// the height field, the second the detail normal). Applying 0.25 to
		// both made the detail perturbation 4x weaker than GL3, which left
		// the coarse per-vertex envmap steps of the water screen grid visible.
		// Keep GL3's non-bump default (1) too, not 0.25 * 1.
		ITexture *bump0 = _CurrentMaterial->getTexture(0);
		ITexture *bump1 = _CurrentMaterial->getTexture(1);
		const float factor0 = (bump0 && bump0->isBumpMap())
			? 0.25f * bumpNormalizationFactor(bump0) : 1.f;
		const float factor1 = (bump1 && bump1->isBumpMap())
			? bumpNormalizationFactor(bump1) : 1.f;
		waterData.bump0ScaleBias[0] = 2.f * factor0;
		waterData.bump0ScaleBias[1] = -factor0;
		waterData.bump1ScaleBias[0] = 2.f * factor1;
		waterData.bump1ScaleBias[1] = -factor1;
		// Same mapping the water vertex program uses for o[TEX2]:
		// -normalize(toObs) * 0.5 + 0.5. Constant in the engine
		// (setupMaterialNVertexShader sets c[10] to (0.5, 0.5, 0, 1)).
		waterData.reflectParams[0] = 0.5f;
		waterData.reflectParams[1] = 0.5f;
		waterData.reflectParams[2] = 0.0f;
		waterData.reflectParams[3] = 1.0f;
		const uint32 waterOffset = allocUniformBlock(&waterData, sizeof(waterData));
		if (waterOffset == 0xFFFFFFFF)
			return false;
		bufferInfos[5].buffer = _UniformRing;
		bufferInfos[5].offset = waterOffset;
		bufferInfos[5].range = sizeof(waterData);
	}
	// The user vertex program reads its own block at binding 4: the
	// NlNelvpConstants registers for a converted nelvp program, or the UBO the
	// engine bound for a GLSL program. glsl3vi insert programs also read the
	// skeleton bones at binding 6 (bound once per skeleton before the draws).
	if (_ActiveUserVP && !stageUserUB(UBBindingVertexProgram, bufferInfos[4]))
		return false;
	// A user pixel program reads its own block at binding 5 (FXAA's NlFXAA).
	if (_ActiveUserPP && !stageUserUB(UBBindingPixelProgram, bufferInfos[5]))
		return false;
	if (_ActiveUserVP && _ActiveUserVP->IsInsertProgram && !stageUserUB(UBBindingSkeleton, bufferInfos[6]))
		return false;
	if (_ActiveUserVP && !_ActiveUserVP->FirstDrawLogged)
	{
		_ActiveUserVP->FirstDrawLogged = true;
		nlinfo("Vulkan: first draw with user vertex program '%s' (vb format 0x%x, shader %d, texture0 %p)",
			_ActiveUserVP->DisplayName.c_str(), vb.getVertexFormat(),
			(int)_CurrentMaterial->getShader(), (void *)_StageTextures[0]);
	}
	// Stage 1 is declared samplerCube by the mega pixel program when the
	// material's reflection texture is a cube map: a missing image there needs
	// a cube view, not the white 2D fallback.
	ITexture *stage1Tex = _CurrentMaterial->getTexture(1);
	const bool cubeStage1 = stage1Tex && stage1Tex->isTextureCube() &&
		_StageTextures[1] && _StageTextures[1]->Cube;
	VkDescriptorImageInfo imageInfos[NL_VK_MEGA_SAMPLER_COUNT];
	for (uint stage = 0; stage < NL_VK_MEGA_SAMPLER_COUNT; ++stage)
	{
		CTextureVulkan *tex = _StageTextures[stage];
		imageInfos[stage].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		const bool cubeStage = cubeStage1 && stage == 1;
		if (!tex && cubeStage && getWhiteCubeTexture() == VK_NULL_HANDLE)
			return false;
		imageInfos[stage].imageView = tex ? tex->View : (cubeStage ? _WhiteCubeTexture.View : getWhiteTexture());
		imageInfos[stage].sampler = tex ? tex->Sampler : (cubeStage ? _WhiteCubeTexture.Sampler : _WhiteTexture.Sampler);
	}

	VkDescriptorImageInfo shadowMaskInfos[2];
	for (uint i = 0; i < 2; ++i)
	{
		// Mask[i] is non-null while the mask pass itself runs (the id pass
		// samples the depth mask it just wrote), so bind it regardless of
		// Enabled; the shader only samples these when its pass or the shadow
		// state asks for them.
		CTextureVulkan *maskTex = NULL;
		if (_ModernShadowState.Mask[i])
			maskTex = getTextureVulkan(*_ModernShadowState.Mask[i]);
		shadowMaskInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		shadowMaskInfos[i].imageView = maskTex ? maskTex->View : getWhiteTexture();
		shadowMaskInfos[i].sampler = maskTex ? maskTex->Sampler : _WhiteTexture.Sampler;
	}
	VkDescriptorImageInfo shadowIdInfos[2];
	for (uint i = 0; i < 2; ++i)
	{
		CTextureVulkan *idTex = NULL;
		if (_ModernShadowState.IdMask[i])
			idTex = getTextureVulkan(*_ModernShadowState.IdMask[i]);
		shadowIdInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		shadowIdInfos[i].imageView = idTex ? idTex->View : getWhiteTexture();
		shadowIdInfos[i].sampler = idTex ? idTex->Sampler : _WhiteTexture.Sampler;
	}

	VkDescriptorSet descriptorSet = allocDescriptorSet(_MegaDescriptorSetLayout);
	if (descriptorSet == VK_NULL_HANDLE)
		return false;
	if (_StatsEnabled)
		++_StatsDescriptorSets;

	VkWriteDescriptorSet writes[20];
	memset(writes, 0, sizeof(writes));
	uint writeCount = 0;
	for (uint i = 0; i < 8; ++i)
	{
		writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[writeCount].dstSet = descriptorSet;
		writes[writeCount].dstBinding = i;
		writes[writeCount].descriptorCount = 1;
		writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[writeCount].pBufferInfo = &bufferInfos[i];
		++writeCount;
	}
	for (uint i = 0; i < NL_VK_MEGA_SAMPLER_COUNT; ++i)
	{
		writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[writeCount].dstSet = descriptorSet;
		writes[writeCount].dstBinding = NL_VK_MEGA_BINDING_SAMPLER_BASE + i;
		writes[writeCount].descriptorCount = 1;
		writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[writeCount].pImageInfo = &imageInfos[i];
		++writeCount;
	}
	for (uint i = 0; i < 2; ++i)
	{
		writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[writeCount].dstSet = descriptorSet;
		writes[writeCount].dstBinding = NL_VK_MEGA_BINDING_SHADOW_MASK0 + i;
		writes[writeCount].descriptorCount = 1;
		writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[writeCount].pImageInfo = &shadowMaskInfos[i];
		++writeCount;
	}
	for (uint i = 0; i < 2; ++i)
	{
		writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[writeCount].dstSet = descriptorSet;
		writes[writeCount].dstBinding = NL_VK_MEGA_BINDING_SHADOW_ID0 + i;
		writes[writeCount].descriptorCount = 1;
		writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[writeCount].pImageInfo = &shadowIdInfos[i];
		++writeCount;
	}
	if (_StatsEnabled)
		_StatsMegaDescWrites += writeCount;
	vkUpdateDescriptorSets(_Device, writeCount, writes, 0, NULL);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _MegaPipelineLayout, 0, 1, &descriptorSet, 0, NULL);

	// The mega pipelines declare VK_DYNAMIC_STATE_BLEND_CONSTANTS like the
	// simple ones, so the state must be set on both paths.
	setBlendConstants(cmd);
	return true;
}

// ---------------------------------------------------------------------------
// LightMap pass setup (ported from driver_opengl3_material.cpp)
// ---------------------------------------------------------------------------

void CDriverVulkan::computeLightMapInfos(const CMaterial &mat)
{
	static const uint32 RGBMaskPacked = CRGBA(255, 255, 255, 0).getPacked();

	nlassert(mat._LightMaps.size() <= NL_VK_MAX_LIGHTMAP);

	// Only lightmaps with a non-null RGB factor are used; build the LUT.
	_NLightMaps = 0;
	for (uint i = 0; i < mat._LightMaps.size(); ++i)
	{
		if (mat._LightMaps[i].Factor.getPacked() & RGBMaskPacked)
		{
			_LightMapLUT[_NLightMaps] = i;
			++_NLightMaps;
		}
	}

	// One stage is reserved for the material texture.
	_NLightMapPerPass = NL_VK_MEGA_SAMPLER_COUNT - 1;
	_NLightMapPass = (_NLightMaps + _NLightMapPerPass - 1) / _NLightMapPerPass;
	// NB: _NLightMaps == 0 means the material has no lightmap at all.
}

} /* namespace NLVULKAN */

/* end of file */
