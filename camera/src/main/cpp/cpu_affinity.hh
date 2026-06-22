#pragma once

#include <android/log.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <vector>

// Header-only thread-placement helpers. The RAW video pipeline runs one heavy
// orchestrator thread (GPU dispatch + readback + encoder feed) at a strict
// ~33ms cadence; if the scheduler migrates it onto a little core mid-recording
// it produces periodic frame-time spikes (a classic ~1Hz hitch). Pinning it to
// the SoC's fast cluster removes that migration jitter. All calls are
// best-effort — a failure logs and is otherwise ignored (no behavior change).
namespace cpuaff {

// Pins the calling thread to the cores in the highest max-frequency tier (the
// prime/big cluster), discovered from sysfs. Returns false best-effort.
inline bool pin_current_thread_to_fast_cores(const char* tag) {
    const long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n <= 1) return false;

    std::vector<long> freq(static_cast<size_t>(n), 0);
    long maxf = 0;
    for (long i = 0; i < n; ++i) {
        char path[128];
        std::snprintf(path, sizeof(path),
                      "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", i);
        FILE* f = std::fopen(path, "re");
        if (!f) continue;
        long v = 0;
        if (std::fscanf(f, "%ld", &v) == 1) {
            freq[static_cast<size_t>(i)] = v;
            if (v > maxf) maxf = v;
        }
        std::fclose(f);
    }

    // Slowest reported frequency = the little cluster. Pin to every core FASTER
    // than that (big + prime). On a 1+4+3 SoC like the SD8g2 the prime core is
    // its own top tier, so a "within 5% of peak" rule would select only that one
    // core and serialize our pipeline/drain/mux threads onto it — selecting
    // "faster than the little tier" hands the scheduler the whole big+prime
    // cluster instead, which is what we want for throughput without hitches.
    long minf = 0;
    for (long i = 0; i < n; ++i) {
        long v = freq[static_cast<size_t>(i)];
        if (v > 0 && (minf == 0 || v < minf)) minf = v;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    int picked = 0;
    if (maxf > minf) {
        for (long i = 0; i < n; ++i)
            if (freq[static_cast<size_t>(i)] > minf) { CPU_SET(i, &set); ++picked; }
    }
    if (picked == 0) {  // uniform freqs or sysfs unreadable — upper half of cores
        for (long i = n / 2; i < n; ++i) { CPU_SET(i, &set); ++picked; }
    }

    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        __android_log_print(ANDROID_LOG_WARN, tag,
                            "cpu affinity not applied (wanted %d fast core(s))", picked);
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, tag,
                        "pinned to %d fast core(s) of %ld", picked, n);
    return true;
}

// Best-effort: nudge the calling thread to a higher (more favorable) priority.
// Android usually clamps how negative an unprivileged app may go; failures are
// silently ignored.
inline void raise_current_thread_priority(int nice_value) {
    setpriority(PRIO_PROCESS, 0 /* calling thread */, nice_value);
}

}  // namespace cpuaff
