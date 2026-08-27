# RESOLVED: the right/bottom edge band is a PLAYBACK artifact, not a capture one

Read this before spending another hour on it. **The recordings are correct.**

The symptom: a streaked ~1 mm band along the right and bottom edges of every clip,
horizontal streaks on the right, vertical along the bottom, never top or left,
present since the very first clip this project ever produced.

**It is the hardware video decoder, at playback.** In mpv, switching
`--hwdec` from `mediacodec` / `mediacodec-copy` to **software** makes it disappear
entirely — same file, same player, only the decoder changed. A wrong bitstream
would look wrong to both decoders. It does not.

Everything that had been treated as evidence about capture is explained by this,
and only by this:

- **Stills are clean.** Not because they skip our ISP or the encoder — because a
  DNG/PNG never goes through a hardware *video* decoder at all.
- **Every capture-side change failed to move it**, because none of them was ever
  touching the cause.
- **It has been there since the beginning**, across totally different pipelines.

## Three capture-side theories, all disproved — do not retry them

1. **Encoder input stride padding.** Disproved by the device log: the encoder
   reports a tight stride (`stride 4096 B, slice height 1536` for 2048x1536), so
   `submit_to_encoder` takes the single-memcpy fast path and no pad exists.
2. **Partial CTUs + the HEVC conformance window** (which crops only from the right
   and bottom — the asymmetry matched perfectly). Padding the encode to whole
   64x64 CTUs did not remove the band, and the pad became its own measurable
   artifact. See the CTU padding entry below.
3. **Rate-control starvation at the end of the raster scan.** The bitrate numbers
   are real (see the table below) but they are not this. Also: the encoder ignores
   any bitrate request above its own ~254 Mbps wall, so this was never tunable
   anyway.

## What this does and does not license

- It does **not** mean the capture is unimprovable — the bitrate/denoise trade
  below is real and independent.
- It **does** mean the band is not a reason to disable anything, crop anything, or
  change the container. Play with software decode, or in a player that does.

---

# Method catalogue — what is on, what is off, and why

Everything here **works and is compiled into the app**. Nothing is deleted, because
the measurements behind it were expensive and are still valid. Each entry says what
it does, what it measurably bought, whether it is currently on or off, and the exact
switch to change that.

**Current shipping configuration:** 2x2 binned RAW at 2040x1530, NLM denoise **on**,
3-pass chroma median (incl. the dilated middle pass) **on**, CTU padding **off**,
no crop, 30 fps, ~243 Mbps requested.

## The denoise / bitrate question is OPEN — the numbers below are not an A/B

An earlier version of this file claimed the denoise halves the encoder's load
(573 KB/frame vs 1030 KB/frame, 141 vs 253 Mbps) and presented it as decisive.
**Retracted: those two clips were different scenes shot ~35 minutes apart.** A
later clip *with* the denoise on measured 1003 KB/frame (246 Mbps), i.e. right up
with the un-filtered one. Scene content dominates the bitrate far more than the
denoise does, so the comparison measured the room, not the filter.

This is the same error that invalidated an earlier round of noise comparisons in
this project. **Any future claim here needs one fixed scene, phone untouched,
exposure matched, framing correlation verified, denoise toggled between takes.**

What IS established and does not depend on that comparison:

- The encoder has a hard ceiling near **254 Mbps** and ignores requests above it —
  device-measured, asking for 449 Mbps delivered 253, exactly what asking for 243
  delivered. **Never try to fix encoder quality by raising `kTargetBppBinned`.**
- Sensor noise is uncorrelated frame to frame, so inter prediction cannot predict
  it. That mechanism is real; its magnitude here is simply unmeasured.
- The denoise's *picture* benefit was measured properly on a fixed scene and does
  stand: shadow luma noise 13.0 -> 7.1, mid-tone 8.9 -> 2.3.

**Current default is denoise ON + chroma median ON**, on the strength of the
picture measurement and because the edge artifact turned out to be a playback
issue (above) rather than a reason to strip the pipeline.

---

## 1. NLM spatial denoise (RGB, binned path) — ON (see above)

**Files:** `isp/shaders_src/nlm_rgb.slang`, `nlm_rgb_fp16.slang`
**Switch:** `RawVideoPipeline::set_denoise(true)` — or the `denoise_enabled_{false}` default in `raw_video_pipeline.hh`

Non-local-means over the binned linear-RGB plane. Patch distance on a
**white-balanced** guide luma (un-gained luma is ~colour-blind and bleeds colour
across edges — that was a real, fixed bug, not a theoretical one). Search radius 1
(8 candidates), 3x3 patch, driven by the sensor's **measured** `NoiseProfile`
rather than constants.

**What it measurably bought:** shadow luma noise 13.0 -> 7.1, mid-tone 8.9 -> 2.3,
edge-to-flat gradient ratio 1.85 -> 2.4.

**Cost:** ~16-21 ms of the 33.3 ms frame budget — about **80 % of all GPU time** in
the pipeline. Everything else together is ~5 ms.

**Why off:** it is the single largest thing standing between the encoder and the
raw sensor signal, and the prime suspect for any spatial artifact. Turning it off
is both the release default and the decisive test.

**Known limits, so nobody re-derives them:**
- `kNlmH` 1.25 vs 4 vs 8 measured **identical** in a dark scene (within 0.2 %). The
  3x3 search window is the binding constraint, not the strength. You cannot tune
  your way out of dark-scene noise with this knob.
- Radius 2 (24 candidates) lands near 45-50 ms. **Does not fit 30 fps.** Settled by
  measurement, not opinion.

---

## 2. Chroma median, 3 passes incl. a dilated one — ON (see above)

**File:** `isp/shaders_src/chroma_median.slang`
**Switch:** `RawVideoPipeline::set_chroma(true)`

Three 9-tap median passes over the CbCr plane: spacing 1 -> **spacing 2** -> spacing 1,
ping-ponging two scratch buffers. A median *selects* one of the nine values already
present, so unlike any blur it **cannot invent a colour or bleed across an edge**.
The dilated middle pass exists because a 2x2 speckle blob is the majority inside a
3x3 window and is therefore a fixed point of a spacing-1 median; at spacing 2 the
ring lies outside the blob and it loses the vote.

**What it measurably bought:** colour speckle down ~30 % (one pass), **38-43 %**
(two passes) in the shadows.

**Cost:** ~0.8 ms per pass, ~2.4 ms total — about 7 % of budget. Cheap.

**Why off:** it only exists to clean up noise that the plain path no longer tries
to hide, and it is the other pass that touches pixel values. Off for the same
"faithful representation" reason, not because it misbehaved. **It never writes the
Y plane**, so luma is byte-identical either way.

---

## 3. HQ directional demosaic (RCD-style) — OFF, and irrelevant while binned

**Files:** `isp/shaders_src/green_isp.slang`, `debayer_isp.slang`
**Switch:** `set_demosaic_hq(true)`, full-resolution path only (`push.cfa` bit 8)

Two-pass directional green plane + R/B reconstruction in colour-difference space.
The default full-res demosaic is single-pass Malvar.

**Why off:** the binned path has **no demosaic at all** (each 2x2 CFA quad becomes
one pixel from real measurements), so this pipeline is never even created. HQ + the
old chroma denoise together measured ~57 ms/frame — holds 30 fps only until the GPU
throttles, then halves the frame rate.

---

## 4. Chroma bilateral denoise (full-res path) — OFF

**File:** `isp/shaders_src/chroma_denoise.slang`
**Switch:** `push.cfa` bit 9, full-resolution path only

Luma-guided bilateral over chroma. Superseded by the median (#2), which is
structurally halo-free where a bilateral is not — a weighted average near a
high-contrast edge mixes both sides, which is exactly how a chroma denoise produces
a coloured aura.

---

## 4b. CTU padding of the encoded picture — OFF

**Files:** `isp/raw_video_pipeline.{cc,hh}` (`kCtuPadEncode`), `shaders_src/nlm_rgb.slang` (`srcDim`)
**Switch:** `kCtuPadEncode = true`

Rounds the encoded picture up to whole 64x64 HEVC coding tree units
(2040x1530 -> 2048x1536, 4080x3060 -> 4096x3072, +0.79 % pixels, aspect ratio
unchanged), filling the surplus by **replicating** the last real column and row.
Nothing is ever cropped. `nlm_rgb.slang`'s `load_rgb` clamp does the replication;
`debayer_isp.slang` needed no change because it already clamps its raw read.

**Why it was built:** a streaked ~1 mm border on the right and bottom edges of
every clip. An unaligned picture forces the encoder to code partial CTUs there and
signal a conformance window, and an HEVC conformance window crops only from the
right and bottom — which matched the symptom's asymmetry exactly.

**Why it is off — the theory did not survive the device test.** The border was
still visible with the pad in place. And the pad turned out to be its own artifact:
the encoder spends few bits on the flat replicated strip, so after lossy coding it
no longer matches the column it was copied from — measured **1.21-1.26x** interior
gradient across the pad against **1.09-1.16x** for the adjacent real columns, a
step at the pad boundary. With padding off that step is gone (right columns flat at
1.02-1.16).

**Status of the original artifact: UNEXPLAINED.** Two hypotheses are now dead —
encoder stride padding (disproved by the device log: stride is tight) and CTU
alignment (disproved above). Note also that the numeric edge probe used here
**cannot see the reported artifact**: on a plain clip it reports the *left* and
*top* edges as more active (1.7-1.8x) than the right and bottom (1.0-1.2x), i.e.
it is dominated by scene content. Any future attempt needs a different measurement,
and should start by reproducing the artifact on a controlled target rather than a
room.

---

## 5. Full-resolution capture (4080x3060) — OFF

**Switch:** `kBinnedRawVideo` in `recorder/recorder.cc`

**Why off, device-measured on the S23 Ultra:** a full-res take decays to **18.0 fps
with ~35 % of frames dropped** (4754 in / 3095 encoded / 1659 dropped) after ~2.5
minutes of thermal throttling. Binned holds a flat 30 fps for 6+ minutes with zero
drops. Frame rate is the one thing a recorder cannot give up.

---

# Deleted on purpose — do NOT reintroduce

- **Motion-adaptive temporal denoise.** Device-verified to smear handheld motion.
  Its absence is now *structural*: the binned path binds only the current frame's
  staging slot plus per-slot intermediates, and a shader can only read what is
  bound, so a trail or aura behind motion is impossible by construction. Bringing
  it back requires explicit past-frame bindings — that is the thing not to do.
- **AI Bayer denoiser (ncnn/DNCNN).** `run()` was never called, yet its constructor
  span up a whole ncnn Vulkan instance per pipeline. Removed with the dependency.
- **Auto black-point pedestal.** Built on a measurement that turned out to be
  wrong (block means measure the *picture*, not the sensor floor). Whole-frame low
  percentiles land at or below the declared black level, so there is nothing to
  subtract.
- **Vendor-interpolated 50 MP still mode.** Interpolated, not real detail, and
  unreachable without a private ODM tag.

# Still active, and staying that way

These are not filters — they are the develop, and removing them would make the
picture *wrong* rather than *raw*:

- Black subtract and normalisation (`bin_isp.slang`), clamping only the top so the
  negative half of zero-mean sensor noise is not clipped into a colour cast.
- White balance from the capture result's neutral.
- **Highlight reconstruction** (`develop_rgb`) — this is what stops a clipped
  photosite rendering as cyan or magenta. Runs unconditionally, independent of
  every switch above.
- CCM to BT.2020 and the PQ ST 2084 transfer.
