#pragma once

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR   // expose vkGetAndroidHardwareBufferProperties etc.
#endif
#include <vulkan/vulkan.h>
#include <android/asset_manager.h>
#include <android/hardware_buffer.h>
#include <cstdint>

// Minimal compute-only Vulkan context for the RAW ISP. Deliberately separate
// from the canvas engine's Renderer: no swapchain, no surface, no YCbCr — and
// its own queue, so heavy ISP dispatches never contend with preview drawing.
namespace isp {

class VkCompute {
public:
    VkCompute()  = default;
    ~VkCompute() { destroy(); }

    bool init();
    void destroy();

    bool ok() const { return device_ != VK_NULL_HANDLE; }

    VkDevice        device() const { return device_; }
    VkQueue         queue()  const { return queue_; }
    uint32_t        queue_family() const { return queue_family_; }
    VkCommandPool   pool()   const { return cmd_pool_; }

    // Host-visible-or-device buffer. `mapped` is non-null only for
    // host-visible allocations (persistently mapped).
    struct Buffer {
        VkBuffer       buf  = VK_NULL_HANDLE;
        VkDeviceMemory mem  = VK_NULL_HANDLE;
        void*          mapped = nullptr;
        VkDeviceSize   size = 0;
    };
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props, Buffer& out);
    void destroy_buffer(Buffer& b);

    // 2D single-mip device-local image (for the R16_UINT Bayer input).
    struct Image {
        VkImage        img  = VK_NULL_HANDLE;
        VkDeviceMemory mem  = VK_NULL_HANDLE;
        VkImageView    view = VK_NULL_HANDLE;
    };
    bool create_image(uint32_t w, uint32_t h, VkFormat format,
                      VkImageUsageFlags usage, Image& out);
    void destroy_image(Image& i);

    // Imports a camera AHardwareBuffer (RAW16) as a sampled R16_UINT image with
    // zero copy. Returns false if AHB import is unsupported or the buffer isn't a
    // plain R16_UINT (e.g. implementation-defined / YCbCr — which would feed the
    // integer debayer garbage), so the caller can fall back to a host copy. The
    // returned Image owns the import; destroy_image frees it (releasing the AHB
    // reference the import took).
    bool import_ahb(AHardwareBuffer* ahb, Image& out);
    bool ahb_import_supported() const { return ahb_supported_; }
    // Queue family the camera/gralloc "owns" imported buffers as, for ownership
    // transfer barriers: FOREIGN if that extension is enabled, else EXTERNAL.
    uint32_t external_queue_family() const {
        return foreign_queue_ext_ ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
    }

    VkShaderModule  load_shader(AAssetManager* assets, const char* asset_path);
    VkCommandBuffer alloc_cmd();
    VkFence         create_fence(bool signaled);

private:
    uint32_t find_mem_type(uint32_t type_bits, VkMemoryPropertyFlags props);

    VkInstance       instance_  = VK_NULL_HANDLE;
    VkPhysicalDevice phys_      = VK_NULL_HANDLE;
    VkDevice         device_    = VK_NULL_HANDLE;
    VkQueue          queue_     = VK_NULL_HANDLE;
    uint32_t         queue_family_ = 0;
    VkCommandPool    cmd_pool_  = VK_NULL_HANDLE;

    // AHB-import (zero-copy RAW input) capability, resolved at init.
    bool ahb_supported_     = false;
    bool foreign_queue_ext_ = false;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID pfn_ahb_props_ = nullptr;
};

} // namespace isp
