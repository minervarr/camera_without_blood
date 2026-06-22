#pragma once

#include <cstdint>
#include <vector>

// Helpers to bridge MediaCodec's HEVC output (Annex B / start-code byte stream)
// to what Matroska expects: an hvcC configuration record for CodecPrivate, and
// length-prefixed NAL units for each frame.
namespace hevc {

// Builds an HEVCDecoderConfigurationRecord (hvcC) from a csd buffer containing
// VPS/SPS/PPS NAL units in Annex B form. Returns false if no parameter sets
// were found.
bool build_hvcc(const uint8_t* csd_annexb, int len, std::vector<uint8_t>& out);

// Converts an Annex B access unit to length-prefixed (4-byte) NAL units.
void annexb_to_length_prefixed(const uint8_t* in, int len, std::vector<uint8_t>& out);

} // namespace hevc
