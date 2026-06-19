#include "ai_denoiser.hh"
#include <net.h>
#include <string.h>
#include <android/asset_manager.h>
#include <android/log.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AiDenoiser", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AiDenoiser", __VA_ARGS__)

namespace isp {

AiDenoiser::AiDenoiser() {
    ncnn::create_gpu_instance();
    net_ = new ncnn::Net();
}

AiDenoiser::~AiDenoiser() {
    if (net_) {
        delete static_cast<ncnn::Net*>(net_);
        net_ = nullptr;
    }
    ncnn::destroy_gpu_instance();
}

bool AiDenoiser::init(AAssetManager* assets) {
    ncnn::Net* net = static_cast<ncnn::Net*>(net_);
    net->opt.lightmode = true;
    net->opt.num_threads = 4;
    
    if (ncnn::get_gpu_count() > 0) {
        net->opt.use_vulkan_compute = true;
        net->set_vulkan_device(ncnn::get_gpu_device(0));
    } else {
        net->opt.use_vulkan_compute = false;
        LOGI("No Vulkan device found for NCNN, falling back to CPU.");
    }
    
    if (net->load_param(assets, "dncnn.param") != 0 ||
        net->load_model(assets, "dncnn.bin") != 0) {
        LOGE("AI Denoiser models not found in assets, skipping AI pass.");
        return false;
    }
    LOGI("AI Denoiser loaded successfully. Vulkan: %d", net->opt.use_vulkan_compute);
    return true;
}

bool AiDenoiser::run(uint16_t* bayer_data, int width, int height) {
    ncnn::Net* net = static_cast<ncnn::Net*>(net_);
    if (!net || net->blobs().empty()) return false;
    
    const std::vector<int>& in_indexes = net->input_indexes();
    const std::vector<int>& out_indexes = net->output_indexes();
    if (in_indexes.empty() || out_indexes.empty()) return false;
    
    ncnn::Mat in_mat_f32(width, height, 1);
    float* in_ptr = in_mat_f32.row(0);
    
    // Fast cast to float (Scaling is baked into the AI weights at compile-time!)
    #pragma omp parallel for
    for (int i = 0; i < width * height; i++) {
        in_ptr[i] = static_cast<float>(bayer_data[i]);
    }

    ncnn::Extractor ex = net->create_extractor();
    ex.input(in_indexes[0], in_mat_f32);
    
    ncnn::Mat out_mat;
    ex.extract(out_indexes[0], out_mat);
    
    if (!out_mat.empty() && out_mat.w == width && out_mat.h == height) {
        float* out_ptr = out_mat.row(0);
        
        // Fast cast back to 16-bit integer
        #pragma omp parallel for
        for (int i = 0; i < width * height; i++) {
            float val = out_ptr[i];
            if (val < 0.0f) val = 0.0f;
            if (val > 65535.0f) val = 65535.0f;
            bayer_data[i] = static_cast<uint16_t>(val);
        }
        return true;
    }
    return false;
}

} // namespace isp
