#pragma once
// Compatibility shim. The compute context moved into the canvas library as
// vce::gpu::ComputeContext (libs/vulkan_canvas_engine/.../cpp/compute_context.hh).
// The ISP keeps referring to it as `isp::VkCompute` via this alias so the RAW
// video pipeline didn't need a mechanical rename across its many member types.
#include "compute_context.hh"

namespace isp {
using VkCompute = vce::gpu::ComputeContext;
}  // namespace isp
