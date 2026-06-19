#include "df_net.hh"

#include <onnxruntime_cxx_api.h>
#include "kiss_fftr.h"

#include <android/asset_manager.h>
#include <android/log.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "DfNet", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "DfNet", __VA_ARGS__)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace aud {

namespace {

// DeepFilterNet3 params (config.ini), all verified against the bundled models.
constexpr int   FFT   = 960;
constexpr int   HOP   = 480;
constexpr int   FREQS = FFT / 2 + 1;   // 481
constexpr int   NB_ERB = 32;
constexpr int   NB_DF  = 96;
constexpr int   DF_ORDER     = 5;
constexpr int   DF_LOOKAHEAD = 2;
constexpr int   CONV_LOOKAHEAD = 2;
constexpr float ALPHA = 0.99f;         // round(exp(-hop/sr/tau), 3)
constexpr int   DELAY = FFT - HOP;     // 480-sample STFT/ISTFT algorithmic delay

// Offline inference is chunked so a long background clip never builds one giant
// activation tensor. Each chunk re-runs WARMUP leading frames (output discarded)
// so the GRU hidden state re-converges before the kept region.
constexpr int CHUNK  = 2000;   // ~40 s of kept output per inference
constexpr int WARMUP = 100;    // ~2 s of discarded warmup context

// libdf erb band widths (sum == FREQS == 481), min_nb_erb_freqs = 2.
constexpr int ERB_W[NB_ERB] = {
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 5, 5, 7,
    7, 8, 10, 12, 13, 15, 18, 20, 24, 28, 31, 37, 42, 50, 56, 67,
};

using cf = std::complex<float>;

// DeepFilterNet post-filter on an ERB mask gain (Mask.pf, beta=0.02): preserves
// speech-band gains (~1) and pushes low/noise-band gains lower. Validated against
// the reference in df_validate.py.
inline float post_filter_gain(float g) {
    constexpr float beta = 0.02f;
    const float ms = std::max(g * std::sin((float)M_PI * g * 0.5f), 1e-12f);
    const float r  = g / ms;                      // == 1 / sin(pi*g/2)
    return (1.0f + beta) * g / (1.0f + beta * r * r);
}

} // namespace

struct DfNet::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "dfnet"};
    Ort::SessionOptions so;
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::unique_ptr<Ort::Session> enc, erbdec, dfdec;

    kiss_fftr_cfg fwd = nullptr, inv = nullptr;
    std::array<float, FFT>   window{};
    std::array<int, FREQS>   band_of{};   // freq bin -> erb band index
    bool ready = false;

    ~Impl() {
        if (fwd) kiss_fftr_free(fwd);
        if (inv) kiss_fftr_free(inv);
    }
};

DfNet::DfNet() : p_(std::make_unique<Impl>()) {}
DfNet::~DfNet() = default;
bool DfNet::ready() const { return p_ && p_->ready; }

bool DfNet::init(AAssetManager* assets) {
    Impl& I = *p_;

    auto load = [&](const char* fn, std::vector<uint8_t>& buf) -> bool {
        AAsset* a = AAssetManager_open(assets, fn, AASSET_MODE_BUFFER);
        if (!a) { LOGE("missing asset %s", fn); return false; }
        size_t n = AAsset_getLength(a);
        buf.resize(n);
        AAsset_read(a, buf.data(), n);
        AAsset_close(a);
        return true;
    };

    std::vector<uint8_t> e, r, d;
    if (!load("models/tmp/export/enc.onnx", e) ||
        !load("models/tmp/export/erb_dec.onnx", r) ||
        !load("models/tmp/export/df_dec.onnx", d)) {
        return false;
    }

    try {
        // CPU EP: the models are GRU-heavy with a dynamic time axis, which NNAPI
        // handles unreliably. Inference is offline at finalize, so CPU is fine.
        I.so.SetIntraOpNumThreads(6);   // use the big cores; finalize is offline
        // Full graph optimization for speed (verified on-device to denoise correctly
        // — the earlier white-noise was a failing build running the old placeholder,
        // not the optimizer).
        I.so.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
        I.enc    = std::make_unique<Ort::Session>(I.env, e.data(), e.size(), I.so);
        I.erbdec = std::make_unique<Ort::Session>(I.env, r.data(), r.size(), I.so);
        I.dfdec  = std::make_unique<Ort::Session>(I.env, d.data(), d.size(), I.so);
    } catch (const Ort::Exception& ex) {
        LOGE("ORT init failed: %s", ex.what());
        return false;
    }

    // Vorbis analysis/synthesis window: sin(pi/2 * sin(pi*(i+0.5)/N)^2).
    for (int i = 0; i < FFT; ++i) {
        float s = std::sin((float)M_PI * (i + 0.5f) / FFT);
        I.window[i] = std::sin((float)M_PI / 2.0f * s * s);
    }
    // Map each of the 481 freq bins to its (rectangular) ERB band.
    for (int b = 0, k = 0; b < NB_ERB; ++b)
        for (int w = 0; w < ERB_W[b]; ++w) I.band_of[k++] = b;

    I.fwd = kiss_fftr_alloc(FFT, 0, nullptr, nullptr);
    I.inv = kiss_fftr_alloc(FFT, 1, nullptr, nullptr);
    I.ready = (I.fwd && I.inv && I.enc && I.erbdec && I.dfdec);
    if (I.ready) LOGI("DeepFilterNet engine ready");
    return I.ready;
}

std::vector<float> DfNet::process(const std::vector<float>& in, bool post_filter) {
    Impl& I = *p_;
    if (!I.ready || in.empty()) return {};

    const int orig = (int)in.size();

    // Stage diagnostics (first chunk only) so an on-device run can localize where
    // garbage enters: input -> features -> model -> apply. Grep logcat tag "DfNet".
    auto logstat = [](const char* name, const float* p, size_t n) {
        if (!p || n == 0) { LOGI("  %s: empty", name); return; }
        double s2 = 0; float mn = p[0], mx = p[0]; int bad = 0;
        for (size_t i = 0; i < n; ++i) {
            const float v = p[i];
            if (std::isnan(v) || std::isinf(v)) { ++bad; continue; }
            s2 += (double)v * v; if (v < mn) mn = v; if (v > mx) mx = v;
        }
        LOGI("  %s: n=%zu rms=%.4g min=%.4g max=%.4g nan/inf=%d",
             name, n, std::sqrt(s2 / n), mn, mx, bad);
    };
    LOGI("DfNet.process: orig=%d samples", orig);
    logstat("input", in.data(), in.size());

    // Mirror df.enhance.enhance(): end-pad by FFT, then the STFT front-pads by
    // DELAY internally; the final result is recovered by trimming [DELAY, DELAY+orig).
    std::vector<float> ap((size_t)DELAY + orig + FFT, 0.0f);
    std::copy(in.begin(), in.end(), ap.begin() + DELAY);
    const int T = (int)((ap.size() - FFT) / HOP) + 1;
    if (T <= 0) return {};

    // ---- STFT + features (stateful normalization, run once over the whole clip).
    std::vector<cf>    spec((size_t)T * FREQS);
    std::vector<float> ferb((size_t)T * NB_ERB);   // erb_norm features
    std::vector<float> fspec_ri((size_t)T * NB_DF * 2);  // unit_norm, [re,im] interleaved

    std::array<float, NB_ERB> mState;
    std::array<float, NB_DF>  uState;
    for (int b = 0; b < NB_ERB; ++b) mState[b] = -60.0f - 30.0f * b / (NB_ERB - 1);          // linspace(-60,-90)
    for (int f = 0; f < NB_DF;  ++f) uState[f] = 0.001f - 0.0009f * f / (NB_DF - 1);          // linspace(1e-3,1e-4)

    std::array<float, FFT> frame{};
    std::array<kiss_fft_cpx, FREQS> fout{};
    for (int t = 0; t < T; ++t) {
        const float* src = &ap[(size_t)t * HOP];
        for (int i = 0; i < FFT; ++i) frame[i] = src[i] * I.window[i];
        kiss_fftr(I.fwd, frame.data(), fout.data());

        cf* sp = &spec[(size_t)t * FREQS];
        for (int k = 0; k < FREQS; ++k) sp[k] = cf(fout[k].r / FFT, fout[k].i / FFT);

        // erb: mean power per band -> dB -> running-mean normalize.
        float* fe = &ferb[(size_t)t * NB_ERB];
        for (int b = 0, k = 0; b < NB_ERB; ++b) {
            float accP = 0.0f;
            for (int w = 0; w < ERB_W[b]; ++w) {
                const cf v = sp[k++];
                accP += v.real() * v.real() + v.imag() * v.imag();
            }
            const float db = 10.0f * std::log10(accP / ERB_W[b] + 1e-10f);
            mState[b] = db * (1.0f - ALPHA) + mState[b] * ALPHA;
            fe[b] = (db - mState[b]) / 40.0f;
        }
        // unit_norm on the first NB_DF complex bins.
        float* fs = &fspec_ri[(size_t)t * NB_DF * 2];
        for (int f = 0; f < NB_DF; ++f) {
            const cf v = sp[f];
            const float mag = std::abs(v);
            uState[f] = mag * (1.0f - ALPHA) + uState[f] * ALPHA;
            const float inv = 1.0f / std::sqrt(uState[f]);
            fs[2 * f]     = v.real() * inv;
            fs[2 * f + 1] = v.imag() * inv;
        }
    }

    // pad_feat: ConstantPad2d((0,0,-CONV_LOOKAHEAD,CONV_LOOKAHEAD)) — shift the
    // encoder features CONV_LOOKAHEAD frames earlier, zero-filling the tail.
    auto shifted_erb = [&](int t, int b) -> float {
        const int s = t + CONV_LOOKAHEAD;
        return s < T ? ferb[(size_t)s * NB_ERB + b] : 0.0f;
    };
    auto shifted_spec = [&](int t, int f, int c) -> float {
        const int s = t + CONV_LOOKAHEAD;
        return s < T ? fspec_ri[(size_t)s * NB_DF * 2 + 2 * f + c] : 0.0f;
    };

    // ---- Output overlap-add accumulators (and squared-window weights for COLA).
    std::vector<float> acc(ap.size() + FFT, 0.0f), wacc(ap.size() + FFT, 0.0f);

    const char* enc_in[]  = {"feat_erb", "feat_spec"};
    const char* enc_out[] = {"e0", "e1", "e2", "e3", "emb", "c0", "lsnr"};
    const char* erb_in[]  = {"emb", "e3", "e2", "e1", "e0"};
    const char* erb_out[] = {"m"};
    const char* df_in[]   = {"emb", "c0"};
    const char* df_out[]  = {"coefs"};

    std::array<kiss_fft_cpx, FREQS> ospec{};
    std::array<float, FFT> otime{};

    for (int c0 = 0; c0 < T; c0 += CHUNK) {
        const int c1 = std::min(c0 + CHUNK, T);
        const int cs = std::max(0, c0 - WARMUP);   // include warmup context
        const int Tc = c1 - cs;
        const bool dbg = (c0 == 0);

        // Build enc inputs for [cs, c1) with pad_feat applied.
        std::vector<float> bErb((size_t)Tc * NB_ERB);
        std::vector<float> bSpec((size_t)2 * Tc * NB_DF);  // [C=2, Tc, NB_DF]
        for (int lt = 0; lt < Tc; ++lt) {
            const int t = cs + lt;
            for (int b = 0; b < NB_ERB; ++b) bErb[(size_t)lt * NB_ERB + b] = shifted_erb(t, b);
            for (int f = 0; f < NB_DF; ++f) {
                bSpec[(size_t)0 * Tc * NB_DF + (size_t)lt * NB_DF + f] = shifted_spec(t, f, 0);
                bSpec[(size_t)1 * Tc * NB_DF + (size_t)lt * NB_DF + f] = shifted_spec(t, f, 1);
            }
        }
        if (dbg) { logstat("feat_erb", bErb.data(), bErb.size());
                   logstat("feat_spec", bSpec.data(), bSpec.size()); }

        std::vector<Ort::Value> mOut, cOut;
        // Owned copies of the encoder outputs fed to the decoders. Feeding one ORT
        // session's live output buffer straight into another session is fragile
        // across ORT builds; ORT-Python returns copies, so we mirror that here.
        std::vector<float> bufEmb, bufE0, bufE1, bufE2, bufE3, bufC0;
        std::vector<int64_t> shEmb, shE0, shE1, shE2, shE3, shC0;
        try {
            const std::array<int64_t, 4> erbShape{1, 1, Tc, NB_ERB};
            const std::array<int64_t, 4> specShape{1, 2, Tc, NB_DF};
            Ort::Value encIns[] = {
                Ort::Value::CreateTensor<float>(I.mem, bErb.data(),  bErb.size(),  erbShape.data(),  4),
                Ort::Value::CreateTensor<float>(I.mem, bSpec.data(), bSpec.size(), specShape.data(), 4),
            };
            std::vector<Ort::Value> encOut =
                I.enc->Run(Ort::RunOptions{nullptr}, enc_in, encIns, 2, enc_out, 7);

            // enc_out order: e0,e1,e2,e3,emb,c0,lsnr. Copy out into owned buffers.
            auto copyOut = [](Ort::Value& v, std::vector<float>& buf, std::vector<int64_t>& sh) {
                auto info = v.GetTensorTypeAndShapeInfo();
                sh = info.GetShape();
                const float* p = v.GetTensorData<float>();
                buf.assign(p, p + info.GetElementCount());
            };
            copyOut(encOut[0], bufE0, shE0);  copyOut(encOut[1], bufE1, shE1);
            copyOut(encOut[2], bufE2, shE2);  copyOut(encOut[3], bufE3, shE3);
            copyOut(encOut[4], bufEmb, shEmb); copyOut(encOut[5], bufC0, shC0);
            // encOut and its live buffers are released here; the decoders read our copies.

            auto own = [&](std::vector<float>& buf, std::vector<int64_t>& sh) {
                return Ort::Value::CreateTensor<float>(I.mem, buf.data(), buf.size(), sh.data(), sh.size());
            };
            Ort::Value erbIns[] = { own(bufEmb, shEmb), own(bufE3, shE3), own(bufE2, shE2),
                                    own(bufE1, shE1), own(bufE0, shE0) };
            mOut = I.erbdec->Run(Ort::RunOptions{nullptr}, erb_in, erbIns, 5, erb_out, 1);

            Ort::Value dfIns[] = { own(bufEmb, shEmb), own(bufC0, shC0) };
            cOut = I.dfdec->Run(Ort::RunOptions{nullptr}, df_in, dfIns, 2, df_out, 1);
        } catch (const Ort::Exception& ex) {
            LOGE("inference failed: %s", ex.what());
            return {};
        }
        if (dbg) { logstat("emb", bufEmb.data(), bufEmb.size());
                   logstat("enc_c0", bufC0.data(), bufC0.size()); }

        const float* mData = mOut[0].GetTensorData<float>();      // [1,1,Tc,NB_ERB]
        const float* cData = cOut[0].GetTensorData<float>();      // [1,Tc,NB_DF,DF_ORDER*2]
        if (dbg) {
            logstat("m(mask)", mData, mOut[0].GetTensorTypeAndShapeInfo().GetElementCount());
            logstat("coefs",   cData, cOut[0].GetTensorTypeAndShapeInfo().GetElementCount());
        }

        // Apply mask + deep filter for the kept range [c0, c1) and OLA-synthesize.
        for (int t = c0; t < c1; ++t) {
            const int lt = t - cs;
            const float* mt = &mData[(size_t)lt * NB_ERB];
            const float* ct = &cData[(size_t)lt * NB_DF * DF_ORDER * 2];
            const cf* st = &spec[(size_t)t * FREQS];

            // Per-frame ERB mask gains (optionally post-filtered), indexed by band.
            float gm[NB_ERB];
            for (int b = 0; b < NB_ERB; ++b)
                gm[b] = post_filter ? post_filter_gain(mt[b]) : mt[b];

            for (int f = 0; f < FREQS; ++f) {
                if (f < NB_DF) {
                    // Deep filter: 5-tap complex MAC over spec[t-2 .. t+2].
                    cf e(0.0f, 0.0f);
                    for (int o = 0; o < DF_ORDER; ++o) {
                        const int tt = t - DF_LOOKAHEAD + o;
                        if (tt < 0 || tt >= T) continue;
                        const cf coef(ct[f * DF_ORDER * 2 + o * 2], ct[f * DF_ORDER * 2 + o * 2 + 1]);
                        e += spec[(size_t)tt * FREQS + f] * coef;
                    }
                    ospec[f].r = e.real();
                    ospec[f].i = e.imag();
                } else {
                    // ERB mask gain (rectangular interpolation, real).
                    const float g = gm[I.band_of[f]];
                    ospec[f].r = st[f].real() * g;
                    ospec[f].i = st[f].imag() * g;
                }
            }

            kiss_fftri(I.inv, ospec.data(), otime.data());
            float* a = &acc[(size_t)t * HOP];
            float* w = &wacc[(size_t)t * HOP];
            for (int i = 0; i < FFT; ++i) {
                const float win = I.window[i];
                a[i] += otime[i] * win;
                w[i] += win * win;
            }
        }
    }

    // Recover the original-aligned region: index DELAY corresponds to input sample 0.
    std::vector<float> out(orig);
    for (int i = 0; i < orig; ++i) {
        const float wv = wacc[(size_t)DELAY + i];
        float s = wv > 1e-8f ? acc[(size_t)DELAY + i] / wv : 0.0f;
        out[i] = std::clamp(s, -1.0f, 1.0f);
    }
    logstat("output", out.data(), out.size());
    return out;
}

} // namespace aud
