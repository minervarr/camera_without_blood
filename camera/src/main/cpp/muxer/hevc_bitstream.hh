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

// True if the access unit contains an IRAP VCL NAL (IDR / CRA). MediaCodec on
// this device sets BUFFER_FLAG_KEY_FRAME on every HEVC buffer, including
// TRAIL_R P-frames; the container key flag and muxer cluster split must follow
// the bitstream, not that flag.
bool access_unit_is_irap(const uint8_t* annexb, int len);

} // namespace hevc
