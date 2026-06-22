#pragma once

#include <cstdint>

class AAssetManager;

namespace isp {

class AiDenoiser {
public:
    AiDenoiser();
    ~AiDenoiser();
    
    bool init(AAssetManager* assets);
    bool run(uint16_t* bayer_data, int width, int height);
    
private:
    void* net_ = nullptr;
};

} // namespace isp
