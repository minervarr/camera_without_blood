#include "vk_compute.hh"

#include "../logger.hh"
#include <vector>
#include <cstring>

#undef LOG_TAG
#define LOG_TAG "VkCompute"

namespace isp {

bool VkCompute::init() {
    VkApplicationInfo app{};
    app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "raw_isp";
    app.apiVersion       = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
        LOGE("vkCreateInstance failed");
        return false;
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) { LOGE("no physical devices"); return false; }
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(instance_, &count, devs.data());
    phys_ = devs[0];

    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qcount, qprops.data());
    bool found = false;
    // Prefer a compute-capable family without graphics (a dedicated async
    // compute queue on Adreno/Mali); fall back to any compute family.
    for (uint32_t i = 0; i < qcount; ++i) {
        bool compute  = qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT;
        bool graphics = qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
        if (compute && !graphics) { queue_family_ = i; found = true; break; }
    }
    if (!found) {
        for (uint32_t i = 0; i < qcount; ++i) {
            if (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                queue_family_ = i; found = true; break;
            }
        }
    }
    if (!found) { LOGE("no compute queue family"); return false; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    // Optional zero-copy-RAW-input extensions. Enable when present (their deps
    // are Vulkan 1.1 core); absent -> ahb_supported_ stays false and the pipeline
    // uses the host-copy path. VK_EXT_queue_family_foreign just makes the AHB
    // ownership-transfer barrier exact (else VK_QUEUE_FAMILY_EXTERNAL is used).
    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ext_count, exts.data());
    auto has_ext = [&](const char* n) {
        for (auto& e : exts) if (std::strcmp(e.extensionName, n) == 0) return true;
        return false;
    };
    std::vector<const char*> enabled;
    if (has_ext(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)) {
        enabled.push_back(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
        ahb_supported_ = true;
        if (has_ext(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
            enabled.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
            foreign_queue_ext_ = true;
        }
    }

    VkDeviceCreateInfo dci{};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = static_cast<uint32_t>(enabled.size());
    dci.ppEnabledExtensionNames = enabled.empty() ? nullptr : enabled.data();
    if (vkCreateDevice(phys_, &dci, nullptr, &device_) != VK_SUCCESS) {
        LOGE("vkCreateDevice failed");
        return false;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    if (ahb_supported_) {
        pfn_ahb_props_ = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
            vkGetDeviceProcAddr(device_, "vkGetAndroidHardwareBufferPropertiesANDROID"));
        if (!pfn_ahb_props_) ahb_supported_ = false;
    }

    VkCommandPoolCreateInfo cpi{};
    cpi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = queue_family_;
    if (vkCreateCommandPool(device_, &cpi, nullptr, &cmd_pool_) != VK_SUCCESS) {
        LOGE("vkCreateCommandPool failed");
        return false;
    }

    LOGI("compute context ready (queue family %u, ahb-import %s)",
         queue_family_, ahb_supported_ ? "yes" : "no");
    return true;
}

void VkCompute::destroy() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        if (cmd_pool_) { vkDestroyCommandPool(device_, cmd_pool_, nullptr); cmd_pool_ = VK_NULL_HANDLE; }
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
    queue_ = VK_NULL_HANDLE;
    phys_  = VK_NULL_HANDLE;
}

uint32_t VkCompute::find_mem_type(uint32_t type_bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return UINT32_MAX;
}

bool VkCompute::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags props, Buffer& out) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bci, nullptr, &out.buf) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, out.buf, &req);
    uint32_t type = find_mem_type(req.memoryTypeBits, props);
    if (type == UINT32_MAX) { destroy_buffer(out); return false; }

    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &mai, nullptr, &out.mem) != VK_SUCCESS) {
        destroy_buffer(out);
        return false;
    }
    vkBindBufferMemory(device_, out.buf, out.mem, 0);
    out.size = size;

    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        if (vkMapMemory(device_, out.mem, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) {
            destroy_buffer(out);
            return false;
        }
    }
    return true;
}

void VkCompute::destroy_buffer(Buffer& b) {
    if (b.mapped) { vkUnmapMemory(device_, b.mem); b.mapped = nullptr; }
    if (b.buf)    { vkDestroyBuffer(device_, b.buf, nullptr); b.buf = VK_NULL_HANDLE; }
    if (b.mem)    { vkFreeMemory(device_, b.mem, nullptr); b.mem = VK_NULL_HANDLE; }
    b.size = 0;
}

bool VkCompute::create_image(uint32_t w, uint32_t h, VkFormat format,
                             VkImageUsageFlags usage, Image& out) {
    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = format;
    ici.extent        = { w, h, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = usage;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &out.img) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, out.img, &req);
    uint32_t type = find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) { destroy_image(out); return false; }

    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &mai, nullptr, &out.mem) != VK_SUCCESS) {
        destroy_image(out);
        return false;
    }
    vkBindImageMemory(device_, out.img, out.mem, 0);

    VkImageViewCreateInfo vci{};
    vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image    = out.img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = format;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(device_, &vci, nullptr, &out.view) != VK_SUCCESS) {
        destroy_image(out);
        return false;
    }
    return true;
}

void VkCompute::destroy_image(Image& i) {
    if (i.view) { vkDestroyImageView(device_, i.view, nullptr); i.view = VK_NULL_HANDLE; }
    if (i.img)  { vkDestroyImage(device_, i.img, nullptr); i.img = VK_NULL_HANDLE; }
    if (i.mem)  { vkFreeMemory(device_, i.mem, nullptr); i.mem = VK_NULL_HANDLE; }
}

bool VkCompute::import_ahb(AHardwareBuffer* ahb, Image& out) {
    if (!ahb_supported_ || !pfn_ahb_props_ || !ahb) return false;

    VkAndroidHardwareBufferFormatPropertiesANDROID fmt{};
    fmt.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID props{};
    props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    props.pNext = &fmt;
    if (pfn_ahb_props_(device_, ahb, &props) != VK_SUCCESS) return false;

    // Only the clean case: a plain single-channel 16-bit format with no external
    // format / YCbCr conversion (which would feed the integer debayer garbage).
    if (fmt.format != VK_FORMAT_R16_UINT || fmt.externalFormat != 0) {
        LOGE("AHB import: format 0x%x extFmt %llu not plain R16_UINT — host-copy fallback",
             fmt.format, static_cast<unsigned long long>(fmt.externalFormat));
        return false;
    }

    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);

    VkExternalMemoryImageCreateInfo ext{};
    ext.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext         = &ext;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_R16_UINT;
    ici.extent        = { desc.width, desc.height, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &out.img) != VK_SUCCESS) return false;

    // Imported memory must use one of props.memoryTypeBits.
    uint32_t type_index = 0; bool found = false;
    for (uint32_t i = 0; i < 32; ++i)
        if (props.memoryTypeBits & (1u << i)) { type_index = i; found = true; break; }
    if (!found) { destroy_image(out); return false; }

    VkImportAndroidHardwareBufferInfoANDROID import{};
    import.sType  = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    import.buffer = ahb;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = out.img;
    dedicated.pNext = &import;
    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext           = &dedicated;
    mai.allocationSize  = props.allocationSize;
    mai.memoryTypeIndex = type_index;
    if (vkAllocateMemory(device_, &mai, nullptr, &out.mem) != VK_SUCCESS) {
        destroy_image(out); return false;
    }
    if (vkBindImageMemory(device_, out.img, out.mem, 0) != VK_SUCCESS) {
        destroy_image(out); return false;
    }

    VkImageViewCreateInfo vci{};
    vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image    = out.img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = VK_FORMAT_R16_UINT;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(device_, &vci, nullptr, &out.view) != VK_SUCCESS) {
        destroy_image(out); return false;
    }
    return true;
}

VkShaderModule VkCompute::load_shader(AAssetManager* assets, const char* asset_path) {
    AAsset* asset = AAssetManager_open(assets, asset_path, AASSET_MODE_BUFFER);
    if (!asset) { LOGE("shader asset missing: %s", asset_path); return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = static_cast<size_t>(AAsset_getLength(asset));
    ci.pCode    = reinterpret_cast<const uint32_t*>(AAsset_getBuffer(asset));
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device_, &ci, nullptr, &mod);
    AAsset_close(asset);
    return mod;
}

VkCommandBuffer VkCompute::alloc_cmd() {
    VkCommandBufferAllocateInfo cai{};
    cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool        = cmd_pool_;
    cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cai, &cmd);
    return cmd;
}

VkFence VkCompute::create_fence(bool signaled) {
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
    VkFence f = VK_NULL_HANDLE;
    vkCreateFence(device_, &fci, nullptr, &f);
    return f;
}

} // namespace isp
