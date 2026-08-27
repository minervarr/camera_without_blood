// fp16 build of the binned-RGB NLM pass. See nlm_rgb.slang for the algorithm;
// the only difference is that the groupshared tile and the patch SSD use half
// precision. Selected at load time when the device supports shaderFloat16.
#define NLM_HALF 1
#include "nlm_rgb.slang"
