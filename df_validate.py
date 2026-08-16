"""End-to-end validation of the C++ DeepFilterNet engine (cpp/audio/df_net.cc).

Mirrors df_net.process() EXACTLY in numpy and drives the same three ONNX models
via onnxruntime, so a denoising result here proves the algorithm + models work and
localizes any bug before trusting the C++ (which is a 1:1 transcription).

Run: .venv_313/Scripts/python.exe df_validate.py
"""
import os, urllib.request
import numpy as np
import onnxruntime as ort
import soundfile as sf

ROOT = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.join(ROOT, "camera/src/main/assets/models/tmp/export")
OUT = os.path.join(ROOT, "tmp_df_golden")
os.makedirs(OUT, exist_ok=True)

# --- constants, identical to df_net.cc ---
FFT, HOP, FREQS = 960, 480, 481
NB_ERB, NB_DF, DF_ORDER, DF_LA, CONV_LA = 32, 96, 5, 2, 2
ALPHA, DELAY = 0.99, FFT - HOP
CHUNK, WARMUP = 2000, 100
ERB_W = np.array([2,2,2,2,2,2,2,2,2,2,2,2,2,5,5,7,7,8,10,12,13,15,18,20,24,28,31,37,42,50,56,67])
assert ERB_W.sum() == FREQS
_i = np.arange(FFT)
WIN = np.sin(np.pi/2 * np.sin(np.pi*(_i+0.5)/FFT)**2)
BAND_OF = np.repeat(np.arange(NB_ERB), ERB_W)   # [481] bin -> band

_enc = ort.InferenceSession(os.path.join(MODELS, "enc.onnx"), providers=["CPUExecutionProvider"])
_erb = ort.InferenceSession(os.path.join(MODELS, "erb_dec.onnx"), providers=["CPUExecutionProvider"])
_df  = ort.InferenceSession(os.path.join(MODELS, "df_dec.onnx"), providers=["CPUExecutionProvider"])


def analysis(x):
    ap = np.concatenate([np.zeros(DELAY), x, np.zeros(FFT)])
    T = (len(ap) - FFT)//HOP + 1
    spec = np.empty((T, FREQS), np.complex128)
    for t in range(T):
        spec[t] = np.fft.rfft(WIN * ap[t*HOP:t*HOP+FFT]) / FFT
    return spec, T


def features(spec):
    T = spec.shape[0]
    ferb = np.empty((T, NB_ERB), np.float32)
    fspec = np.empty((T, NB_DF), np.complex64)
    ms = -60.0 - 30.0*np.arange(NB_ERB)/(NB_ERB-1)      # linspace(-60,-90)
    us = 0.001 - 0.0009*np.arange(NB_DF)/(NB_DF-1)      # linspace(1e-3,1e-4)
    P = (spec.real**2 + spec.imag**2)
    for t in range(T):
        # erb: mean power per band -> dB -> running-mean normalize
        bandP = np.add.reduceat(P[t], np.r_[0, np.cumsum(ERB_W)[:-1]]) / ERB_W
        db = 10*np.log10(bandP + 1e-10)
        ms = db*(1-ALPHA) + ms*ALPHA
        ferb[t] = (db - ms)/40.0
        # unit_norm on first NB_DF bins
        x = spec[t, :NB_DF]
        us = np.abs(x)*(1-ALPHA) + us*ALPHA
        fspec[t] = x/np.sqrt(us)
    return ferb, fspec


def process(x, chunked=True):
    spec, T = analysis(x)
    ferb, fspec = features(spec)
    # pad_feat: ConstantPad2d((0,0,-2,2)) -> shift +CONV_LA, zero tail
    def shift(a):
        out = np.zeros_like(a)
        out[:T-CONV_LA] = a[CONV_LA:]
        return out
    ferb_s = shift(ferb)
    fspec_s = shift(fspec)

    acc = np.zeros(len(x) + DELAY + 2*FFT)
    wacc = np.zeros_like(acc)
    step = CHUNK if chunked else T
    for c0 in range(0, T, step):
        c1 = min(c0+step, T)
        cs = max(0, c0-WARMUP) if chunked else 0
        sl = slice(cs, c1)
        feat_erb = ferb_s[sl][None, None].astype(np.float32)             # [1,1,Tc,32]
        fs = fspec_s[sl]
        feat_spec = np.stack([fs.real, fs.imag]).astype(np.float32)[None]  # [1,2,Tc,96]
        e0,e1,e2,e3,emb,c0o,lsnr = _enc.run(None, {"feat_erb":feat_erb, "feat_spec":feat_spec})
        m = _erb.run(["m"], {"emb":emb,"e3":e3,"e2":e2,"e1":e1,"e0":e0})[0]   # [1,1,Tc,32]
        coefs = _df.run(["coefs"], {"emb":emb,"c0":c0o})[0]                   # [1,Tc,96,10]
        for t in range(c0, c1):
            lt = t - cs
            osp = np.empty(FREQS, np.complex128)
            # bins 0..95: 5-tap complex deep filter over spec[t-2..t+2]
            cf = coefs[0, lt].reshape(NB_DF, DF_ORDER, 2)
            low = np.zeros(NB_DF, np.complex128)
            for o in range(DF_ORDER):
                tt = t - DF_LA + o
                if 0 <= tt < T:
                    low += spec[tt, :NB_DF] * (cf[:, o, 0] + 1j*cf[:, o, 1])
            osp[:NB_DF] = low
            # bins 96..480: rectangular ERB mask gain
            g = m[0, 0, lt][BAND_OF[NB_DF:]]
            osp[NB_DF:] = spec[t, NB_DF:] * g
            tf = np.fft.irfft(osp*FFT, n=FFT) * WIN
            acc[t*HOP:t*HOP+FFT] += tf
            wacc[t*HOP:t*HOP+FFT] += WIN*WIN
    out = np.zeros(len(x))
    seg = np.where(wacc[DELAY:DELAY+len(x)] > 1e-8, acc[DELAY:DELAY+len(x)]/np.maximum(wacc[DELAY:DELAY+len(x)],1e-8), 0.0)
    out[:len(seg)] = seg
    return np.clip(out, -1.0, 1.0)


def snr(ref, est):
    return 10*np.log10(np.sum(ref**2) / (np.sum((est-ref)**2) + 1e-12))


def main():
    # Real speech sample (DeepFilterNet's own test asset).
    clean_path = os.path.join(OUT, "clean_freesound_33711.wav")
    if not os.path.exists(clean_path):
        url = "https://github.com/Rikorose/DeepFilterNet/raw/main/assets/clean_freesound_33711.wav"
        print("downloading test sample...")
        urllib.request.urlretrieve(url, clean_path)
    clean, sr = sf.read(clean_path)
    if clean.ndim > 1: clean = clean.mean(1)
    if sr != 48000:
        n = int(len(clean)*48000/sr)
        clean = np.interp(np.linspace(0, len(clean)-1, n), np.arange(len(clean)), clean)
        sr = 48000
    clean = clean.astype(np.float64)
    clean /= (np.abs(clean).max() + 1e-9)
    clean *= 0.5

    rng = np.random.default_rng(0)
    noise = rng.standard_normal(len(clean))
    target_snr = 5.0
    ns = np.sqrt(np.sum(clean**2)/(np.sum(noise**2)) / (10**(target_snr/10)))
    noisy = clean + ns*noise

    enh = process(noisy, chunked=True)
    enh_whole = process(noisy, chunked=False)

    sf.write(os.path.join(OUT, "val_clean.wav"), clean, sr)
    sf.write(os.path.join(OUT, "val_noisy.wav"), noisy, sr)
    sf.write(os.path.join(OUT, "val_enhanced.wav"), enh, sr)

    print(f"input  SNR : {snr(clean, noisy):6.2f} dB")
    print(f"output SNR : {snr(clean, enh):6.2f} dB   (improvement {snr(clean,enh)-snr(clean,noisy):+.2f} dB)")
    print(f"noise-only energy: noisy {10*np.log10(np.mean((noisy-clean)**2)):.1f} dB -> "
          f"residual {10*np.log10(np.mean((enh-clean)**2)+1e-12):.1f} dB")
    print(f"chunked vs whole-clip max abs diff: {np.max(np.abs(enh-enh_whole)):.2e}")

    # Sanity: pure noise should be strongly attenuated.
    pn = process(ns*noise, chunked=False)
    print(f"pure-noise attenuation: in {10*np.log10(np.mean((ns*noise)**2)):.1f} dB -> "
          f"out {10*np.log10(np.mean(pn**2)+1e-12):.1f} dB")
    print("wrote val_{clean,noisy,enhanced}.wav to tmp_df_golden/")


if __name__ == "__main__":
    main()
