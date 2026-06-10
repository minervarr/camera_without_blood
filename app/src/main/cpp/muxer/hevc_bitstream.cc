#include "hevc_bitstream.hh"

#include <cstring>

namespace hevc {

namespace {

struct Nal { const uint8_t* data; int size; };

// Splits an Annex B buffer into NAL units (strips 3- and 4-byte start codes).
std::vector<Nal> split_nals(const uint8_t* p, int len) {
    std::vector<Nal> nals;
    int i = 0;
    // find first start code
    auto is_start = [&](int j, int& sc) -> bool {
        if (j + 3 < len && p[j] == 0 && p[j+1] == 0 && p[j+2] == 0 && p[j+3] == 1) { sc = 4; return true; }
        if (j + 2 < len && p[j] == 0 && p[j+1] == 0 && p[j+2] == 1) { sc = 3; return true; }
        return false;
    };
    int sc = 0;
    while (i < len && !is_start(i, sc)) ++i;
    while (i < len) {
        i += sc;                     // skip start code
        int start = i;
        int next = i;
        int nsc = 0;
        while (next < len && !is_start(next, nsc)) ++next;
        if (next > start) nals.push_back({p + start, next - start});
        i = next;
        sc = nsc;
    }
    return nals;
}

void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x & 0xFF));
}
void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x & 0xFF));
}

} // namespace

bool build_hvcc(const uint8_t* csd_annexb, int len, std::vector<uint8_t>& out) {
    auto nals = split_nals(csd_annexb, len);
    std::vector<Nal> vps, sps, pps;
    for (auto& n : nals) {
        if (n.size < 2) continue;
        int type = (n.data[0] >> 1) & 0x3F;
        if      (type == 32) vps.push_back(n);   // VPS
        else if (type == 33) sps.push_back(n);   // SPS
        else if (type == 34) pps.push_back(n);   // PPS
    }
    if (sps.empty() || pps.empty()) return false;

    // Profile/tier/level fields are filled with Main10 defaults; decoders
    // (ffmpeg/mpv) re-parse the SPS in the arrays for the authoritative values.
    out.clear();
    out.push_back(1);                 // configurationVersion
    out.push_back(0x02);              // profile_space(0)|tier(0)|profile_idc(2 = Main10)
    put_u32(out, 0x60000000);         // general_profile_compatibility_flags (bit for idc 2)
    for (int i = 0; i < 6; ++i) out.push_back(0x00);  // constraint indicator flags
    out.push_back(153);               // general_level_idc (level 5.1)
    put_u16(out, 0xF000);             // reserved(4=1111) | min_spatial_segmentation_idc(0)
    out.push_back(0xFC);              // reserved(6) | parallelismType(0)
    out.push_back(0xFC | 0x01);       // reserved(6) | chromaFormat(1 = 4:2:0)
    out.push_back(0xF8 | 0x02);       // reserved(5) | bitDepthLumaMinus8(2 = 10-bit)
    out.push_back(0xF8 | 0x02);       // reserved(5) | bitDepthChromaMinus8(2)
    put_u16(out, 0);                  // avgFrameRate
    // constantFrameRate(0)|numTemporalLayers(1)|temporalIdNested(0)|lengthSizeMinusOne(3)
    out.push_back(uint8_t((1 << 3) | 0x03));
    out.push_back(3);                 // numOfArrays (VPS, SPS, PPS)

    auto emit_array = [&](int nal_type, std::vector<Nal>& arr) {
        out.push_back(uint8_t(0x80 | (nal_type & 0x3F)));  // array_completeness | type
        put_u16(out, uint16_t(arr.size()));
        for (auto& n : arr) {
            put_u16(out, uint16_t(n.size));
            out.insert(out.end(), n.data, n.data + n.size);
        }
    };
    emit_array(32, vps);
    emit_array(33, sps);
    emit_array(34, pps);
    return true;
}

void annexb_to_length_prefixed(const uint8_t* in, int len, std::vector<uint8_t>& out) {
    out.clear();
    for (auto& n : split_nals(in, len)) {
        put_u32(out, uint32_t(n.size));
        out.insert(out.end(), n.data, n.data + n.size);
    }
}

} // namespace hevc
