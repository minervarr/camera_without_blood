#include "frame_store.hh"

#include <cstring>

#include "../logger.hh"

#undef LOG_TAG
#define LOG_TAG "FrameStore"

namespace isp {

bool FrameStore::init(int width, int height) {
    width_       = width;
    height_      = height;
    row_bytes_   = static_cast<size_t>(width) * 2;
    frame_bytes_ = row_bytes_ * static_cast<size_t>(height);

    sealed_.store(false);
    shutdown_.store(false);
    drop_warned_.store(false);
    pushed_.store(0);
    popped_.store(0);

    LOGI("init %dx%d frame=%zuMB cap=%zu frames (%zuMB)", width, height,
         frame_bytes_ >> 20, kMaxQueuedFrames,
         (kMaxQueuedFrames * frame_bytes_) >> 20);
    return true;
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
    if (pool_.size() < kMaxPooledBuffers) pool_.push_back(std::move(buf));
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

    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (ready_.size() >= kMaxQueuedFrames) {
            // Recycle rather than free: under sustained overload this is the hot
            // path, and a 25 MB malloc/free per dropped frame would be worse than
            // the drop itself. (mtx_ -> pool_mtx_ is the only nesting order used.)
            pool_return(std::move(buf));
            if (!drop_warned_.exchange(true))
                LOGE("backlog cap (%zu frames, %zuMB) hit — develop behind camera, dropping frames",
                     kMaxQueuedFrames, (kMaxQueuedFrames * frame_bytes_) >> 20);
            return false;
        }
        Frame f;
        f.data  = std::move(buf);
        f.ts_ns = ts_ns;
        ready_.push_back(std::move(f));
        pushed_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_.notify_one();
    return true;
}

bool FrameStore::pop(Frame& out) {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [this] {
        return shutdown_.load() || !ready_.empty() || sealed_.load();
    });
    if (shutdown_.load()) return false;
    if (ready_.empty()) return false;          // sealed + drained

    out = std::move(ready_.front());
    ready_.pop_front();
    popped_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void FrameStore::seal() {
    sealed_.store(true, std::memory_order_relaxed);
    cv_.notify_all();
}

void FrameStore::shutdown() {
    shutdown_.store(true);
    cv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ready_.clear();
    }
    { std::lock_guard<std::mutex> lk(pool_mtx_); pool_.clear(); }
}

} // namespace isp
