#include "frame_store.hh"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

#include "../logger.hh"

#undef LOG_TAG
#define LOG_TAG "FrameStore"

namespace isp {

bool FrameStore::init(int width, int height, size_t ram_budget_bytes,
                      std::string spill_dir) {
    width_       = width;
    height_      = height;
    row_bytes_   = static_cast<size_t>(width) * 2;
    frame_bytes_ = row_bytes_ * static_cast<size_t>(height);
    record_size_ = 8 + frame_bytes_;
    ram_budget_  = ram_budget_bytes;   // legacy; disk spill is disabled (see push)
    // BOUNDED in-RAM queue — NEVER spill to disk. If develop falls behind the
    // camera, push() drops the newest frame past this cap rather than writing tens
    // of GB of overflow. Sized to absorb jitter (~16 frames ≈ 0.4 GB); under
    // sustained overload the clip simply records at the develop rate.
    ram_hard_cap_ = 16 * frame_bytes_;
    spill_dir_    = std::move(spill_dir);

    sealed_.store(false);
    shutdown_.store(false);
    ever_spilled_.store(false);
    drop_warned_.store(false);
    pushed_.store(0);
    popped_.store(0);
    disk_frames_ = 0;
    write_chunk_ = read_chunk_ = 0;
    frames_in_write_chunk_ = frames_read_in_chunk_ = 0;

    io_thread_ = std::thread(&FrameStore::io_loop, this);
    LOGI("init %dx%d frame=%zuMB budget=%zuMB dir=%s",
         width, height, frame_bytes_ >> 20, ram_budget_ >> 20, spill_dir_.c_str());
    return true;
}

std::string FrameStore::chunk_path(uint64_t idx) const {
    char buf[32];
    snprintf(buf, sizeof(buf), "/fs_%06llu.raw", static_cast<unsigned long long>(idx));
    return spill_dir_ + buf;
}

std::vector<uint8_t> FrameStore::take_buffer() {
    {
        std::lock_guard<std::mutex> lk(pool_mtx_);
        if (!pool_.empty()) {
            std::vector<uint8_t> b = std::move(pool_.back());
            pool_.pop_back();
            return b;
        }
    }
    return std::vector<uint8_t>(frame_bytes_);
}

void FrameStore::pool_return(std::vector<uint8_t>&& buf) {
    if (buf.size() != frame_bytes_) return;   // drop odd-sized buffers
    std::lock_guard<std::mutex> lk(pool_mtx_);
    if (pool_.size() < 8) pool_.push_back(std::move(buf));
}

void FrameStore::reclaim(Frame&& f) { pool_return(std::move(f.data)); }

bool FrameStore::push(const uint8_t* data, int stride_bytes, int64_t ts_ns) {
    // De-stride into a pooled buffer first (the heavy part — keep it off the lock).
    std::vector<uint8_t> buf = take_buffer();
    uint8_t* dst = buf.data();
    if (static_cast<size_t>(stride_bytes) == row_bytes_) {
        std::memcpy(dst, data, frame_bytes_);
    } else {
        for (int y = 0; y < height_; ++y)
            std::memcpy(dst + static_cast<size_t>(y) * row_bytes_,
                        data + static_cast<size_t>(y) * stride_bytes, row_bytes_);
    }

    std::lock_guard<std::mutex> lk(mtx_);
    // Bounded in-RAM queue — no disk spill. If develop is behind the camera, drop
    // the newest frame past the cap (the clip records at the develop rate) instead
    // of spilling tens of GB to storage.
    if (ready_bytes_ + frame_bytes_ > ram_hard_cap_) {
        pool_return(std::move(buf));
        if (!drop_warned_.exchange(true))
            LOGE("backlog cap (%zuMB) hit — develop behind camera, dropping frames",
                 ram_hard_cap_ >> 20);
        return false;
    }

    Frame f;
    f.data  = std::move(buf);
    f.ts_ns = ts_ns;
    ready_bytes_ += frame_bytes_;
    ready_.push_back(std::move(f));
    pushed_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_all();
    return true;
}

bool FrameStore::pop(Frame& out) {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [this] {
        if (shutdown_.load()) return true;
        if (!ready_.empty()) return true;
        // Fully drained after seal: nothing in RAM, nothing on/headed-to disk.
        return sealed_.load() && to_spill_.empty() && disk_frames_ == 0;
    });
    if (shutdown_.load()) return false;
    if (ready_.empty()) return false;          // sealed + drained

    out = std::move(ready_.front());
    ready_.pop_front();
    ready_bytes_ -= frame_bytes_;
    popped_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_all();                          // wake I/O thread to prefetch into freed room
    return true;
}

void FrameStore::seal() {
    sealed_.store(true, std::memory_order_relaxed);
    cv_.notify_all();
}

// ── Disk chunk I/O (I/O thread only) ─────────────────────────────────────────

bool FrameStore::write_one_to_disk(Frame& f) {
    if (wfd_ < 0) {
        wfd_ = ::open(chunk_path(write_chunk_).c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (wfd_ < 0) { LOGE("open write chunk %llu failed", (unsigned long long)write_chunk_); return false; }
        frames_in_write_chunk_ = 0;
    }
    const off_t off = static_cast<off_t>(frames_in_write_chunk_) * static_cast<off_t>(record_size_);
    if (::pwrite(wfd_, &f.ts_ns, 8, off) != 8 ||
        ::pwrite(wfd_, f.data.data(), frame_bytes_, off + 8) != static_cast<ssize_t>(frame_bytes_)) {
        LOGE("pwrite chunk %llu failed", (unsigned long long)write_chunk_);
        return false;
    }
    if (++frames_in_write_chunk_ >= kFramesPerChunk) {
        ::close(wfd_);
        wfd_ = -1;
        ++write_chunk_;
        frames_in_write_chunk_ = 0;
    }
    return true;
}

bool FrameStore::read_one_from_disk(Frame& out) {
    if (rfd_ < 0) {
        rfd_ = ::open(chunk_path(read_chunk_).c_str(), O_RDONLY);
        if (rfd_ < 0) { LOGE("open read chunk %llu failed", (unsigned long long)read_chunk_); return false; }
        frames_read_in_chunk_ = 0;
    }
    const off_t off = static_cast<off_t>(frames_read_in_chunk_) * static_cast<off_t>(record_size_);
    if (out.data.size() != frame_bytes_) out.data.resize(frame_bytes_);
    if (::pread(rfd_, &out.ts_ns, 8, off) != 8 ||
        ::pread(rfd_, out.data.data(), frame_bytes_, off + 8) != static_cast<ssize_t>(frame_bytes_)) {
        LOGE("pread chunk %llu failed", (unsigned long long)read_chunk_);
        return false;
    }
    ++frames_read_in_chunk_;
    // Advance + delete only a chunk the writer has already moved past (it's full).
    if (read_chunk_ < write_chunk_ && frames_read_in_chunk_ >= kFramesPerChunk) {
        ::close(rfd_);
        rfd_ = -1;
        ::unlink(chunk_path(read_chunk_).c_str());
        ++read_chunk_;
        frames_read_in_chunk_ = 0;
    }
    return true;
}

void FrameStore::io_loop() {
    for (;;) {
        bool do_write = false, do_read = false;
        Frame wframe;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] {
                if (shutdown_.load()) return true;
                if (!to_spill_.empty()) return true;
                if (disk_frames_ > 0 && ready_bytes_ + frame_bytes_ <= ram_budget_) return true;
                return false;
            });
            if (shutdown_.load()) break;

            const bool ram_room  = ready_bytes_ + frame_bytes_ <= ram_budget_;
            const bool ready_low = ready_bytes_ < frame_bytes_ * 2;   // keep ~2 frames ahead of the consumer
            if (disk_frames_ > 0 && ram_room && (ready_low || to_spill_.empty())) {
                do_read = true;                                       // feed the consumer first
            } else if (!to_spill_.empty()) {
                wframe = std::move(to_spill_.front());
                to_spill_.pop_front();
                to_spill_bytes_ -= frame_bytes_;
                do_write = true;
            } else if (disk_frames_ > 0 && ram_room) {
                do_read = true;
            }
        }

        if (do_write) {
            write_one_to_disk(wframe);
            pool_return(std::move(wframe.data));
            { std::lock_guard<std::mutex> lk(mtx_); ++disk_frames_; }
            cv_.notify_all();
        }
        if (do_read) {
            Frame f;
            f.data = take_buffer();
            if (read_one_from_disk(f)) {
                std::lock_guard<std::mutex> lk(mtx_);
                ready_bytes_ += frame_bytes_;
                ready_.push_back(std::move(f));
                --disk_frames_;
            } else {
                pool_return(std::move(f.data));
            }
            cv_.notify_all();
        }
    }
}

void FrameStore::shutdown() {
    if (!io_thread_.joinable() && wfd_ < 0 && rfd_ < 0) return;
    shutdown_.store(true);
    cv_.notify_all();
    if (io_thread_.joinable()) io_thread_.join();

    if (wfd_ >= 0) { ::close(wfd_); wfd_ = -1; }
    if (rfd_ >= 0) { ::close(rfd_); rfd_ = -1; }
    // Delete any chunk files still on disk (read_chunk_ .. write_chunk_).
    for (uint64_t i = read_chunk_; i <= write_chunk_; ++i)
        ::unlink(chunk_path(i).c_str());

    ready_.clear();
    to_spill_.clear();
    ready_bytes_ = to_spill_bytes_ = 0;
    disk_frames_ = 0;
    { std::lock_guard<std::mutex> lk(pool_mtx_); pool_.clear(); }
}

} // namespace isp
