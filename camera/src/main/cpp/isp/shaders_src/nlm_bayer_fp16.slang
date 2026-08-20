// fp16 build of the NLM pass. See nlm_bayer.slang for the algorithm; the only
// difference is that the groupshared tile and the patch SSD use half precision.
// Selected at load time when the device supports shaderFloat16.
#define NLM_HALF 1
#include "nlm_bayer.slang"
