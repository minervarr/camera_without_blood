import re

header_path = "libs/vulkan_canvas_engine/app/src/main/cpp/renderer.hh"
source_path = "libs/vulkan_canvas_engine/app/src/main/cpp/renderer.cc"

with open(header_path, 'r') as f:
    header = f.read()

# Add unordered_map to header
if "<unordered_map>" not in header:
    header = header.replace("#include <vector>", "#include <vector>\n#include <unordered_map>")

# Add cache struct and map
cache_struct = """
    struct HwbCache {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet desc_set = VK_NULL_HANDLE;
    };
    std::unordered_map<AHardwareBuffer*, HwbCache> hwb_cache_;
"""

if "HwbCache" not in header:
    header = header.replace("AHardwareBuffer* current_hwb_ = nullptr;", cache_struct + "\n    AHardwareBuffer* current_hwb_ = nullptr;")
    header = header.replace("VkDescriptorSet desc_set_ = VK_NULL_HANDLE;", "")

with open(header_path, 'w') as f:
    f.write(header)

# Now source file
with open(source_path, 'r') as f:
    source = f.read()

# Update bind_hwb to use cache
bind_hwb_new = """void Renderer::bind_hwb(AHardwareBuffer* hwb) {
    if (hwb_cache_.find(hwb) != hwb_cache_.end()) return;

    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(hwb, &desc);

    VkExternalMemoryImageCreateInfo ext_mem_ci{};
    ext_mem_ci.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_mem_ci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkExternalFormatANDROID ext_fmt{};
    ext_fmt.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    ext_fmt.pNext = &ext_mem_ci;
    ext_fmt.externalFormat = last_external_format_;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &ext_fmt;
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
    
    HwbCache cache;
    if (vkCreateImage(device_, &image_info, nullptr, &cache.image) != VK_SUCCESS) return;

    VkImportAndroidHardwareBufferInfoANDROID import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    import_info.buffer = hwb;

    VkMemoryDedicatedAllocateInfo dedicated_info{};
    dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated_info.pNext = &import_info;
    dedicated_info.image = cache.image;

    VkAndroidHardwareBufferPropertiesANDROID hwb_props{};
    hwb_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    vkGetAndroidHardwareBufferPropertiesANDROID_(device_, hwb, &hwb_props);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = &dedicated_info;
    alloc_info.allocationSize = hwb_props.allocationSize;
    alloc_info.memoryTypeIndex = find_memory_type(hwb_props.memoryTypeBits, 0);

    if (vkAllocateMemory(device_, &alloc_info, nullptr, &cache.memory) != VK_SUCCESS) return;

    vkBindImageMemory(device_, cache.image, cache.memory, 0);

    VkSamplerYcbcrConversionInfo ycbcr_info{};
    ycbcr_info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    ycbcr_info.conversion = ycbcr_conversion_;

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = &ycbcr_info;
    view_info.image = cache.image;
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

    if (vkCreateImageView(device_, &view_info, nullptr, &cache.view) != VK_SUCCESS) return;

    VkDescriptorSetAllocateInfo alloc_info_desc{};
    alloc_info_desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info_desc.descriptorPool = desc_pool_;
    alloc_info_desc.descriptorSetCount = 1;
    alloc_info_desc.pSetLayouts = &desc_layout_;
    vkAllocateDescriptorSets(device_, &alloc_info_desc, &cache.desc_set);

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = cache.view;
    imageInfo.sampler = hwb_sampler_;

    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = cache.desc_set;
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_, 1, &descriptorWrite, 0, nullptr);

    hwb_cache_[hwb] = cache;
}
"""

source = re.sub(r'void Renderer::bind_hwb\(AHardwareBuffer\* hwb\) \{.*?(?=\nvoid Renderer::draw)', bind_hwb_new, source, flags=re.DOTALL)

# Also need to modify setup_hwb_resources to create a large enough descriptor pool, let's say 10
source = source.replace("poolInfo.maxSets = 1;", "poolInfo.maxSets = 10;")
source = source.replace("poolSize.descriptorCount = 1;", "poolSize.descriptorCount = 10;")

# Remove the single desc_set allocation from setup_hwb_resources
source = re.sub(r'VkDescriptorSetAllocateInfo allocInfo\{\};.*?vkAllocateDescriptorSets\(device_, &allocInfo, &desc_set_\);', '', source, flags=re.DOTALL)

# In draw, we need to bind the correct descriptor set from cache
draw_replace_target = r'vkCmdBindDescriptorSets\(cmd_buffers_\[image_index\], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &desc_set_, 0, nullptr\);'
draw_replacement = r'vkCmdBindDescriptorSets(cmd_buffers_[image_index], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &hwb_cache_[current_hwb_].desc_set, 0, nullptr);'
source = re.sub(draw_replace_target, draw_replacement, source)

# In cleanup_hwb_resources, clean up the cache
cleanup_new = """void Renderer::cleanup_hwb_resources() {
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    
    for (auto& pair : hwb_cache_) {
        vkDestroyImageView(device_, pair.second.view, nullptr);
        vkDestroyImage(device_, pair.second.image, nullptr);
        vkFreeMemory(device_, pair.second.memory, nullptr);
    }
    hwb_cache_.clear();

    if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    if (desc_layout_) vkDestroyDescriptorSetLayout(device_, desc_layout_, nullptr);
    
    if (hwb_sampler_) vkDestroySampler(device_, hwb_sampler_, nullptr);
    if (ycbcr_conversion_ && vkDestroySamplerYcbcrConversion_) vkDestroySamplerYcbcrConversion_(device_, ycbcr_conversion_, nullptr);
"""
source = re.sub(r'void Renderer::cleanup_hwb_resources\(\) \{.*?(?=    if \(current_hwb_\))', cleanup_new, source, flags=re.DOTALL)

with open(source_path, 'w') as f:
    f.write(source)
