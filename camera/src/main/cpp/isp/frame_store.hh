#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace isp {

// Bounded in-RAM FIFO of RAW16 frames between the camera (producer, ~30 fps) and
// the offline NLM/ISP pipeline (consumer, slower-than-real-time). Frames are
// de-strided into pooled buffers so the 25 MB-per-frame malloc/free churn stays
// off the camera thread.
//
// This store NEVER touches disk. When develop falls behind the camera, push()
// drops the newest frame past the cap (the clip then records at the develop
// rate) rather than writing tens of GB of overflow to storage. The cap is sized
// to absorb jitter (~16 frames ≈ 0.4 GB at 4080x3060); the remaining backlog is
// drained after Stop, in the recorder's FINALIZING phase.
class FrameStore {
public:
    struct Frame {
        std::vector<uint8_t> data;   // tight RAW16 (row_bytes * height)
        int64_t              ts_ns = 0;
    };

    FrameStore() = default;
    ~FrameStore() { shutdown(); }

    bool init(int width, int height);

    // Producer (camera thread). De-strides the frame into a pooled buffer and
    // enqueues it. Returns false (and logs once) when the backlog cap is hit —
    // the sole drop path.
    bool push(const uint8_t* data, int stride_bytes, int64_t ts_ns);

    // Consumer (pipeline thread). Blocks until a frame is available; returns
    // false once the store is sealed AND fully drained. Moves the frame out;
    // recycle the buffer afterwards with reclaim().
    bool pop(Frame& out);

    // Return a consumed frame's buffer to the pool for reuse.
    void reclaim(Frame&& f);

    void seal();        // producer done; pop() drains the remainder then returns false
    void shutdown();    // wake any waiter, free the queue and the pool

    uint64_t pushed() const { return pushed_.load(std::memory_order_relaxed); }
    uint64_t popped() const { return popped_.load(std::memory_order_relaxed); }
    bool     sealed() const { return sealed_.load(std::memory_order_relaxed); }

private:
    std::vector<uint8_t> take_buffer();             // reuse or allocate a tight frame buffer
    void                 pool_return(std::vector<uint8_t>&& buf);

    // Backlog cap, in frames. Absorbs producer/consumer jitter; past it, push drops.
    static constexpr size_t kMaxQueuedFrames = 16;
    // Pooled buffers kept alive between frames (cap + the one in flight).
    static constexpr size_t kMaxPooledBuffers = kMaxQueuedFrames + 2;

    int    width_ = 0, height_ = 0;
    size_t row_bytes_   = 0;
    size_t frame_bytes_ = 0;

    std::mutex              mtx_;
    std::condition_variable cv_;
    std::deque<Frame>       ready_;

    std::vector<std::vector<uint8_t>> pool_;
    std::mutex                        pool_mtx_;

    std::atomic<bool> sealed_{false};
    std::atomic<bool> shutdown_{false};
    std::atomic<bool> drop_warned_{false};
    std::atomic<uint64_t> pushed_{0}, popped_{0};
};

} // namespace isp
