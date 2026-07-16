# Third-Party Notices

This project (`io.nava.camera`) is licensed under the **GNU Affero General Public
License v3.0** — see [`LICENSE`](LICENSE).

It incorporates the third-party components listed below. Each remains under its
own license; all of them are compatible with the AGPLv3 under which the combined
work is distributed. Their original license texts are retained in their
respective source trees (paths in parentheses) and must be preserved in any
redistribution.

| Component | License | Notes |
|-----------|---------|-------|
| FreeType | FreeType License (FTL), or GPLv2 at your option | GPLv3-compatible. FTL requires crediting the FreeType Project in product documentation. (`libs/firstparty/Vk_Canvas_Lb_LAW/first_party/vulkan_font_engine/third_party/freetype/LICENSE.TXT`) |
| libebml | LGPL-2.1 | (`libs/thirdparty/libebml/LICENSE.LGPL`) |
| libmatroska | LGPL-2.1 | (`libs/thirdparty/libmatroska/LICENSE.LGPL`) |
| libFLAC | BSD-3-Clause (Xiph) | Only the library is linked; the `flac`/`metaflac` CLI tools (GPL) are not used. (`libs/thirdparty/flac/COPYING.Xiph`) |
| ncnn | BSD-3-Clause | Tencent. (`libs/thirdparty/ncnn/LICENSE.txt`) |
| rnnoise | BSD-3-Clause | Xiph; submodule present but currently unused. (`libs/thirdparty/rnnoise/COPYING`) |
| libusb | LGPL-2.1 | Bundled via the `audio_engine` submodule. (`libs/firstparty/audio_engine/src/main/cpp/libusb/COPYING`) |
| kiss_fft | BSD-3-Clause | Vendored source. (`libs/thirdparty/kiss_fft`) |
| soxr (SoX Resampler) | LGPL-2.1 | Vendored source. (`libs/thirdparty/soxr/LICENCE`) |
| ONNX Runtime | MIT | Microsoft; prebuilt `.so` import. (`libs/thirdparty/onnxruntime`) |
| stb_image_write | Public Domain / MIT (dual) | Vendored. (`camera/src/main/cpp/third_party/stb_image_write.h`) |
| TinyDNGWriter | MIT | Syoyo Fujita; vendored. (`camera/src/main/cpp/third_party/tiny_dng_writer.h`) |
| DeepFilterNet ONNX models | MIT / Apache-2.0 | Bundled denoiser weights. (`camera/src/main/assets/models/`) |

## LGPL components (libusb, libebml, libmatroska, soxr)

These are statically linked into `libcamera_recorder.so`. The LGPL's relinking
requirement (LGPL-2.1 §6) is satisfied because the complete corresponding source
for the combined work is published under the AGPLv3.

## Build-time tools (not redistributed)

The Vulkan SDK / `slangc` shader compiler and the Android NDK are used only to
build the project and are not distributed as part of it.
