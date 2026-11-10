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

#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>

using namespace std;
using namespace NLMISC;
using namespace NL3D;

namespace NLVULKAN {

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

static std::string buildVertexShader(uint32 flags)
{
	bool hasColor = (flags & CVertexBuffer::PrimaryColorFlag) != 0;
	bool hasUV = (flags & CVertexBuffer::TexCoord0Flag) != 0;

	std::string src;
	src += "#version 450\n";
	src += "layout(location = 0) in vec3 attrPosition;\n";
	if (hasColor)
		src += "layout(location = 3) in vec4 attrColor;\n";
	if (hasUV)
		src += "layout(location = 8) in vec2 attrTexCoord0;\n";
	src +=
		"layout(push_constant) uniform Push {\n"
		"    mat4 mvp;\n"
		"    vec4 color;\n"
		"    vec4 uvTransform;\n"
		"    float alphaRef;\n"
		"    int flags;\n"
		"} pc;\n"
		"layout(location = 0) out vec4 vColor;\n"
		"layout(location = 1) out vec2 vUv;\n"
		"void main() {\n"
		"    vec4 pos = pc.mvp * vec4(attrPosition, 1.0);\n"
		"    pos.z = (pos.z + pos.w) * 0.5;\n"
		"    gl_Position = pos;\n";
	if (hasColor)
		src += "    vColor = attrColor * pc.color;\n";
	else
		src += "    vColor = pc.color;\n";
	if (hasUV)
		src += "    vUv = attrTexCoord0 * pc.uvTransform.xy + pc.uvTransform.zw;\n";
	else
		src += "    vUv = pc.uvTransform.zw;\n";
	src += "}\n";
	return src;
}

static const char *s_FragmentShader =
	"#version 450\n"
	"layout(location = 0) in vec4 vColor;\n"
	"layout(location = 1) in vec2 vUv;\n"
	"layout(binding = 0) uniform sampler2D tex0;\n"
	"layout(binding = 1) uniform sampler2D tex1;\n"
	"layout(binding = 2) uniform sampler2D tex2;\n"
	"layout(binding = 3) uniform sampler2D tex3;\n"
	"layout(push_constant) uniform Push {\n"
	"    mat4 mvp;\n"
	"    vec4 color;\n"
	"    vec4 uvTransform;\n"
	"    float alphaRef;\n"
	"    int flags;\n"
	"} pc;\n"
	"layout(location = 0) out vec4 fragColor;\n"
	"void main() {\n"
	"    vec4 res = vColor;\n"
	"    if ((pc.flags & 1) != 0) res *= texture(tex0, vUv);\n"
	"    if ((pc.flags & 2) != 0) res *= texture(tex1, vUv);\n"
	"    if ((pc.flags & 4) != 0) res *= texture(tex2, vUv);\n"
	"    if ((pc.flags & 8) != 0) res *= texture(tex3, vUv);\n"
	"    if ((pc.flags & 16) != 0 && res.a < pc.alphaRef) discard;\n"
	"    fragColor = res;\n"
	"}\n";

struct SPushConstants
{
	float	MVP[16];
	float	Color[4];
	float	UVTransform[4];
	float	AlphaRef;
	sint32	Flags;
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

	uint32 shaderFlags = key.VertexFlags & (CVertexBuffer::PrimaryColorFlag | CVertexBuffer::TexCoord0Flag);
	VkShaderModule vertexShader = _VertexShaderVariants[shaderFlags];
	if (vertexShader == VK_NULL_HANDLE)
	{
		std::string source = buildVertexShader(key.VertexFlags);
		if (!compileShader(source.c_str(), &vertexShader, true))
			return VK_NULL_HANDLE;
		_VertexShaderVariants[shaderFlags] = vertexShader;
	}
	VkShaderModule fragmentShader = _FragmentShader;
	if (fragmentShader == VK_NULL_HANDLE)
	{
		if (!compileShader(s_FragmentShader, &fragmentShader, false))
			return VK_NULL_HANDLE;
		_FragmentShader = fragmentShader;
	}

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
	pci.subpass = 0;

	VkPipeline pipeline = VK_NULL_HANDLE;
	VkResult res = vkCreateGraphicsPipelines(_Device, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline);
	if (res != VK_SUCCESS)
	{
		nlwarning("Vulkan: vkCreateGraphicsPipelines failed (%d)", (int)res);
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
		if (vkCreateGraphicsPipelines(_Device, VK_NULL_HANDLE, 1, &pci, NULL, &_TestPipeline) != VK_SUCCESS)
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
	, _VertexBuffer(VK_NULL_HANDLE)
	, _Memory(VK_NULL_HANDLE)
	, _Mapped(NULL)
	, _Size(0)
{
}

CVBDrvInfosVulkan::~CVBDrvInfosVulkan()
{
	if (VertexBufferPtr)
		VertexBufferPtr->setLocation(CVertexBuffer::NotResident);
	CDriverVulkan *driver = static_cast<CDriverVulkan *>(_Driver);
	if (driver && driver->getDevice())
	{
		if (_VertexBuffer)
		{
			if (driver->isFrameOpen())
				driver->deferDestroyBuffer(_VertexBuffer, _Memory);
			else
			{
				vkDestroyBuffer(driver->getDevice(), _VertexBuffer, NULL);
				if (_Memory)
					vkFreeMemory(driver->getDevice(), _Memory, NULL);
			}
		}
	}
}

void CVBDrvInfosVulkan::ensureCapacity(uint32 size)
{
	if (size <= _Size)
		return;
	CDriverVulkan *driver = static_cast<CDriverVulkan *>(_Driver);
	VkDevice device = driver->getDevice();
	if (_VertexBuffer)
	{
		if (driver->isFrameOpen())
			driver->deferDestroyBuffer(_VertexBuffer, _Memory);
		else
		{
			vkDestroyBuffer(device, _VertexBuffer, NULL);
			if (_Memory)
				vkFreeMemory(device, _Memory, NULL);
		}
		_VertexBuffer = VK_NULL_HANDLE;
		_Memory = VK_NULL_HANDLE;
	}

	VkBufferCreateInfo bci;
	memset(&bci, 0, sizeof(bci));
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = size;
	bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(device, &bci, NULL, &_VertexBuffer) != VK_SUCCESS)
	{
		nlwarning("Vulkan: vertex buffer creation failed (%u bytes)", size);
		return;
	}
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device, _VertexBuffer, &req);
	VkMemoryAllocateInfo mai;
	memset(&mai, 0, sizeof(mai));
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = driver->findMemoryType(req.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (vkAllocateMemory(device, &mai, NULL, &_Memory) != VK_SUCCESS)
	{
		nlwarning("Vulkan: vertex buffer memory allocation failed (%u bytes)", size);
		return;
	}
	vkBindBufferMemory(device, _VertexBuffer, _Memory, 0);
	vkMapMemory(device, _Memory, 0, VK_WHOLE_SIZE, 0, &_Mapped);
	_Size = size;
}

uint8 *CVBDrvInfosVulkan::lock(uint begin, uint end, bool readOnly)
{
	return (uint8 *)_Mapped + begin;
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
// Per-frame index upload (small ring, reset each frame)
// ---------------------------------------------------------------------------

VkBuffer CDriverVulkan::getIndexUploadBuffer(const void *data, uint32 size, uint32 stride)
{
	if (_IndexRing == VK_NULL_HANDLE)
	{
		if (_IndexRing)
		{
			vkDestroyBuffer(_Device, _IndexRing, NULL);
			vkFreeMemory(_Device, _IndexRingMemory, NULL);
		}
		_IndexRingSize = 16 * 1024 * 1024u;
		VkBufferCreateInfo bci;
		memset(&bci, 0, sizeof(bci));
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = _IndexRingSize;
		bci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		vkCreateBuffer(_Device, &bci, NULL, &_IndexRing);
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(_Device, _IndexRing, &req);
		VkMemoryAllocateInfo mai;
		memset(&mai, 0, sizeof(mai));
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		vkAllocateMemory(_Device, &mai, NULL, &_IndexRingMemory);
		vkBindBufferMemory(_Device, _IndexRing, _IndexRingMemory, 0);
		vkMapMemory(_Device, _IndexRingMemory, 0, VK_WHOLE_SIZE, 0, &_IndexRingMapped);
		_IndexRingOffset = 0;
	}
	if (_IndexRingOffset + size > _IndexRingSize)
		_IndexRingOffset = 0; // simple wrap; frame is small enough that this is fine for now
	memcpy((uint8 *)_IndexRingMapped + _IndexRingOffset, data, size);
	_IndexRingDrawOffset = _IndexRingOffset;
	_IndexRingOffset += (size + stride - 1) / stride * stride;
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
	_DepthWriteEnabled = (mat.getFlags() & IDRV_MAT_ZWRITE) != 0;
	_DepthTestEnabled = true;
	_DepthFunc = toVkCompare(mat.getZFunc());
	_MaterialTwoSided = (mat.getFlags() & IDRV_MAT_DOUBLE_SIDED) != 0;
	_MaterialAlphaTest = (mat.getFlags() & IDRV_MAT_ALPHA_TEST) != 0;
	_MaterialAlphaRef = mat.getAlphaTestThreshold();
	_MaterialColor = mat.getColor();
	_MaterialShader = mat.getShader();
	_CurrentMaterial = &mat;

	for (uint stage = 0; stage < IDRV_PROGRAM_MAXSAMPLERS; ++stage)
	{
		_StageTextures[stage] = NULL;
		ITexture *texture = (stage < IDRV_MAT_MAXTEXTURES) ? mat.getTexture(stage) : NULL;
		if (texture)
		{
			bool allUploaded = false;
			if (setupTextureEx(*texture, true, allUploaded, false))
				_StageTextures[stage] = getTextureVulkan(*texture);
		}
	}
	_TexturesDirty = true;
}

bool CDriverVulkan::setupMaterial(CMaterial &mat)
{
	updateMaterialState(mat);
	return true;
}

sint CDriverVulkan::beginMaterialMultiPass()
{
	return 1;
}

void CDriverVulkan::setupMaterialPass(uint pass)
{
}

void CDriverVulkan::endMaterialMultiPass()
{
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

void CDriverVulkan::commitPushConstants(const CPipelineKey &key, VkCommandBuffer cmd, CVertexBuffer &vb)
{
	SPushConstants push;
	memset(&push, 0, sizeof(push));

	CMatrix mvp = _GLProjMat * _ChangeBasis * _ModelViewMatrix;
	mvp.get(push.MVP);

	push.Color[0] = _MaterialColor.R / 255.f;
	push.Color[1] = _MaterialColor.G / 255.f;
	push.Color[2] = _MaterialColor.B / 255.f;
	push.Color[3] = _MaterialColor.A / 255.f;
	push.UVTransform[0] = 1.f;
	push.UVTransform[1] = 1.f;
	push.AlphaRef = _MaterialAlphaRef;

	int flags = 0;
	for (uint stage = 0; stage < 4; ++stage)
		if (_StageTextures[stage])
			flags |= (1 << stage);
	if (_MaterialAlphaTest)
		flags |= 16;
	push.Flags = flags;

	vkCmdPushConstants(cmd, _PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

	// Describe the 4 stage textures (unused stages get the white texture).
	VkDescriptorImageInfo infos[4];
	for (uint stage = 0; stage < 4; ++stage)
	{
		CTextureVulkan *tex = _StageTextures[stage];
		infos[stage].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		infos[stage].imageView = tex ? tex->View : getWhiteTexture();
		infos[stage].sampler = tex ? tex->Sampler : _WhiteTexture.Sampler;
	}
	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
	VkDescriptorSetAllocateInfo dsai;
	memset(&dsai, 0, sizeof(dsai));
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = _DescriptorPools[_FrameIndex];
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &_DescriptorSetLayout;
	if (vkAllocateDescriptorSets(_Device, &dsai, &descriptorSet) != VK_SUCCESS)
		return;

	VkWriteDescriptorSet writes[4];
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
	vkUpdateDescriptorSets(_Device, 4, writes, 0, NULL);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _PipelineLayout, 0, 1, &descriptorSet, 0, NULL);
}

// ---------------------------------------------------------------------------
// Draws
// ---------------------------------------------------------------------------

bool CDriverVulkan::drawArrays(CMaterial &mat, uint32 firstVertex, uint32 count, VkPrimitiveTopology topo)
{
	if (!_CurrentVB || count == 0 || !_FrameOpen)
		return false;

	CVBDrvInfosVulkan *info = dynamic_cast<CVBDrvInfosVulkan *>((IVBDrvInfos *)_CurrentVB->DrvInfos);
	if (!info || !info->_VertexBuffer)
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
	key.CullMode = getenv("NLVK_NOCULL") ? VK_CULL_MODE_NONE : (_MaterialTwoSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
	key.FrontFace = VK_FRONT_FACE_CLOCKWISE;
	key.ColorWriteMask = _ClearColorMask ? _ClearColorMask : 0xF;
	key.AlphaTest = _MaterialAlphaTest ? 1 : 0;
	key.HasTexture0 = _StageTextures[0] ? 1 : 0;

	VkPipeline pipeline = getPipeline(key);
	if (pipeline == VK_NULL_HANDLE)
		return false;

	VkCommandBuffer cmd = _CommandBuffers[_FrameIndex];
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	commitPushConstants(key, cmd, *_CurrentVB);

	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &info->_VertexBuffer, &offset);
	vkCmdDraw(cmd, count, 1, firstVertex, 0);
	++_ArrayDrawCount;
	return true;
}

bool CDriverVulkan::drawIndexed(CMaterial &mat, uint32 firstIndex, uint32 count, VkPrimitiveTopology topo, uint32 indexStride)
{
	if (!_CurrentVB || !_IndexValues || count == 0 || !_FrameOpen)
		return false;

	CVBDrvInfosVulkan *info = dynamic_cast<CVBDrvInfosVulkan *>((IVBDrvInfos *)_CurrentVB->DrvInfos);
	if (!info || !info->_VertexBuffer)
		return false;

	uint32 bytes = count * indexStride;
	if (bytes == 0)
		return false;
	VkBuffer indexBuffer = getIndexUploadBuffer(_IndexValues + (uint64)firstIndex * indexStride, bytes, indexStride);
	if (indexBuffer == VK_NULL_HANDLE)
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
	key.CullMode = getenv("NLVK_NOCULL") ? VK_CULL_MODE_NONE : (_MaterialTwoSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
	key.FrontFace = VK_FRONT_FACE_CLOCKWISE;
	key.ColorWriteMask = _ClearColorMask ? _ClearColorMask : 0xF;
	key.AlphaTest = _MaterialAlphaTest ? 1 : 0;
	key.HasTexture0 = _StageTextures[0] ? 1 : 0;

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

	VkCommandBuffer cmd = _CommandBuffers[_FrameIndex];
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	commitPushConstants(key, cmd, *_CurrentVB);

	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &info->_VertexBuffer, &offset);
	vkCmdBindIndexBuffer(cmd, indexBuffer, _IndexRingDrawOffset,
		indexStride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(cmd, count, 1, 0, 0, 0);
	++_IndexedDrawCount;
	return true;
}

bool CDriverVulkan::renderTriangles(CMaterial &mat, uint32 firstIndex, uint32 nlines)
{
	if (!setupMaterial(mat))
		return false;
	return drawIndexed(mat, firstIndex, nlines * 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, _IndexFormat / 8);
}

bool CDriverVulkan::renderLines(CMaterial &mat, uint32 firstIndex, uint32 nlines)
{
	if (!setupMaterial(mat))
		return false;
	return drawIndexed(mat, firstIndex, nlines * 2, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, _IndexFormat / 8);
}

bool CDriverVulkan::renderSimpleTriangles(uint32 firstIndex, uint32 ntris)
{
	return drawIndexed(*_CurrentMaterial, firstIndex, ntris * 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, _IndexFormat / 8);
}

bool CDriverVulkan::renderRawTriangles(CMaterial &mat, uint32 startVertex, uint32 numTri)
{
	if (!setupMaterial(mat))
		return false;
	return drawArrays(mat, startVertex, numTri * 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
}

bool CDriverVulkan::renderRawLines(CMaterial &mat, uint32 startVertex, uint32 numTri)
{
	if (!setupMaterial(mat))
		return false;
	return drawArrays(mat, startVertex, numTri * 2, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
}

bool CDriverVulkan::renderRawPoints(CMaterial &mat, uint32 startVertex, uint32 numPoints)
{
	if (!setupMaterial(mat))
		return false;
	return drawArrays(mat, startVertex, numPoints, VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
}

bool CDriverVulkan::renderRawQuads(CMaterial &mat, uint32 startVertex, uint32 numQuads)
{
	if (numQuads == 0)
		return true;
	if (!setupMaterial(mat))
		return false;
	if (_IndexedDrawCount == 0 && _ArrayDrawCount == 0)
	{
		nlinfo("Vulkan: first quad draw: %u quads, vb=%p, stride=%u, vformat=0x%x, mat flags=0x%x, diffuse=(%u,%u,%u,%u), tex0=%p",
			numQuads, (void *)_CurrentVB, _CurrentVB ? _CurrentVB->getVertexSize() : 0,
			_CurrentVB ? _CurrentVB->getVertexFormat() : 0,
			mat.getFlags(), mat.getDiffuse().R, mat.getDiffuse().G, mat.getDiffuse().B, mat.getDiffuse().A,
			(void *)mat.getTexture(0));
		CMatrix mvp = _GLProjMat * _ChangeBasis * _ModelViewMatrix;
		const float *m = mvp.get();
		nlinfo("Vulkan: mvp = [%f %f %f %f] [%f %f %f %f]", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
		nlinfo("Vulkan: mat color=(%u,%u,%u,%u) posOff=%u posType=%d uvOff=%u uvType=%d",
			mat.getColor().R, mat.getColor().G, mat.getColor().B, mat.getColor().A,
			_CurrentVB->getValueOffEx(CVertexBuffer::Position), (int)_CurrentVB->getValueType(CVertexBuffer::Position),
			_CurrentVB->getValueOffEx(CVertexBuffer::TexCoord0), (int)_CurrentVB->getValueType(CVertexBuffer::TexCoord0));
		CVBDrvInfosVulkan *dbgInfo = dynamic_cast<CVBDrvInfosVulkan *>((IVBDrvInfos *)_CurrentVB->DrvInfos);
		if (dbgInfo && dbgInfo->_Mapped)
		{
			const float *v = (const float *)dbgInfo->_Mapped;
			nlinfo("Vulkan: vertex0 = %f %f %f, vertex1 = %f %f %f", v[0], v[1], v[2], v[8], v[9], v[10]);
		}
	}

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

	if (!_CurrentVB || !_FrameOpen)
		return false;
	CVBDrvInfosVulkan *info = dynamic_cast<CVBDrvInfosVulkan *>((IVBDrvInfos *)_CurrentVB->DrvInfos);
	if (!info || !info->_VertexBuffer)
		return false;

	uint32 remaining = numQuads;
	uint32 base = 0;
	while (remaining > 0)
	{
		uint32 batch = std::min(remaining, QUAD_BATCH_SIZE);
		uint32 bytes = batch * 6 * sizeof(uint16);
		VkBuffer indexBuffer = getIndexUploadBuffer(s_QuadIndices, bytes, sizeof(uint16));
		if (indexBuffer == VK_NULL_HANDLE)
			return false;

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
		key.CullMode = getenv("NLVK_NOCULL") ? VK_CULL_MODE_NONE : (_MaterialTwoSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
		key.FrontFace = VK_FRONT_FACE_CLOCKWISE;
		key.ColorWriteMask = _ClearColorMask ? _ClearColorMask : 0xF;
		key.AlphaTest = _MaterialAlphaTest ? 1 : 0;
		key.HasTexture0 = _StageTextures[0] ? 1 : 0;

		VkPipeline pipeline = getPipeline(key);
		if (pipeline == VK_NULL_HANDLE)
			return false;

		VkCommandBuffer cmd = _CommandBuffers[_FrameIndex];
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		commitPushConstants(key, cmd, *_CurrentVB);
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &info->_VertexBuffer, &offset);
		vkCmdBindIndexBuffer(cmd, indexBuffer, _IndexRingDrawOffset, VK_INDEX_TYPE_UINT16);

		// The quad index pattern addresses vertices relative to the batch start.
		sint32 firstVertex = (sint32)base;
		vkCmdDrawIndexed(cmd, batch * 6, 1, 0, firstVertex, 0);
		++_IndexedDrawCount;

		base += batch * 4;
		remaining -= batch;
	}
	return true;
}

bool CDriverVulkan::renderTrianglesWithIndexOffset(CMaterial &mat, uint32 firstIndex, uint32 ntris, uint indexOffset)
{
	if (!setupMaterial(mat))
		return false;
	return drawIndexed(mat, firstIndex, ntris * 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, _IndexFormat / 8);
}

bool CDriverVulkan::renderLinesWithIndexOffset(CMaterial &mat, uint32 firstIndex, uint32 nlines, uint indexOffset)
{
	if (!setupMaterial(mat))
		return false;
	return drawIndexed(mat, firstIndex, nlines * 2, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, _IndexFormat / 8);
}

bool CDriverVulkan::renderSimpleTrianglesWithIndexOffset(uint32 firstIndex, uint32 ntris, uint indexOffset)
{
	return drawIndexed(*_CurrentMaterial, firstIndex, ntris * 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, _IndexFormat / 8);
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
}

void CDriverVulkan::setFrustumMatrix(CMatrix &frustumMatrix)
{
	_GLProjMat = frustumMatrix;
}

CMatrix CDriverVulkan::getFrustumMatrix()
{
	return _GLProjMat;
}

void CDriverVulkan::setupViewMatrix(const CMatrix &mtx)
{
	_ViewMatrix = mtx;
	_ModelViewMatrix = mtx * _ModelMatrix;
}

void CDriverVulkan::setupViewMatrixEx(const CMatrix &mtx, const CVector &cameraPos)
{
	_ViewMatrix = mtx;
	_ModelViewMatrix = mtx * _ModelMatrix;
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
	// NeL viewports are normalized (0..1) relative to the render target.
	float x = viewport.getX() * _SwapchainExtent.width;
	float y = viewport.getY() * _SwapchainExtent.height;
	float w = viewport.getWidth() * _SwapchainExtent.width;
	float h = viewport.getHeight() * _SwapchainExtent.height;
	if (_FrameOpen)
	{
		VkViewport vk;
		vk.x = x;
		vk.y = _SwapchainExtent.height - y;
		vk.width = w;
		vk.height = -h; // GL-like bottom-up
		vk.minDepth = 0.f;
		vk.maxDepth = 1.f;
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
		VkRect2D rect;
		rect.offset.x = (sint32)(scissor.X * _SwapchainExtent.width);
		rect.offset.y = (sint32)(_SwapchainExtent.height - (scissor.Y + scissor.Height) * _SwapchainExtent.height);
		rect.extent.width = (uint32)(scissor.Width * _SwapchainExtent.width);
		rect.extent.height = (uint32)(scissor.Height * _SwapchainExtent.height);
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
}

void CDriverVulkan::setupFogMode(IDriver::TFogMode mode, float density)
{
	_FogMode = mode;
	_FogDensity = density;
}

float CDriverVulkan::getFogStart() const { return _FogStart; }
float CDriverVulkan::getFogEnd() const { return _FogEnd; }
CRGBA CDriverVulkan::getFogColor() const { return _FogColor; }
IDriver::TFogMode CDriverVulkan::getFogMode() const { return _FogMode; }
float CDriverVulkan::getFogDensity() const { return _FogDensity; }

} /* namespace NLVULKAN */
