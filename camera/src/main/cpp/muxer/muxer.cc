#include "muxer.hh"

#include <ebml/StdIOCallback.h>
#include <ebml/EbmlHead.h>
#include <ebml/EbmlFloat.h>

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

// 100 us tick. All timestamps fed to libmatroska are absolute nanoseconds from
// the first frame; the library divides by this scale to get block timecodes.
//
// This was 1 ms, which cannot represent a 33.3333 ms frame period at all — every
// video timestamp was rounded to the nearest millisecond, so a nominal 30 fps
// stream was written as an alternating 33/33/34 ms pattern. 100 us cuts that
// quantisation tenfold.
//
// HARD BOUND, do not exceed. A Matroska block timecode is a SIGNED 16-BIT offset
// from its cluster's timestamp, so a cluster can only span 32767 ticks. The
// static_assert below is what keeps a future MAX_CLUSTER_NS change from silently
// overflowing it. At 1 s clusters: 100 us gives 10 000 ticks (safe), but 10 us
// would give 100 000 and would NOT fit — going finer requires shorter clusters.
static constexpr uint64_t TS_SCALE = 100000;
// Cap cluster span. 1s (was 2s) halves each flush_cluster() disk write (~15MB vs
// ~30MB at 120Mbps), so the periodic storage/memory-bus burst is gentler and is
// less likely to starve the ISP GPU into a ~1Hz stall.
static constexpr int64_t  MAX_CLUSTER_NS = 1'000'000'000;  // 1 s
static_assert(MAX_CLUSTER_NS / static_cast<int64_t>(TS_SCALE) < 32767,
              "cluster span overflows the signed 16-bit block timecode");
static const EbmlElement::ShouldWrite kWriteDefault = EbmlElement::WriteSkipDefault;
// Force-write even default-valued elements. Needed for the EBML head: DocType's
// libebml default is "matroska", so WriteSkipDefault DROPS the DocType element
// entirely — producing a file with doctype "(none)" that strict players/editors
// reject (no timeline). WriteAll emits the mandatory DocType.
static const EbmlElement::ShouldWrite kWriteAll = EbmlElement::WriteAll;

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
    int64_t last_rel_ns = 0;          // newest block timestamp (for Duration)
    uint64_t segment_pos = 0;         // file offset of the Segment element head
    uint64_t info_pos = 0;            // file offset of the rendered Info element
    uint64_t tracks_pos = 0;          // file offset of the rendered Tracks element

    // Measured video frame cadence, for the DefaultDuration patched in at close().
    // Only the first and last video timestamp and the count are needed: the mean
    // interval over the whole clip is far steadier than any per-frame delta, and
    // it is exactly what a player should be told the constant frame rate is.
    int64_t  vid_first_ns = -1;
    int64_t  vid_last_ns  = -1;
    uint64_t vid_frames   = 0;
    uint64_t nominal_frame_ns = 0;    // 1e9 / fps, the open()-time seed

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
        // WriteAll (not kWriteDefault): otherwise DocType="matroska" — being the
        // libebml default — is skipped, leaving the file with no DocType.
        head.Render(*impl_->file, kWriteAll);

        impl_->segment_pos = impl_->file->getFilePointer();
        impl_->segment.WriteHead(*impl_->file, 5, kWriteDefault);

        // Segment info. Duration is a fixed-size 8-byte float placeholder so
        // close() can seek back and patch the real value in place — players
        // then show the correct length immediately instead of estimating it
        // while reading (the "duration keeps growing" symptom in mpv).
        KaxInfo& info = GetChild<KaxInfo>(impl_->segment);
        GetChild<KaxTimestampScale>(info).SetValue(TS_SCALE);
        KaxDuration& dur = GetChild<KaxDuration>(info);
        dur.SetPrecision(EbmlFloat::FLOAT_64);
        dur.SetValue(1.0);
        GetChild<KaxMuxingApp>(info).SetValue(UTFstring{L"camera_without_blood"});
        GetChild<KaxWritingApp>(info).SetValue(UTFstring{L"camera_without_blood"});
        impl_->info_pos = impl_->file->getFilePointer();
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

            if (vcfg.color != Color::SDR) {
                KaxVideoColour& c = GetChild<KaxVideoColour>(v);
                GetChild<KaxVideoBitsPerChannel>(c).SetValue(10);
                GetChild<KaxVideoColourPrimaries>(c).SetValue(MATROSKA_VIDEO_PRIMARIES_BT2020);
                GetChild<KaxVideoColourMatrix>(c).SetValue(MATROSKA_VIDEO_MATRIXCOEFFICIENTS_BT2020_NCL);
                if (vcfg.color == Color::HDR_PQ_FULL) {
                    GetChild<KaxVideoColourTransferCharacter>(c).SetValue(MATROSKA_TRANSFER_BT2100_PQ);
                    GetChild<KaxVideoColourRange>(c).SetValue(MATROSKA_VIDEO_RANGE_FULL_RANGE);
                } else {
                    GetChild<KaxVideoColourTransferCharacter>(c).SetValue(MATROSKA_TRANSFER_ARIB_STD_B67);
                    GetChild<KaxVideoColourRange>(c).SetValue(MATROSKA_VIDEO_RANGE_BROADCAST_RANGE);
                }
            }
            // DefaultDuration: the field a player reports as the container frame
            // rate. Without it the demuxer has to ESTIMATE the rate from the first
            // few packet timestamps, and at ~240 Mbps ffmpeg's default 5 MB probe
            // covers roughly five frames — estimating from five timestamps is how
            // a 30 fps clip gets reported as 29.92.
            //
            // Seeded from the nominal fps here and patched at close() with the
            // rate the sensor actually delivered, using the same fixed-size
            // placeholder trick as KaxDuration above. SetDefaultSize(8) pins it to
            // 8 bytes so the re-render is byte-identical in size.
            impl_->nominal_frame_ns =
                vcfg.fps > 0 ? static_cast<uint64_t>(1'000'000'000LL / vcfg.fps) : 0;
            if (impl_->nominal_frame_ns > 0) {
                KaxTrackDefaultDuration& dd = GetChild<KaxTrackDefaultDuration>(t);
                dd.SetDefaultSize(8);
                dd.SetValue(impl_->nominal_frame_ns);
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

        impl_->tracks_pos = impl_->file->getFilePointer();
        impl_->tracks->Render(*impl_->file, kWriteDefault);
        impl_->cues.SetGlobalTimestampScale(TS_SCALE);

        open_ = true;
        LOGI("Muxer open: %s (video=%d audio=%d color=%d)",
             path.c_str(), impl_->have_video, impl_->have_audio,
             static_cast<int>(vcfg.color));
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
    // impl_ (and impl_->mtx with it) deliberately outlives close(): write_video/
    // write_audio test open_ and then lock impl_->mtx, so freeing the Impl here
    // would hand a concurrent writer a destroyed mutex. Ownership stays with the
    // Muxer — the destructor and a subsequent open() release it.
    std::lock_guard<std::mutex> lock(impl_->mtx);
    try {
        impl_->flush_cluster();
        if (impl_->have_video || impl_->have_audio)
            impl_->cues.Render(*impl_->file, kWriteDefault);
        // Patch the real duration over the placeholder written at open().
        // KaxDuration is a fixed 8-byte float, so the re-rendered Info element
        // is byte-identical in size.
        // start_ns >= 0 means at least one block was written, so the placeholder
        // must be replaced even for a clip whose blocks all land at rel == 0
        // (a single-frame capture) — otherwise the bogus 1.0 s placeholder ships.
        if (impl_->start_ns >= 0) {
            KaxInfo& info = GetChild<KaxInfo>(impl_->segment);
            GetChild<KaxDuration>(info).SetValue(
                static_cast<double>(impl_->last_rel_ns) / TS_SCALE);
            uint64_t end = impl_->file->getFilePointer();
            impl_->file->setFilePointer(static_cast<int64_t>(impl_->info_pos));
            info.Render(*impl_->file, kWriteDefault);
            impl_->file->setFilePointer(static_cast<int64_t>(end));
        }
        // Patch DefaultDuration with the rate actually delivered. The mean
        // interval across the whole clip is the honest constant frame rate: it
        // is what the hardware produced, so declaring it cannot drift the video
        // against the audio the way snapping timestamps to a nominal 30.000 grid
        // would (~1.6 s per 10 min if the sensor is really 29.92 Hz).
        //
        // Needs >= 2 frames to have an interval at all; below that the nominal
        // seed written at open() stands. The sanity window rejects a wild value
        // from a clock-domain glitch rather than shipping a broken rate.
        if (impl_->have_video && impl_->nominal_frame_ns > 0 && impl_->vid_frames >= 2) {
            const int64_t span = impl_->vid_last_ns - impl_->vid_first_ns;
            const uint64_t measured =
                static_cast<uint64_t>(span / static_cast<int64_t>(impl_->vid_frames - 1));
            const uint64_t lo = impl_->nominal_frame_ns / 2;
            const uint64_t hi = impl_->nominal_frame_ns * 2;
            if (span > 0 && measured >= lo && measured <= hi) {
                GetChild<KaxTrackDefaultDuration>(*impl_->video).SetValue(measured);
                uint64_t end = impl_->file->getFilePointer();
                impl_->file->setFilePointer(static_cast<int64_t>(impl_->tracks_pos));
                impl_->tracks->Render(*impl_->file, kWriteDefault);
                impl_->file->setFilePointer(static_cast<int64_t>(end));
                LOGI("Video DefaultDuration: %llu ns (%.3f fps) over %llu frames",
                     static_cast<unsigned long long>(measured),
                     1e9 / static_cast<double>(measured),
                     static_cast<unsigned long long>(impl_->vid_frames));
            } else {
                LOGE("Measured frame interval %llu ns out of range — keeping nominal %llu ns",
                     static_cast<unsigned long long>(measured),
                     static_cast<unsigned long long>(impl_->nominal_frame_ns));
            }
        }
        // Patch the real Segment size over the 5-byte placeholder reserved by
        // WriteHead() at open(). This is NOT cosmetic: libebml renders that
        // placeholder as a *zero* size (08 00 00 00 00), not the all-ones
        // unknown-size marker (08 FF FF FF FF), so the file declares that the
        // Segment contains nothing. ffmpeg/mpv/VLC ignore the field and read to
        // EOF, but a strict parser — Android's MatroskaExtractor, hence the
        // Samsung stock player — believes it, finds no tracks, and reports
        // "unsupported file type". Device-verified: patching these 5 bytes on an
        // otherwise untouched recording made it play.
        {
            const uint64_t end = impl_->file->getFilePointer();
            // Head = the 4-byte Segment ID + the 5 size bytes reserved by
            // WriteHead(file, 5). EbmlElement::HeadSize() is private in this
            // libebml, so the two are spelled out rather than queried.
            const uint64_t body = end - impl_->segment_pos - (4 + 5);
            if (impl_->segment.ForceSize(body)) {
                impl_->segment.OverwriteHead(*impl_->file);
                impl_->file->setFilePointer(static_cast<int64_t>(end));
                LOGI("Segment size patched: %llu bytes",
                     static_cast<unsigned long long>(body));
            } else {
                LOGE("Segment size %llu does not fit the reserved 5-byte field",
                     static_cast<unsigned long long>(body));
            }
        }
    } catch (std::exception& e) {
        LOGE("Muxer close render error: %s", e.what());
    }
    if (impl_->file) { impl_->file->close(); delete impl_->file; impl_->file = nullptr; }
    open_ = false;
    LOGI("Muxer closed");
}

void Muxer::write_video(const uint8_t* data, int size, int64_t timestamp_ns, bool keyframe) {
    if (!open_ || !data || size <= 0) return;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->have_video) return;

    if (impl_->start_ns < 0) impl_->start_ns = timestamp_ns;
    int64_t rel = timestamp_ns - impl_->start_ns;
    if (rel < 0) rel = 0;
    if (rel > impl_->last_rel_ns) impl_->last_rel_ns = rel;

    // Cadence for DefaultDuration. Uses the raw incoming timestamp, not `rel`,
    // so the 0-clamp above cannot bias the measured interval.
    if (impl_->vid_first_ns < 0) impl_->vid_first_ns = timestamp_ns;
    if (timestamp_ns > impl_->vid_last_ns) impl_->vid_last_ns = timestamp_ns;
    ++impl_->vid_frames;

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
    if (rel > impl_->last_rel_ns) impl_->last_rel_ns = rel;

    // NOTE: no DefaultDuration cadence accounting here. vid_* measures the VIDEO
    // track's frame interval only; counting audio blocks into it inflated the
    // frame count by the number of FLAC packets (measured: 7154 video + 2982
    // audio = 10136) and made the file declare 42.5 fps for a true 30.0 fps
    // stream — the bogus "specified" rate players showed.
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
