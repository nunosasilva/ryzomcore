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

#include <algorithm>

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
	switch (_CurrentMaterial->getShader())
	{
	case CMaterial::Normal:
	case CMaterial::UserColor:
	case CMaterial::Specular:
		break;
	// LightMap materials are drawn by the multi-pass staging implemented in
	// beginMaterialMultiPass()/setupMaterialPass().
	case CMaterial::LightMap:
		break;
	default:
		return false;
	}

	// The mega pixel program declares sampler1 as samplerCube for the Specular
	// material and every other sampler as sampler2D. Cube textures are not
	// uploaded as cube images yet (uploadTextureCube() is still a stub), so a
	// cube here would bind a 2D view to a samplerCube and trip validation;
	// keep those materials on the simple path until cube uploads exist.
	for (uint stage = 0; stage < IDRV_MAT_MAXTEXTURES; ++stage)
	{
		ITexture *tex = _CurrentMaterial->getTexture(stage);
		if (tex && tex->isTextureCube())
			return false;
	}
	return true;
}

void CDriverVulkan::fillMegaPipelineKey(CPipelineKey &key)
{
	// Pipelines are render-pass specific: the swapchain pass and the offscreen
	// pass have different colour formats, so the target selector is part of the
	// key. Every draw path finishes its key here, hence the assignment here.
	key.Target = _RenderTargetInfo ? 1 : 0;

	bool cube = false;
	const bool materialOk = canUseMegaPath(cube);
	// The mega path is used for every draw when NLVK_MEGA=1 selects it, for
	// the draws of an active user vertex program, and for a user pixel program
	// (it needs the mega descriptor layout for its UBO and samplers). The
	// three cases share the descriptor layout and pipeline.
	const bool userVP = hasActiveUserVPS();
	const bool userPP = _ActiveUserPP != NULL;
	if (materialOk && (_MegaEnabled || userVP || userPP))
	{
		key.Mega = 1;
		key.MegaCube = cube ? 1 : 0;
		key.UserVP = userVP ? _ActiveUserVP->Id : 0;
		key.UserVPInsert = userVP && _ActiveUserVP->IsInsertProgram ? 1 : 0;
		key.MegaSpecular = _VPSpecularOutput ? 1 : 0;
		key.UserPP = userPP ? _ActiveUserPP->Id : 0;
	}
	else
	{
		key.Mega = 0;
		key.MegaCube = 0;
		key.UserVP = 0;
		key.UserVPInsert = 0;
		key.MegaSpecular = 0;
		key.UserPP = 0;
	}
}

bool CDriverVulkan::initMega()
{
	// Descriptor set layout. Binding 7 stays unused so the sampler block starts
	// at the same index as the GL3 sampler numbering.
	VkDescriptorSetLayoutBinding bindings[15];
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

	// Light table UBO, one buffer per frame slot so a frame still in flight is
	// never overwritten. 128 entries is the maximum getMaxLightTableSize().
	for (uint i = 0; i < 2; ++i)
	{
		VkBufferCreateInfo bci;
		memset(&bci, 0, sizeof(bci));
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = NL_VK_MEGA_MAX_LIGHT_TABLE * sizeof(CLightTableUBOEntry);
		bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(_Device, &bci, NULL, &_MegaLightTableBuffer[i]) != VK_SUCCESS)
		{
			nlwarning("Vulkan: mega light table buffer creation failed");
			return false;
		}
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(_Device, _MegaLightTableBuffer[i], &req);
		VkMemoryAllocateInfo mai;
		memset(&mai, 0, sizeof(mai));
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (vkAllocateMemory(_Device, &mai, NULL, &_MegaLightTableMemory[i]) != VK_SUCCESS)
		{
			nlwarning("Vulkan: mega light table memory allocation failed");
			return false;
		}
		vkBindBufferMemory(_Device, _MegaLightTableBuffer[i], _MegaLightTableMemory[i], 0);
		vkMapMemory(_Device, _MegaLightTableMemory[i], 0, VK_WHOLE_SIZE, 0, &_MegaLightTableMapped[i]);
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
	if (_MegaVertexShader)
	{
		vkDestroyShaderModule(_Device, _MegaVertexShader, NULL);
		_MegaVertexShader = VK_NULL_HANDLE;
	}
	for (uint i = 0; i < 2; ++i)
	{
		if (_MegaLightTableBuffer[i])
		{
			vkDestroyBuffer(_Device, _MegaLightTableBuffer[i], NULL);
			_MegaLightTableBuffer[i] = VK_NULL_HANDLE;
		}
		if (_MegaLightTableMemory[i])
		{
			vkFreeMemory(_Device, _MegaLightTableMemory[i], NULL);
			_MegaLightTableMemory[i] = VK_NULL_HANDLE;
		}
		_MegaLightTableMapped[i] = NULL;
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

	std::string body;
	megavkVPGenerate(body, true);
	std::string source;
	if (!assembleMegaSource(body, NULL, source))
	{
		nlwarning("Vulkan: mega vertex program could not be assembled");
		_MegaEnabled = false;
		return VK_NULL_HANDLE;
	}
	VkShaderModule module = VK_NULL_HANDLE;
	if (!compileShader(source.c_str(), &module, true))
	{
		// Retrying the same compile on every draw would only repeat the failure.
		nlwarning("Vulkan: mega vertex program failed to compile, falling back to the simple shader");
		_MegaEnabled = false;
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
		return VK_NULL_HANDLE;
	}
	VkShaderModule module = VK_NULL_HANDLE;
	if (!compileShader(source.c_str(), &module, false))
	{
		nlwarning("Vulkan: mega pixel program failed to compile, falling back to the simple shader");
		_MegaEnabled = false;
		return VK_NULL_HANDLE;
	}
	nlinfo("Vulkan: compiled mega pixel program (cube=%d, specular=%d)", cube ? 1 : 0, specular ? 1 : 0);
	_MegaPixelShaderVariants[key] = module;
	return module;
}

// ---------------------------------------------------------------------------
// UBO staging
// ---------------------------------------------------------------------------

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
	data.clipPlaneMask = 0;

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
	if (_MegaLightTableStaged)
		return;
	CLightTableUBOEntry *dst = (CLightTableUBOEntry *)_MegaLightTableMapped[_FrameIndex];
	const uint count = _LightTableMode ? std::min(_LightTableSize, NL_VK_MEGA_MAX_LIGHT_TABLE) : 8;
	for (uint i = 0; i < NL_VK_MEGA_MAX_LIGHT_TABLE; ++i)
	{
		if (i < count && (_LightTableMode ? i < _LightTableSize : _Lights[i].Enabled))
			dst[i] = _LightTableMode ? _LightTable[i].Entry : _Lights[i].Entry;
		else
		{
			memset(&dst[i], 0, sizeof(dst[i]));
			dst[i].mode = -1;
		}
	}
	_MegaLightTableStaged = true;
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
	data.rtFlipMask = 0;
	for (uint i = 0; i < IDRV_MAT_MAXTEXTURES; ++i)
	{
		const ITexture *tex = mat.getTexture(i);
		// Same rule as the simple path: only NeL offscreen textures are
		// flipped (the shadow maps' CTextureMem targets are handled by the
		// shadow map manager's copy pass).
		if (tex && tex->getRenderTarget() && tex->isOffscreenTexture())
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
	if (offset + size > _UniformRingSize)
	{
		// Wrapping would overwrite blocks already referenced by the command
		// buffer being recorded. Grow instead; the old buffer stays alive until
		// the next frame boundary (see growUniformRing()).
		if (!growUniformRing(size))
			return 0xFFFFFFFF;
		offset = 0;
	}
	memcpy((uint8 *)_UniformRingMapped + offset, data, size);
	_UniformRingOffset = offset + size;
	return offset;
}

bool CDriverVulkan::growUniformRing(uint32 required)
{
	uint32 newSize = _UniformRingSize ? _UniformRingSize : 1024 * 1024;
	while (newSize < required || newSize <= _UniformRingSize)
		newSize *= 2;

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
	_UniformRingOffset = 0;
	// Cached user-block offsets referred to the old buffer.
	memset(_UserUBStageCache, 0, sizeof(_UserUBStageCache));
	memset(_UserUBStageSize, 0, sizeof(_UserUBStageSize));
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
	stageMegaLightTableUBO();
	CCameraUBOData cameraData = stageMegaCameraUBO();
	if (_MegaPassFogBlack || _FogColorOverrideBlack)
	{
		// Additive passes must not add fog again: LightMap passes > 0 and any
		// additive-blended material get a black fog colour (GL3 behaviour).
		cameraData.fogColor[0] = 0.f;
		cameraData.fogColor[1] = 0.f;
		cameraData.fogColor[2] = 0.f;
	}

	const uint32 cameraOffset = allocUniformBlock(&cameraData, sizeof(cameraData));
	const uint32 objectOffset = allocUniformBlock(&objectData, sizeof(objectData));
	const uint32 materialOffset = allocUniformBlock(&materialData, sizeof(materialData));
	if (cameraOffset == 0xFFFFFFFF || objectOffset == 0xFFFFFFFF || materialOffset == 0xFFFFFFFF)
	{
		nlwarning("Vulkan: mega uniform ring exhausted");
		return false;
	}

	VkDescriptorBufferInfo bufferInfos[7];
	memset(bufferInfos, 0, sizeof(bufferInfos));
	bufferInfos[0].buffer = _UniformRing;
	bufferInfos[0].offset = cameraOffset;
	bufferInfos[0].range = sizeof(cameraData);
	bufferInfos[1].buffer = _MegaLightTableBuffer[_FrameIndex];
	bufferInfos[1].offset = 0;
	bufferInfos[1].range = NL_VK_MEGA_MAX_LIGHT_TABLE * sizeof(CLightTableUBOEntry);
	bufferInfos[2].buffer = _UniformRing;
	bufferInfos[2].offset = objectOffset;
	bufferInfos[2].range = sizeof(objectData);
	bufferInfos[3].buffer = _UniformRing;
	bufferInfos[3].offset = materialOffset;
	bufferInfos[3].range = sizeof(materialData);
	for (uint i = 4; i < 7; ++i)
	{
		bufferInfos[i].buffer = _MegaUserBuffer;
		bufferInfos[i].offset = 0;
		bufferInfos[i].range = 256;
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
	VkDescriptorImageInfo imageInfos[NL_VK_MEGA_SAMPLER_COUNT];
	for (uint stage = 0; stage < NL_VK_MEGA_SAMPLER_COUNT; ++stage)
	{
		CTextureVulkan *tex = _StageTextures[stage];
		imageInfos[stage].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfos[stage].imageView = tex ? tex->View : getWhiteTexture();
		imageInfos[stage].sampler = tex ? tex->Sampler : _WhiteTexture.Sampler;
	}

	VkDescriptorSetAllocateInfo dsai;
	memset(&dsai, 0, sizeof(dsai));
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = _DescriptorPools[_FrameIndex];
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &_MegaDescriptorSetLayout;
	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
	if (vkAllocateDescriptorSets(_Device, &dsai, &descriptorSet) != VK_SUCCESS)
		return false;

	VkWriteDescriptorSet writes[15];
	memset(writes, 0, sizeof(writes));
	uint writeCount = 0;
	for (uint i = 0; i < 7; ++i)
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
	vkUpdateDescriptorSets(_Device, writeCount, writes, 0, NULL);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _MegaPipelineLayout, 0, 1, &descriptorSet, 0, NULL);

	// The mega pipelines declare VK_DYNAMIC_STATE_BLEND_CONSTANTS like the
	// simple ones, so the state must be set on both paths.
	const float blendConstants[4] = {
		_BlendConstantColor.R / 255.f,
		_BlendConstantColor.G / 255.f,
		_BlendConstantColor.B / 255.f,
		_BlendConstantColor.A / 255.f,
	};
	vkCmdSetBlendConstants(cmd, blendConstants);
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
