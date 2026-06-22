#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace isp {

// Never-drop FIFO of RAW16 frames between the camera (producer, ~30 fps) and the
// offline NLM pipeline (consumer, slower-than-real-time). The oldest pending
// frames live in a RAM budget; once that's exceeded the overflow spills to a
// sequence of fixed-size chunk files on disk, written/read by a dedicated I/O
// thread so the camera thread never blocks on disk. Strict FIFO order is
// preserved across RAM and disk. Short clips stay entirely in RAM (no disk
// touched); chunk files are deleted as they are consumed, so disk use tracks the
// live backlog — not the whole clip.
//
// Order invariant:  ready_ (oldest, RAM) -> disk chunks -> to_spill_ (newest, RAM)
class FrameStore {
public:
    struct Frame {
        std::vector<uint8_t> data;   // tight RAW16 (row_bytes * height)
        int64_t              ts_ns = 0;
    };

    FrameStore() = default;
    ~FrameStore() { shutdown(); }

    // Frame geometry, RAM budget (bytes), and the directory for spill chunks.
    bool init(int width, int height, size_t ram_budget_bytes, std::string spill_dir);

    // Producer (camera thread). De-strides the frame into a pooled buffer and
    // enqueues it. Never blocks on disk. Returns false (and logs once) only if the
    // hard RAM cap is hit because disk can't keep up — the sole drop path.
    bool push(const uint8_t* data, int stride_bytes, int64_t ts_ns);

    // Consumer (pipeline thread). Blocks until a frame is available; returns false
    // once the store is sealed AND fully drained. Moves the frame out; recycle the
    // buffer afterwards with reclaim().
    bool pop(Frame& out);

    // Return a consumed frame's buffer to the pool for reuse (avoids 25 MB
    // malloc/free churn at 30 fps).
    void reclaim(Frame&& f);

    void seal();        // producer done; pop() drains the remainder then returns false
    void shutdown();    // join the I/O thread, delete any spill chunks, free the pool

    uint64_t pushed()  const { return pushed_.load(std::memory_order_relaxed); }
    uint64_t popped()  const { return popped_.load(std::memory_order_relaxed); }
    bool     sealed()  const { return sealed_.load(std::memory_order_relaxed); }
    bool     spilled() const { return ever_spilled_.load(std::memory_order_relaxed); }

private:
    void io_loop();
    std::vector<uint8_t> take_buffer();             // reuse or allocate a tight frame buffer
    void                 pool_return(std::vector<uint8_t>&& buf);

    // chunk-file helpers (I/O thread only)
    std::string chunk_path(uint64_t idx) const;
    bool        write_one_to_disk(Frame& f);        // append to current write chunk
    bool        read_one_from_disk(Frame& out);     // read head of current read chunk

    int    width_ = 0, height_ = 0;
    size_t row_bytes_   = 0;
    size_t frame_bytes_ = 0;
    size_t record_size_ = 0;   // 8 (ts) + frame_bytes_, the on-disk record stride
    size_t ram_budget_  = 0;   // soft target for ready_ + to_spill_
    size_t ram_hard_cap_ = 0;  // absolute ceiling; push fails past this
    std::string spill_dir_;

    // Frames per spill chunk: a chunk is deleted once fully read, so this bounds
    // the disk "slack" beyond the live backlog to ~one chunk.
    static constexpr uint64_t kFramesPerChunk = 16;   // ~400 MB/chunk at 4080x3060

    std::mutex              mtx_;
    std::condition_variable cv_;
    std::deque<Frame>       ready_;       // RAM, poppable (oldest)
    std::deque<Frame>       to_spill_;    // RAM, awaiting disk append (newest)
    size_t                  ready_bytes_   = 0;
    size_t                  to_spill_bytes_ = 0;
    uint64_t                disk_frames_   = 0;   // frames currently on disk (unread)

    // chunk bookkeeping (touched only by the I/O thread under mtx_ for counters)
    uint64_t write_chunk_ = 0, read_chunk_ = 0;
    uint64_t frames_in_write_chunk_ = 0;          // appended to the current write chunk
    uint64_t frames_read_in_chunk_  = 0;          // read from the current read chunk
    int      wfd_ = -1;                            // current write-chunk fd
    int      rfd_ = -1;                            // current read-chunk fd

    std::vector<std::vector<uint8_t>> pool_;
    std::mutex                        pool_mtx_;

    std::thread       io_thread_;
    std::atomic<bool> sealed_{false};
    std::atomic<bool> shutdown_{false};
    std::atomic<bool> ever_spilled_{false};
    std::atomic<bool> drop_warned_{false};
    std::atomic<uint64_t> pushed_{0}, popped_{0};
};

} // namespace isp
