#include "muxer.hh"

#include <ebml/StdIOCallback.h>
#include <ebml/EbmlHead.h>

#include <matroska/KaxSegment.h>
#include <matroska/KaxTracks.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxBlock.h>
#include <matroska/KaxCues.h>
#include <matroska/KaxSemantic.h>
#include <matroska/KaxTypes.h>

#include "../logger.hh"
#include <mutex>
#include <cstdlib>

#undef LOG_TAG
#define LOG_TAG "Muxer"

using namespace libebml;
using namespace libmatroska;

namespace mux {

// 1 ms tick. All timestamps fed to libmatroska are absolute nanoseconds from
// the first frame; the library divides by this scale to get block timecodes.
static constexpr uint64_t TS_SCALE = 1000000;
// Cap cluster span so seeking/streaming stays sane even without keyframes.
static constexpr int64_t  MAX_CLUSTER_NS = 2'000'000'000;  // 2 s
static const EbmlElement::ShouldWrite kWriteDefault = EbmlElement::WriteSkipDefault;

struct Muxer::Impl {
    std::mutex mtx;

    StdIOCallback* file = nullptr;
    KaxSegment     segment;
    KaxTracks*     tracks = nullptr;
    KaxTrackEntry* video  = nullptr;
    KaxTrackEntry* audio  = nullptr;

    KaxCues     cues;
    KaxCluster* cluster      = nullptr;
    int64_t     cluster_base = 0;     // ns (relative to start_ns) of cluster start

    int64_t start_ns = -1;            // absolute ts of first frame seen

    bool have_video = false;
    bool have_audio = false;

    void start_cluster(int64_t base_ns) {
        cluster = new KaxCluster();
        cluster->SetParent(segment);
        cluster->SetPreviousTimestamp(static_cast<uint64_t>(base_ns), TS_SCALE);
        cluster->EnableChecksum();
        cluster_base = base_ns;
    }

    void flush_cluster() {
        if (!cluster) return;
        cluster->Render(*file, cues, kWriteDefault);
        cluster->ReleaseFrames();
        delete cluster;
        cluster = nullptr;
    }
};

Muxer::Muxer()  = default;
Muxer::~Muxer() { close(); }

bool Muxer::open(const std::string& path,
                 const VideoTrackConfig& vcfg,
                 const AudioTrackConfig& acfg) {
    impl_ = new Impl();
    try {
        impl_->file = new StdIOCallback(path.c_str(), MODE_CREATE);

        // EBML header — matroska v4 (needed for the Colour/HDR element).
        EbmlHead head;
        GetChild<EDocType>(head).SetValue("matroska");
        GetChild<EDocTypeVersion>(head).SetValue(4);
        GetChild<EDocTypeReadVersion>(head).SetValue(2);
        head.Render(*impl_->file, kWriteDefault);

        impl_->segment.WriteHead(*impl_->file, 5, kWriteDefault);

        // Segment info.
        KaxInfo& info = GetChild<KaxInfo>(impl_->segment);
        GetChild<KaxTimestampScale>(info).SetValue(TS_SCALE);
        GetChild<KaxMuxingApp>(info).SetValue(UTFstring{L"camera_without_blood"});
        GetChild<KaxWritingApp>(info).SetValue(UTFstring{L"camera_without_blood"});
        info.Render(*impl_->file, kWriteDefault);

        impl_->tracks = &GetChild<KaxTracks>(impl_->segment);

        // ── Video track (only if decoder config is present) ──────────────────
        if (vcfg.private_data && vcfg.private_data_size > 0) {
            KaxTrackEntry& t = GetChild<KaxTrackEntry>(*impl_->tracks);
            t.SetGlobalTimestampScale(TS_SCALE);
            GetChild<KaxTrackNumber>(t).SetValue(1);
            GetChild<KaxTrackUID>(t).SetValue(1);
            GetChild<KaxTrackType>(t).SetValue(track_video);
            GetChild<KaxCodecID>(t).SetValue(vcfg.codec_id);
            GetChild<KaxCodecPrivate>(t).CopyBuffer(
                reinterpret_cast<const binary*>(vcfg.private_data), vcfg.private_data_size);
            t.EnableLacing(false);

            KaxTrackVideo& v = GetChild<KaxTrackVideo>(t);
            GetChild<KaxVideoPixelWidth>(v).SetValue(vcfg.width);
            GetChild<KaxVideoPixelHeight>(v).SetValue(vcfg.height);

            if (vcfg.hdr_hlg) {
                KaxVideoColour& c = GetChild<KaxVideoColour>(v);
                GetChild<KaxVideoBitsPerChannel>(c).SetValue(10);
                GetChild<KaxVideoColourPrimaries>(c).SetValue(MATROSKA_VIDEO_PRIMARIES_BT2020);
                GetChild<KaxVideoColourTransferCharacter>(c).SetValue(MATROSKA_TRANSFER_ARIB_STD_B67);
                GetChild<KaxVideoColourMatrix>(c).SetValue(MATROSKA_VIDEO_MATRIXCOEFFICIENTS_BT2020_NCL);
                GetChild<KaxVideoColourRange>(c).SetValue(MATROSKA_VIDEO_RANGE_BROADCAST_RANGE);
            }
            impl_->video = &t;
            impl_->have_video = true;
        }

        // ── Audio track (only if codec config is present) ────────────────────
        if (acfg.private_data && acfg.private_data_size > 0) {
            KaxTrackEntry& t = impl_->have_video
                ? GetNextChild<KaxTrackEntry>(*impl_->tracks, *impl_->video)
                : GetChild<KaxTrackEntry>(*impl_->tracks);
            t.SetGlobalTimestampScale(TS_SCALE);
            GetChild<KaxTrackNumber>(t).SetValue(2);
            GetChild<KaxTrackUID>(t).SetValue(2);
            GetChild<KaxTrackType>(t).SetValue(track_audio);
            GetChild<KaxCodecID>(t).SetValue(acfg.codec_id);
            GetChild<KaxCodecPrivate>(t).CopyBuffer(
                reinterpret_cast<const binary*>(acfg.private_data), acfg.private_data_size);
            t.EnableLacing(false);

            KaxTrackAudio& a = GetChild<KaxTrackAudio>(t);
            GetChild<KaxAudioSamplingFreq>(a).SetValue(static_cast<double>(acfg.sample_rate));
            GetChild<KaxAudioChannels>(a).SetValue(acfg.channels);
            if (acfg.bit_depth > 0)
                GetChild<KaxAudioBitDepth>(a).SetValue(acfg.bit_depth);
            impl_->audio = &t;
            impl_->have_audio = true;
        }

        impl_->tracks->Render(*impl_->file, kWriteDefault);
        impl_->cues.SetGlobalTimestampScale(TS_SCALE);

        open_ = true;
        LOGI("Muxer open: %s (video=%d audio=%d hdr=%d)",
             path.c_str(), impl_->have_video, impl_->have_audio, vcfg.hdr_hlg);
        return true;
    } catch (std::exception& e) {
        LOGE("Muxer open failed: %s", e.what());
        delete impl_->file; impl_->file = nullptr;
        delete impl_; impl_ = nullptr;
        return false;
    }
}

void Muxer::close() {
    if (!open_) return;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    try {
        impl_->flush_cluster();
        if (impl_->have_video || impl_->have_audio)
            impl_->cues.Render(*impl_->file, kWriteDefault);
        // Segment keeps an unknown size (valid for streaming MKV); players read
        // it linearly without needing the final SeekHead.
    } catch (std::exception& e) {
        LOGE("Muxer close render error: %s", e.what());
    }
    if (impl_->file) { impl_->file->close(); delete impl_->file; impl_->file = nullptr; }
    open_ = false;
    delete impl_; impl_ = nullptr;
    LOGI("Muxer closed");
}

void Muxer::write_video(const uint8_t* data, int size, int64_t timestamp_ns, bool keyframe) {
    if (!open_ || !data || size <= 0) return;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->have_video) return;

    if (impl_->start_ns < 0) impl_->start_ns = timestamp_ns;
    int64_t rel = timestamp_ns - impl_->start_ns;
    if (rel < 0) rel = 0;

    // New cluster on each keyframe, when the current one grows too long, or when the
    // block would fall outside the cluster's 16-bit ms timecode range in EITHER
    // direction (|rel - base| > MAX_CLUSTER_NS) — the latter guards against a
    // libmatroska abort if audio/video ever land in mismatched clock domains.
    if (!impl_->cluster ||
        (keyframe && impl_->cluster_base != rel) ||
        llabs(rel - impl_->cluster_base) > MAX_CLUSTER_NS) {
        impl_->flush_cluster();
        impl_->start_cluster(rel);
    }

    auto* db = new DataBuffer(const_cast<binary*>(reinterpret_cast<const binary*>(data)),
                              static_cast<uint32_t>(size), nullptr, /*internal copy*/ true);
    KaxBlockGroup* blk = nullptr;
    impl_->cluster->AddFrame(*impl_->video, static_cast<uint64_t>(rel), *db, blk, LACING_NONE);
}

void Muxer::write_audio(const uint8_t* data, int size, int64_t timestamp_ns) {
    if (!open_ || !data || size <= 0) return;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->have_audio) return;

    if (impl_->start_ns < 0) impl_->start_ns = timestamp_ns;
    int64_t rel = timestamp_ns - impl_->start_ns;
    if (rel < 0) rel = 0;

    if (!impl_->cluster || llabs(rel - impl_->cluster_base) > MAX_CLUSTER_NS) {
        impl_->flush_cluster();
        impl_->start_cluster(rel);
    }

    auto* db = new DataBuffer(const_cast<binary*>(reinterpret_cast<const binary*>(data)),
                              static_cast<uint32_t>(size), nullptr, /*internal copy*/ true);
    KaxBlockGroup* blk = nullptr;
    impl_->cluster->AddFrame(*impl_->audio, static_cast<uint64_t>(rel), *db, blk, LACING_NONE);
}

} // namespace mux
