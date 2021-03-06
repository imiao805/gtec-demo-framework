/****************************************************************************************************************************************************
 * Copyright 2017 NXP
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    * Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *
 *    * Redistributions in binary form must reproduce the above copyright notice,
 *      this list of conditions and the following disclaimer in the documentation
 *      and/or other materials provided with the distribution.
 *
 *    * Neither the name of the NXP. nor the names of
 *      its contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************************************************************************************/

#include <FslUtil/Vulkan1_0/Batch/QuadBatch.hpp>
#include <FslBase/Bits/AlignmentUtil.hpp>
#include <FslBase/Exceptions.hpp>
#include <FslBase/Math/MathHelper.hpp>
#include <FslBase/Log/Log.hpp>
#include <FslGraphics/Vertices/VertexDeclaration.hpp>
#include <FslGraphics/Vertices/VertexElementFormat.hpp>
#include <FslUtil/Vulkan1_0/Extend/BufferEx.hpp>
#include <FslUtil/Vulkan1_0/Extend/DeviceMemoryEx.hpp>
#include <FslUtil/Vulkan1_0/Util/ConvertUtil.hpp>
#include <FslUtil/Vulkan1_0/Util/MemoryTypeUtil.hpp>
#include <RapidVulkan/Check.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <utility>

using namespace RapidVulkan;

namespace Fsl
{
  namespace Vulkan
  {
    namespace
    {
      const uint32_t QUAD_MIN_BATCH_SIZE = 2048;
      const int32_t INTERNAL_QUAD_VERTEX_COUNT = 6;


      VUBuffer CreateBuffer(const VUDevice& device, const VkDeviceSize bufferByteSize, const VkBufferUsageFlags usageFlags,
                            const VkMemoryPropertyFlags memoryPropertyFlags)
      {
        VkBufferCreateInfo bufferCreateInfo{};
        bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferCreateInfo.size = bufferByteSize;
        bufferCreateInfo.usage = usageFlags;
        bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        return VUBuffer(device, bufferCreateInfo, memoryPropertyFlags);
      }


      VUBuffer CreateUniformBuffer(const VUDevice& device)
      {
        const auto bufferByteSize = AlignmentUtil::GetByteSize(sizeof(Matrix), 16);
        return CreateBuffer(device, bufferByteSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      }


      DescriptorSetLayout CreateDescriptorSetLayout(const VkDevice device, const VkDescriptorType descriptorType, const VkShaderStageFlags stageFlags)
      {
        // Descriptor set layout
        VkDescriptorSetLayoutBinding setLayoutBindings{};
        setLayoutBindings.binding = 0;
        setLayoutBindings.descriptorType = descriptorType;
        setLayoutBindings.descriptorCount = 1;
        setLayoutBindings.stageFlags = stageFlags;

        VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
        descriptorSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptorSetLayoutInfo.pNext = nullptr;
        descriptorSetLayoutInfo.bindingCount = 1;
        descriptorSetLayoutInfo.pBindings = &setLayoutBindings;

        return DescriptorSetLayout(device, descriptorSetLayoutInfo);
      }


      DescriptorPool CreateDescriptorPool(const VkDevice device, const VkDescriptorType type)
      {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = type;
        poolSize.descriptorCount = 1;

        VkDescriptorPoolCreateInfo descriptorPoolCreateInfo{};
        descriptorPoolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descriptorPoolCreateInfo.maxSets = 1;
        descriptorPoolCreateInfo.poolSizeCount = 1;
        descriptorPoolCreateInfo.pPoolSizes = &poolSize;

        return DescriptorPool(device, descriptorPoolCreateInfo);
      }


      VkDescriptorSet CreateDescriptorSet(const VkDevice device, const DescriptorPool& descriptorPool, const DescriptorSetLayout& descriptorSetLayout)
      {
        VkDescriptorSetAllocateInfo descriptorSetAllocInfo{};
        descriptorSetAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descriptorSetAllocInfo.descriptorPool = descriptorPool.Get();
        descriptorSetAllocInfo.descriptorSetCount = 1;
        descriptorSetAllocInfo.pSetLayouts = descriptorSetLayout.GetPointer();

        VkDescriptorSet descriptorSet{};
        RAPIDVULKAN_CHECK(vkAllocateDescriptorSets(device, &descriptorSetAllocInfo, &descriptorSet));
        return descriptorSet;
      }


      PipelineLayout CreatePipelineLayout(const VkDevice device, const VkDescriptorSetLayout& descriptorSetLayoutUniform,
                                          const VkDescriptorSetLayout& descriptorSetLayoutTexture)
      {
        VkDescriptorSetLayout layouts[] = {descriptorSetLayoutUniform, descriptorSetLayoutTexture};

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 2;
        pipelineLayoutInfo.pSetLayouts = layouts;
        return PipelineLayout(device, pipelineLayoutInfo);
      }


      PipelineCache CreatePipelineCache(const VkDevice device)
      {
        // Pipeline cache
        VkPipelineCacheCreateInfo pipelineCacheCreateInfo{};
        pipelineCacheCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        return PipelineCache(device, pipelineCacheCreateInfo);
      }


      VkFormat Convert(const VertexElementFormat& format)
      {
        switch (format)
        {
        case VertexElementFormat::Single:
          return VK_FORMAT_R32_SFLOAT;
        case VertexElementFormat::Vector2:
          return VK_FORMAT_R32G32_SFLOAT;
        case VertexElementFormat::Vector3:
          return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexElementFormat::Vector4:
          return VK_FORMAT_R32G32B32A32_SFLOAT;
        case VertexElementFormat::Matrix4x4:
          throw NotSupportedException("Unsupported VertexElementFormat Matrix4x4");
        default:
          throw NotSupportedException("Unsupported VertexElementFormat");
        }
      }


      std::array<VkVertexInputAttributeDescription, 3> GetVertexAttributes(const VertexDeclaration& vertexDecl)
      {
        assert(vertexDecl.Count() == 3);

        std::array<VkVertexInputAttributeDescription, 3> vertexAttributes{};

        for (std::size_t i = 0; i < vertexAttributes.size(); ++i)
        {
          auto entry = vertexDecl.At(i);
          vertexAttributes[i].location = static_cast<uint32_t>(i);
          vertexAttributes[i].binding = 0;
          vertexAttributes[i].format = Convert(entry.Format);
          vertexAttributes[i].offset = entry.Offset;
        }
        return vertexAttributes;
      }

      VkPipelineColorBlendAttachmentState CreatePipelineColorBlendAttachmentState(const BlendState blendState)
      {
        VkPipelineColorBlendAttachmentState blendAttachmentState{};
        blendAttachmentState.colorWriteMask =
          VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        switch (blendState)
        {
        case BlendState::Additive:
          blendAttachmentState.blendEnable = VK_TRUE;
          blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
          blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
          blendAttachmentState.colorBlendOp = VK_BLEND_OP_ADD;
          blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
          blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
          blendAttachmentState.alphaBlendOp = VK_BLEND_OP_ADD;
          break;
        case BlendState::AlphaBlend:
          blendAttachmentState.blendEnable = VK_TRUE;
          blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
          blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
          blendAttachmentState.colorBlendOp = VK_BLEND_OP_ADD;
          blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
          blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
          blendAttachmentState.alphaBlendOp = VK_BLEND_OP_ADD;
          break;
        case BlendState::NonPremultiplied:
          blendAttachmentState.blendEnable = VK_TRUE;
          blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
          blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
          blendAttachmentState.colorBlendOp = VK_BLEND_OP_ADD;
          blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
          blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
          blendAttachmentState.alphaBlendOp = VK_BLEND_OP_ADD;
          break;
        case BlendState::Opaque:
          blendAttachmentState.blendEnable = VK_FALSE;
          blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
          blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
          blendAttachmentState.colorBlendOp = VK_BLEND_OP_ADD;
          blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
          blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
          blendAttachmentState.alphaBlendOp = VK_BLEND_OP_ADD;
          break;
        default:
          throw NotSupportedException("Unsupported blend state");
        }
        return blendAttachmentState;
      }

      GraphicsPipeline CreateGraphicsPipeline(const VkDevice device, const ShaderModule& vertexShader, const ShaderModule& fragmentShader,
                                              const PipelineLayout& pipelineLayout, const PipelineCache& pipelineCache, const VkRenderPass renderPass,
                                              const BlendState blendState)
      {
        VkPipelineShaderStageCreateInfo pipelineShaderStageCreateInfo[2]{};
        pipelineShaderStageCreateInfo[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineShaderStageCreateInfo[0].flags = 0;
        pipelineShaderStageCreateInfo[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        pipelineShaderStageCreateInfo[0].module = vertexShader.Get();
        pipelineShaderStageCreateInfo[0].pName = "main";
        pipelineShaderStageCreateInfo[0].pSpecializationInfo = nullptr;

        pipelineShaderStageCreateInfo[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineShaderStageCreateInfo[1].flags = 0;
        pipelineShaderStageCreateInfo[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        pipelineShaderStageCreateInfo[1].module = fragmentShader.Get();
        pipelineShaderStageCreateInfo[1].pName = "main";
        pipelineShaderStageCreateInfo[1].pSpecializationInfo = nullptr;

        auto vertexDecl = VertexPositionColorTexture::GetVertexDeclaration();

        VkVertexInputBindingDescription vertexBindings{};
        vertexBindings.binding = 0;
        vertexBindings.stride = vertexDecl.VertexStride();
        vertexBindings.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        const auto vertexAttributes = GetVertexAttributes(vertexDecl);

        VkPipelineVertexInputStateCreateInfo vertexInputState{};
        vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputState.vertexBindingDescriptionCount = 1;
        vertexInputState.pVertexBindingDescriptions = &vertexBindings;
        vertexInputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexAttributes.size());
        vertexInputState.pVertexAttributeDescriptions = vertexAttributes.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{};
        inputAssemblyState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        // viewportState.pViewports = nullptr; // Ignored due to dynamic state
        viewportState.scissorCount = 1;
        // viewportState.pScissors = nullptr;  // Ignored due to dynamic state

        VkPipelineRasterizationStateCreateInfo rasterizationState{};
        rasterizationState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizationState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizationState.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisampleState{};
        multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencilState{};
        depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencilState.depthTestEnable = VK_FALSE;
        depthStencilState.depthWriteEnable = VK_FALSE;
        depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        depthStencilState.front.compareOp = VK_COMPARE_OP_ALWAYS;
        depthStencilState.back.compareOp = VK_COMPARE_OP_ALWAYS;

        auto blendAttachmentState = CreatePipelineColorBlendAttachmentState(blendState);

        VkPipelineColorBlendStateCreateInfo colorBlendState{};
        colorBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlendState.attachmentCount = 1;
        colorBlendState.pAttachments = &blendAttachmentState;

        std::vector<VkDynamicState> dynamicStateEnables = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.flags = 0;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStateEnables.size());
        dynamicState.pDynamicStates = dynamicStateEnables.data();

        VkGraphicsPipelineCreateInfo pipelineCreateInfo{};
        pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineCreateInfo.stageCount = 2;
        pipelineCreateInfo.pStages = pipelineShaderStageCreateInfo;
        pipelineCreateInfo.pVertexInputState = &vertexInputState;
        pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
        pipelineCreateInfo.pViewportState = &viewportState;
        pipelineCreateInfo.pRasterizationState = &rasterizationState;
        pipelineCreateInfo.pMultisampleState = &multisampleState;
        pipelineCreateInfo.pDepthStencilState = &depthStencilState;
        pipelineCreateInfo.pColorBlendState = &colorBlendState;
        pipelineCreateInfo.pDynamicState = &dynamicState;
        pipelineCreateInfo.layout = pipelineLayout.Get();
        pipelineCreateInfo.renderPass = renderPass;

        return GraphicsPipeline(device, pipelineCache.Get(), pipelineCreateInfo);
      }


      void UpdateDescriptorSetUniform(const VkDevice device, const VkDescriptorSet descriptorSet, const VkDescriptorBufferInfo& bufferInfo)
      {
        VkWriteDescriptorSet writeDescriptorSet{};
        // Binding 0: Vertex shader uniform buffer
        writeDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescriptorSet.dstSet = descriptorSet;
        writeDescriptorSet.dstBinding = 0;
        writeDescriptorSet.descriptorCount = 1;
        writeDescriptorSet.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writeDescriptorSet.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
      }

      void UpdateDescriptorSetTexture(const VkDevice device, const VkDescriptorSet descriptorSet, const VkDescriptorImageInfo& imageInfo)
      {
        VkWriteDescriptorSet writeDescriptorSet{};
        // Binding 0: Fragment shader texture sampler
        writeDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescriptorSet.dstSet = descriptorSet;
        writeDescriptorSet.dstBinding = 0;
        writeDescriptorSet.descriptorCount = 1;
        writeDescriptorSet.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writeDescriptorSet.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
      }
    }


    QuadBatch::QuadBatch(const VUDevice& device, const VkRenderPass renderPass, const uint32_t commandBufferCount,
                         const std::vector<uint8_t>& vertexShaderBinary, const std::vector<uint8_t>& fragmentShaderBinary,
                         const uint32_t quadCapacityHint)
      : m_commandBufferCount(commandBufferCount)
      , m_quadCapacity(std::max(quadCapacityHint, QUAD_MIN_BATCH_SIZE))
      , m_vertexCapacity(INTERNAL_QUAD_VERTEX_COUNT * m_quadCapacity)
      , m_uniformBuffer(CreateUniformBuffer(device))
      , m_descriptorSetLayoutUniform(CreateDescriptorSetLayout(device.Get(), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT))
      , m_descriptorPool(CreateDescriptorPool(device.Get(), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER))
      , m_descriptorSetUniform(CreateDescriptorSet(device.Get(), m_descriptorPool, m_descriptorSetLayoutUniform))
      , m_descriptorSetTexture(CreateDescriptorSetLayout(device.Get(), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT))
      , m_render(commandBufferCount)
      , m_vertexShader(device.Get(), 0, vertexShaderBinary.size(), reinterpret_cast<const uint32_t*>(vertexShaderBinary.data()))
      , m_fragmentShader(device.Get(), 0, fragmentShaderBinary.size(), reinterpret_cast<const uint32_t*>(fragmentShaderBinary.data()))
      , m_pipelineLayout(CreatePipelineLayout(device.Get(), m_descriptorSetLayoutUniform.Get(), m_descriptorSetTexture.Get()))
      , m_pipelineCache(CreatePipelineCache(device.Get()))
      , m_pipelineAdditive(
          CreateGraphicsPipeline(device.Get(), m_vertexShader, m_fragmentShader, m_pipelineLayout, m_pipelineCache, renderPass, BlendState::Additive))
      , m_pipelineAlphaBlend(CreateGraphicsPipeline(device.Get(), m_vertexShader, m_fragmentShader, m_pipelineLayout, m_pipelineCache, renderPass,
                                                    BlendState::AlphaBlend))
      , m_pipelineNonPremultiplied(CreateGraphicsPipeline(device.Get(), m_vertexShader, m_fragmentShader, m_pipelineLayout, m_pipelineCache,
                                                          renderPass, BlendState::NonPremultiplied))
      , m_pipelineOpaque(
          CreateGraphicsPipeline(device.Get(), m_vertexShader, m_fragmentShader, m_pipelineLayout, m_pipelineCache, renderPass, BlendState::Opaque))
    {
      if (commandBufferCount <= 0)
      {
        throw std::invalid_argument("commandBufferCount should be >= 1");
      }

      for (auto& rEntry : m_render)
      {
        rEntry.Reset(device.GetPhysicalDevice(), device.Get(), m_descriptorSetTexture.Get(), INTERNAL_QUAD_VERTEX_COUNT);
      }
    }


    void QuadBatch::BeginFrame(const uint32_t commandBufferIndex, const VkCommandBuffer commandBuffer, const Extent2D& screenExtent)
    {
      if (commandBufferIndex > m_commandBufferCount)
      {
        throw std::invalid_argument("commandBufferIndex should be less than commandBufferCount");
      }
      if (m_activeCommandBuffer != VK_NULL_HANDLE)
      {
        throw UsageErrorException("Can not call BeginFrame while inside a BeginFrame/EndFrame block");
      }
      assert(m_render[commandBufferIndex].IsValid());

      if (screenExtent != m_cachedScreenExtent)
      {
        m_cachedScreenExtent = screenExtent;

        // Setup the shader
        const auto screenWidth = static_cast<float>(screenExtent.Width);
        const auto screenHeight = static_cast<float>(screenExtent.Height);
        const float screenOffsetX = std::floor(screenWidth / 2.0f);
        const float screenOffsetY = std::floor(screenHeight / 2.0f);

        const Matrix matViewProj =
          Matrix::CreateTranslation(-screenOffsetX, -screenOffsetY, -2.0f) * Matrix::CreateOrthographic(screenWidth, screenHeight, 1.0f, 10.0f);
        m_uniformBuffer.Upload(0, 0, matViewProj);
        UpdateDescriptorSetUniform(m_uniformBuffer.GetDevice(), m_descriptorSetUniform, m_uniformBuffer.GetDescriptor());
      }


      // FIX: there is no need for this to be dynamic. So change the code to use static viewport and scissor
      //      once we implement 'soft' restart.
      {    // Set default viewport and scissor
        VkRect2D framebufferRect{};
        framebufferRect.extent = ConvertUtil::Convert(screenExtent);

        VkViewport viewport{};
        viewport.x = static_cast<float>(framebufferRect.offset.x);
        viewport.y = static_cast<float>(framebufferRect.offset.y);
        viewport.width = static_cast<float>(framebufferRect.extent.width);
        viewport.height = static_cast<float>(framebufferRect.extent.height);

        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &framebufferRect);
      }

      m_activeCommandBufferIndex = commandBufferIndex;
      m_activeCommandBuffer = commandBuffer;
    }


    void QuadBatch::Begin(const Point2& screenResolution, const BlendState blendState, const bool restoreState)
    {
      if (m_activeCommandBuffer == VK_NULL_HANDLE)
      {
        throw UsageErrorException("Call BeginFrame before Begin");
      }

      if (static_cast<uint32_t>(screenResolution.X) != m_cachedScreenExtent.Width ||
          static_cast<uint32_t>(screenResolution.Y) != m_cachedScreenExtent.Height)
      {
        throw NotSupportedException("Dynamic changes of the screen resolution is not supported");
      }

      switch (blendState)
      {
      case BlendState::Additive:
        m_activePipeline = m_pipelineAdditive.Get();
        break;
      case BlendState::AlphaBlend:
        m_activePipeline = m_pipelineAlphaBlend.Get();
        break;
      case BlendState::NonPremultiplied:
        m_activePipeline = m_pipelineNonPremultiplied.Get();
        break;
      case BlendState::Opaque:
        m_activePipeline = m_pipelineOpaque.Get();
        break;
      default:
        throw NotSupportedException("Unsupported blend state");
      }
    }

    // Basic quad vertex format
    // 0-1
    // | |
    // 2-3
    void QuadBatch::DrawQuads(const VertexPositionColorTexture* const pVertices, const uint32_t length, const VUTextureInfo& textureInfo)
    {
      if (m_activeCommandBuffer == VK_NULL_HANDLE)
      {
        throw UsageErrorException("Call Begin before DrawQuads");
      }
      auto& rRender = m_render[m_activeCommandBufferIndex];

      const VertexPositionColorTexture* pSrcVertices = pVertices;
      const VertexPositionColorTexture* const pSrcVerticesEnd = pVertices + (length * 4);

      uint32_t remainingQuads = length;
      while (pSrcVertices < pSrcVerticesEnd)
      {
        auto current = rRender.VertexBuffers.NextFree(remainingQuads * INTERNAL_QUAD_VERTEX_COUNT);

        auto pDst = current.pMapped;
        auto pDstEnd = current.pMapped + current.VertexCapacity;

        static_assert(INTERNAL_QUAD_VERTEX_COUNT == 6, "the code below expects the internal quad vertex count to be six");
        while (pDst < pDstEnd)
        {
          // Expand the quad to two counter clockwise triangles
          pDst[0] = pSrcVertices[2];
          pDst[1] = pSrcVertices[3];
          pDst[2] = pSrcVertices[0];

          pDst[3] = pSrcVertices[0];
          pDst[4] = pSrcVertices[3];
          pDst[5] = pSrcVertices[1];

          pSrcVertices += 4;
          pDst += INTERNAL_QUAD_VERTEX_COUNT;
          --remainingQuads;
        }

        assert(m_activeCommandBuffer != VK_NULL_HANDLE);

        auto currentPipeline = m_activePipeline;
        if (currentPipeline != m_cachedPipeline)
        {
          m_cachedPipeline = currentPipeline;
          vkCmdBindPipeline(m_activeCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, currentPipeline);
        }

        if (textureInfo != m_cachedTextureInfo)
        {
          m_cachedTextureDescriptorSet = rRender.TextureDescriptorSets.NextFree();
          m_cachedTextureInfo = textureInfo;

          assert(m_cachedTextureDescriptorSet != VK_NULL_HANDLE);

          UpdateDescriptorSetTexture(m_uniformBuffer.GetDevice(), m_cachedTextureDescriptorSet, textureInfo.ImageInfo);

          VkDescriptorSet descriptorSets[2] = {m_descriptorSetUniform, m_cachedTextureDescriptorSet};
          vkCmdBindDescriptorSets(m_activeCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout.Get(), 0, 2, descriptorSets, 0, nullptr);
        }

        VkDeviceSize offsets = 0;
        vkCmdBindVertexBuffers(m_activeCommandBuffer, 0, 1, &current.VertexBuffer, &offsets);
        vkCmdDraw(m_activeCommandBuffer, current.VertexCapacity, 1, current.UsedStartIndex, 0);
      }
    }


    void QuadBatch::End()
    {
      if (m_activeCommandBuffer == VK_NULL_HANDLE)
      {
        throw UsageErrorException("Call Begin before End");
      }
    }


    void QuadBatch::EndFrame()
    {
      if (m_activeCommandBuffer == VK_NULL_HANDLE)
      {
        throw UsageErrorException("Can not call EndFrame without a BeginFrame");
      }

      m_render[m_activeCommandBufferIndex].Clear();

      m_activePipeline = VK_NULL_HANDLE;
      m_activeCommandBufferIndex = 0;
      m_activeCommandBuffer = VK_NULL_HANDLE;
      m_cachedPipeline = VK_NULL_HANDLE;
      m_cachedTextureInfo.Reset();
      m_cachedTextureDescriptorSet = VK_NULL_HANDLE;
    }
  }
}
