import os

header = """#pragma once
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <android/native_window.h>
#include <android/asset_manager.h>
#include <android/hardware_buffer.h>
#include <vector>
#include "canvas.hh"

class Renderer {
public:
    Renderer(ANativeWindow* window, AAssetManager* asset_manager);
    ~Renderer();

    void draw(const Canvas& canvas);
    void update_camera_frame(AHardwareBuffer* hwb);

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }

private:
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    AAssetManager* asset_manager_ = nullptr;

    VkInstance       instance_       = VK_NULL_HANDLE;
    VkSurfaceKHR     surface_        = VK_NULL_HANDLE;
    VkPhysicalDevice physical_dev_   = VK_NULL_HANDLE;
    VkDevice         device_         = VK_NULL_HANDLE;
    VkQueue          queue_          = VK_NULL_HANDLE;
    VkSwapchainKHR   swapchain_      = VK_NULL_HANDLE;
    
    VkRenderPass                 render_pass_ = VK_NULL_HANDLE;
    std::vector<VkImage>         swapchain_images_;
    std::vector<VkImageView>     swapchain_image_views_;
    std::vector<VkFramebuffer>   framebuffers_;
    
    VkCommandPool                cmd_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmd_buffers_;
    
    VkSemaphore image_available_sem_ = VK_NULL_HANDLE;
    VkSemaphore render_finished_sem_ = VK_NULL_HANDLE;
    VkFence     in_flight_fence_     = VK_NULL_HANDLE;

    // AHardwareBuffer specific
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID vkGetAndroidHardwareBufferPropertiesANDROID_ = nullptr;
    
    AHardwareBuffer* current_hwb_ = nullptr;
    VkSamplerYcbcrConversion ycbcr_conversion_ = VK_NULL_HANDLE;
    VkSampler hwb_sampler_ = VK_NULL_HANDLE;
    VkImage hwb_image_ = VK_NULL_HANDLE;
    VkDeviceMemory hwb_memory_ = VK_NULL_HANDLE;
    VkImageView hwb_view_ = VK_NULL_HANDLE;
    
    uint64_t last_external_format_ = 0;

    // Pipeline
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    void create_instance();
    void create_surface(ANativeWindow* window);
    void pick_physical_device();
    void create_logical_device();
    void create_swapchain();
    void create_render_pass();
    void create_framebuffers();
    void create_sync_objects();
    void create_command_buffers();
    
    void setup_hwb_resources(AHardwareBuffer* hwb);
    void cleanup_hwb_resources();
    void bind_hwb(AHardwareBuffer* hwb);

    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties);
    void cleanup();
};
"""

source = """#include "renderer.hh"
#include <android/log.h>
#include <stdexcept>
#include <cstring>
#include <algorithm>

#define LOG_TAG "vk_canvas"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

Renderer::Renderer(ANativeWindow* window, AAssetManager* asset_manager) : asset_manager_(asset_manager) {
    width_  = static_cast<uint32_t>(ANativeWindow_getWidth(window));
    height_ = static_cast<uint32_t>(ANativeWindow_getHeight(window));
    create_instance();
    create_surface(window);
    pick_physical_device();
    create_logical_device();
    create_swapchain();
    create_render_pass();
    create_framebuffers();
    create_command_buffers();
    create_sync_objects();
    LOGI("Renderer ready (%ux%u)", width_, height_);
}

Renderer::~Renderer() {
    cleanup();
}

void Renderer::update_camera_frame(AHardwareBuffer* hwb) {
    if (!hwb) return;
    
    // Acquire the buffer so it doesn't get destroyed while we use it
    AHardwareBuffer_acquire(hwb);
    
    vkWaitForFences(device_, 1, &in_flight_fence_, VK_TRUE, UINT64_MAX);
    
    if (current_hwb_ != hwb) {
        if (!ycbcr_conversion_) {
            setup_hwb_resources(hwb);
        }
        bind_hwb(hwb);
        if (current_hwb_) AHardwareBuffer_release(current_hwb_);
        current_hwb_ = hwb;
    } else {
        AHardwareBuffer_release(hwb);
    }
}

void Renderer::setup_hwb_resources(AHardwareBuffer* hwb) {
    VkAndroidHardwareBufferFormatPropertiesANDROID hwb_format_props{};
    hwb_format_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;

    VkAndroidHardwareBufferPropertiesANDROID hwb_props{};
    hwb_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    hwb_props.pNext = &hwb_format_props;

    vkGetAndroidHardwareBufferPropertiesANDROID_(device_, hwb, &hwb_props);
    last_external_format_ = hwb_format_props.externalFormat;
    LOGI("HWB externalFormat: %llu", (unsigned long long)last_external_format_);

    VkExternalFormatANDROID ext_fmt{};
    ext_fmt.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    ext_fmt.externalFormat = last_external_format_;

    VkSamplerYcbcrConversionCreateInfo ycbcr_ci{};
    ycbcr_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
    ycbcr_ci.pNext = &ext_fmt;
    ycbcr_ci.format = VK_FORMAT_UNDEFINED;
    ycbcr_ci.ycbcrModel = hwb_format_props.suggestedYcbcrModel;
    ycbcr_ci.ycbcrRange = hwb_format_props.suggestedYcbcrRange;
    ycbcr_ci.components = hwb_format_props.samplerYcbcrConversionComponents;
    ycbcr_ci.xChromaOffset = hwb_format_props.suggestedXChromaOffset;
    ycbcr_ci.yChromaOffset = hwb_format_props.suggestedYChromaOffset;
    ycbcr_ci.chromaFilter = VK_FILTER_LINEAR;
    ycbcr_ci.forceExplicitReconstruction = VK_FALSE;

    if (vkCreateSamplerYcbcrConversion(device_, &ycbcr_ci, nullptr, &ycbcr_conversion_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create YCbCr conversion");
    }

    VkSamplerYcbcrConversionInfo sampler_ycbcr_info{};
    sampler_ycbcr_info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    sampler_ycbcr_info.conversion = ycbcr_conversion_;

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.pNext = &sampler_ycbcr_info;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxAnisotropy = 1.0f;
    sampler_info.unnormalizedCoordinates = VK_FALSE;
    vkCreateSampler(device_, &sampler_info, nullptr, &hwb_sampler_);

    VkDescriptorSetLayoutBinding samplerLayoutBinding{};
    samplerLayoutBinding.binding = 0;
    samplerLayoutBinding.descriptorCount = 1;
    samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerLayoutBinding.pImmutableSamplers = &hwb_sampler_;
    samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &samplerLayoutBinding;
    vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &desc_layout_);

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;
    vkCreateDescriptorPool(device_, &poolInfo, nullptr, &desc_pool_);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = desc_pool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &desc_layout_;
    vkAllocateDescriptorSets(device_, &allocInfo, &desc_set_);

    // Pipeline
    auto loadShader = [this](const char* path) -> VkShaderModule {
        AAsset* asset = AAssetManager_open(asset_manager_, path, AASSET_MODE_BUFFER);
        if (!asset) return VK_NULL_HANDLE;
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = AAsset_getLength(asset);
        ci.pCode = (const uint32_t*)AAsset_getBuffer(asset);
        VkShaderModule mod;
        vkCreateShaderModule(device_, &ci, nullptr, &mod);
        AAsset_close(asset);
        return mod;
    };

    VkShaderModule vert = loadShader("shaders/composite_vert.spv");
    VkShaderModule frag = loadShader("shaders/composite_frag.spv");

    VkPipelineShaderStageCreateInfo shaderStages[2]{};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vert;
    shaderStages[0].pName = "main";
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = frag;
    shaderStages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamic_states;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &desc_layout_;
    vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipeline_layout_);

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipeline_layout_;
    pipelineInfo.renderPass = render_pass_;
    pipelineInfo.subpass = 0;
    vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);

    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);
}

void Renderer::bind_hwb(AHardwareBuffer* hwb) {
    if (hwb_image_) {
        vkDestroyImageView(device_, hwb_view_, nullptr);
        vkDestroyImage(device_, hwb_image_, nullptr);
        vkFreeMemory(device_, hwb_memory_, nullptr);
    }

    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(hwb, &desc);

    VkExternalMemoryImageCreateInfo ext_mem_ci{};
    ext_mem_ci.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_mem_ci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &ext_mem_ci;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = desc.width;
    image_info.extent.height = desc.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = VK_FORMAT_UNDEFINED;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateImage(device_, &image_info, nullptr, &hwb_image_) != VK_SUCCESS) {
        LOGE("Failed to create HWB image");
        return;
    }

    VkImportAndroidHardwareBufferInfoANDROID import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    import_info.buffer = hwb;

    VkMemoryDedicatedAllocateInfo dedicated_info{};
    dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated_info.pNext = &import_info;
    dedicated_info.image = hwb_image_;

    VkAndroidHardwareBufferPropertiesANDROID hwb_props{};
    hwb_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    vkGetAndroidHardwareBufferPropertiesANDROID_(device_, hwb, &hwb_props);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = &dedicated_info;
    alloc_info.allocationSize = hwb_props.allocationSize;
    alloc_info.memoryTypeIndex = find_memory_type(hwb_props.memoryTypeBits, 0);

    if (vkAllocateMemory(device_, &alloc_info, nullptr, &hwb_memory_) != VK_SUCCESS) {
        LOGE("Failed to allocate HWB memory");
        return;
    }

    vkBindImageMemory(device_, hwb_image_, hwb_memory_, 0);

    VkSamplerYcbcrConversionInfo ycbcr_info{};
    ycbcr_info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    ycbcr_info.conversion = ycbcr_conversion_;

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = &ycbcr_info;
    view_info.image = hwb_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_UNDEFINED;
    view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &view_info, nullptr, &hwb_view_) != VK_SUCCESS) {
        LOGE("Failed to create HWB image view");
        return;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = hwb_view_;
    imageInfo.sampler = hwb_sampler_;

    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = desc_set_;
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_, 1, &descriptorWrite, 0, nullptr);
}

void Renderer::draw(const Canvas& /*canvas*/) {
    if (!device_) return;
    vkWaitForFences(device_, 1, &in_flight_fence_, VK_TRUE, UINT64_MAX);

    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_sem_, VK_NULL_HANDLE, &image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) return;

    vkResetFences(device_, 1, &in_flight_fence_);
    vkResetCommandBuffer(cmd_buffers_[image_index], 0);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd_buffers_[image_index], &begin_info);

    VkRenderPassBeginInfo rp_info{};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_info.renderPass = render_pass_;
    rp_info.framebuffer = framebuffers_[image_index];
    rp_info.renderArea.offset = {0, 0};
    rp_info.renderArea.extent = {width_, height_};

    VkClearValue clear_color = {{{0.05f, 0.05f, 0.07f, 1.0f}}};
    rp_info.clearValueCount = 1;
    rp_info.pClearValues = &clear_color;

    vkCmdBeginRenderPass(cmd_buffers_[image_index], &rp_info, VK_SUBPASS_CONTENTS_INLINE);

    if (pipeline_ && current_hwb_) {
        AHardwareBuffer_Desc desc;
        AHardwareBuffer_describe(current_hwb_, &desc);
        
        // Android camera frames are typically 90 degrees rotated.
        float raw_w = desc.height;
        float raw_h = desc.width;
        float scale = std::min((float)width_ / raw_w, (float)height_ / raw_h);
        float draw_w = raw_w * scale;
        float draw_h = raw_h * scale;
        float x_offset = (width_ - draw_w) / 2.0f;
        float y_offset = (height_ - draw_h) / 2.0f;
        
        VkViewport viewport{};
        viewport.x = x_offset;
        viewport.y = y_offset;
        viewport.width = draw_w;
        viewport.height = draw_h;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd_buffers_[image_index], 0, 1, &viewport);
        
        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {width_, height_};
        vkCmdSetScissor(cmd_buffers_[image_index], 0, 1, &scissor);

        vkCmdBindPipeline(cmd_buffers_[image_index], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(cmd_buffers_[image_index], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &desc_set_, 0, nullptr);
        vkCmdDraw(cmd_buffers_[image_index], 3, 1, 0, 0);
    }

    vkCmdEndRenderPass(cmd_buffers_[image_index]);
    vkEndCommandBuffer(cmd_buffers_[image_index]);

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkSemaphore wait_sems[] = {image_available_sem_};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = wait_sems;
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buffers_[image_index];

    VkSemaphore signal_sems[] = {render_finished_sem_};
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = signal_sems;

    vkQueueSubmit(queue_, 1, &submit_info, in_flight_fence_);

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = signal_sems;
    VkSwapchainKHR swapchains[] = {swapchain_};
    present_info.swapchainCount = 1;
    present_info.pSwapchains = swapchains;
    present_info.pImageIndices = &image_index;

    vkQueuePresentKHR(queue_, &present_info);
}

uint32_t Renderer::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_dev_, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

void Renderer::create_instance() {
    VkApplicationInfo app_info{};
    app_info.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "vk_canvas";
    app_info.apiVersion       = VK_API_VERSION_1_1;

    const char* extensions[] = {
        "VK_KHR_surface",
        "VK_KHR_android_surface",
        "VK_KHR_external_memory_capabilities",
        "VK_KHR_get_physical_device_properties2"
    };

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &app_info;
    ci.enabledExtensionCount   = 4;
    ci.ppEnabledExtensionNames = extensions;

    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateInstance failed");
}

void Renderer::create_surface(ANativeWindow* window) {
    VkAndroidSurfaceCreateInfoKHR ci{};
    ci.sType  = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = window;

    auto fn = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
        vkGetInstanceProcAddr(instance_, "vkCreateAndroidSurfaceKHR"));
    if (!fn || fn(instance_, &ci, nullptr, &surface_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateAndroidSurfaceKHR failed");
}

void Renderer::pick_physical_device() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) throw std::runtime_error("no Vulkan physical devices");
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(instance_, &count, devs.data());
    physical_dev_ = devs[0];
}

void Renderer::create_logical_device() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &priority;

    const char* dev_exts[] = {
        "VK_KHR_swapchain",
        "VK_KHR_sampler_ycbcr_conversion",
        "VK_KHR_external_memory",
        "VK_ANDROID_external_memory_android_hardware_buffer",
        "VK_EXT_queue_family_foreign",
        "VK_KHR_bind_memory2",
        "VK_KHR_maintenance1"
    };

    VkPhysicalDeviceFeatures features{};
    features.samplerAnisotropy = VK_TRUE;

    VkDeviceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount    = 1;
    ci.pQueueCreateInfos       = &qci;
    ci.enabledExtensionCount   = 7;
    ci.ppEnabledExtensionNames = dev_exts;
    ci.pEnabledFeatures        = &features;

    if (vkCreateDevice(physical_dev_, &ci, nullptr, &device_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDevice failed");

    vkGetDeviceQueue(device_, 0, 0, &queue_);
    
    vkGetAndroidHardwareBufferPropertiesANDROID_ = 
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(device_, "vkGetAndroidHardwareBufferPropertiesANDROID");
}

void Renderer::create_swapchain() {
    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = surface_;
    ci.minImageCount    = 2;
    ci.imageFormat      = VK_FORMAT_R8G8B8A8_UNORM;
    ci.imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent      = { width_, height_ };
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped          = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateSwapchainKHR failed");
}

void Renderer::create_render_pass() {
    VkAttachmentDescription color_attachment{};
    color_attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments = &color_attachment;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;

    vkCreateRenderPass(device_, &ci, nullptr, &render_pass_);
}

void Renderer::create_framebuffers() {
    uint32_t image_count;
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    swapchain_images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, swapchain_images_.data());

    swapchain_image_views_.resize(image_count);
    framebuffers_.resize(image_count);

    for (size_t i = 0; i < image_count; i++) {
        VkImageViewCreateInfo iv_ci{};
        iv_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv_ci.image = swapchain_images_[i];
        iv_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        iv_ci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv_ci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv_ci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv_ci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv_ci.subresourceRange.baseMipLevel = 0;
        iv_ci.subresourceRange.levelCount = 1;
        iv_ci.subresourceRange.baseArrayLayer = 0;
        iv_ci.subresourceRange.layerCount = 1;

        vkCreateImageView(device_, &iv_ci, nullptr, &swapchain_image_views_[i]);

        VkFramebufferCreateInfo fb_ci{};
        fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = render_pass_;
        fb_ci.attachmentCount = 1;
        fb_ci.pAttachments = &swapchain_image_views_[i];
        fb_ci.width = width_;
        fb_ci.height = height_;
        fb_ci.layers = 1;

        vkCreateFramebuffer(device_, &fb_ci, nullptr, &framebuffers_[i]);
    }
}

void Renderer::create_command_buffers() {
    VkCommandPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_ci.queueFamilyIndex = 0;

    vkCreateCommandPool(device_, &pool_ci, nullptr, &cmd_pool_);

    cmd_buffers_.resize(framebuffers_.size());
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = cmd_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = (uint32_t)cmd_buffers_.size();

    vkAllocateCommandBuffers(device_, &alloc_info, cmd_buffers_.data());
}

void Renderer::create_sync_objects() {
    VkSemaphoreCreateInfo sem_ci{};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    vkCreateSemaphore(device_, &sem_ci, nullptr, &image_available_sem_);
    vkCreateSemaphore(device_, &sem_ci, nullptr, &render_finished_sem_);
    vkCreateFence(device_, &fence_ci, nullptr, &in_flight_fence_);
}

void Renderer::cleanup_hwb_resources() {
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    if (desc_layout_) vkDestroyDescriptorSetLayout(device_, desc_layout_, nullptr);
    
    if (hwb_sampler_) vkDestroySampler(device_, hwb_sampler_, nullptr);
    if (ycbcr_conversion_) vkDestroySamplerYcbcrConversion(device_, ycbcr_conversion_, nullptr);
    
    if (hwb_view_) vkDestroyImageView(device_, hwb_view_, nullptr);
    if (hwb_image_) vkDestroyImage(device_, hwb_image_, nullptr);
    if (hwb_memory_) vkFreeMemory(device_, hwb_memory_, nullptr);
    
    if (current_hwb_) {
        AHardwareBuffer_release(current_hwb_);
        current_hwb_ = nullptr;
    }
}

void Renderer::cleanup() {
    if (device_) vkDeviceWaitIdle(device_);
    cleanup_hwb_resources();

    if (image_available_sem_) vkDestroySemaphore(device_, image_available_sem_, nullptr);
    if (render_finished_sem_) vkDestroySemaphore(device_, render_finished_sem_, nullptr);
    if (in_flight_fence_)     vkDestroyFence(device_, in_flight_fence_, nullptr);
    if (cmd_pool_)            vkDestroyCommandPool(device_, cmd_pool_, nullptr);
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    for (auto iv : swapchain_image_views_) vkDestroyImageView(device_, iv, nullptr);
    if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    if (device_)    vkDestroyDevice(device_, nullptr);
    if (surface_)   vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_)  vkDestroyInstance(instance_, nullptr);
}
"""

with open("script.py", "w") as f:
    f.write(f"open('libs/vulkan_canvas_engine/app/src/main/cpp/renderer.hh', 'w').write({repr(header)})\n")
    f.write(f"open('libs/vulkan_canvas_engine/app/src/main/cpp/renderer.cc', 'w').write({repr(source)})\n")
