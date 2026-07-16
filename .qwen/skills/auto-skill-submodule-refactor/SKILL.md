---
name: submodule-refactor
description: Reorganize project structure with git submodules and CMake libraries while preserving build integrity
source: auto-skill
extracted_at: '2026-07-16T23:02:00.158Z'
---

# Refactoring Projects with Git Submodules and CMake Libraries

When reorganizing a project's library structure (e.g., separating first-party from third-party dependencies), follow this approach to maintain build integrity.

## Key Steps

### 1. Move Submodules with `git mv`
```bash
mkdir -p libs/firstparty libs/thirdparty
git mv libs/old_path/submodule libs/firstparty/submodule
```

**Critical**: After moving, submodules become empty directories. You must re-initialize them:
```bash
git submodule sync
git submodule update --init --force <moved-submodule-path>
```

### 2. Handle Nested Submodules
When a submodule contains its own submodules (e.g., `Vk_Canvas_Lb_LAW` containing `vulkan_font_engine`), initialize them recursively:
```bash
git submodule update --init --recursive libs/firstparty/Vk_Canvas_Lb_LAW
```

### 3. Update CMakeLists.txt Paths
When replacing a library with a new version that has a different structure:

**Old structure** (individual source files):
```cmake
set(CANVAS_DIR ${LIBS_DIR}/vulkan_canvas_engine/app/src/main)
add_library(camera_recorder SHARED
    ${CANVAS_DIR}/cpp/renderer.cc
    ${CANVAS_DIR}/cpp/canvas.cc
    # ... more individual files
)
```

**New structure** (CMake library target):
```cmake
set(CANVAS_DIR ${LIBS_DIR}/firstparty/Vk_Canvas_Lb_LAW)
add_subdirectory(${CANVAS_DIR}/core ${CMAKE_BINARY_DIR}/vk_canvas_core)

add_library(camera_recorder SHARED
    # ... your sources
)
target_link_libraries(camera_recorder vk_canvas_core)
```

### 4. Update Include Paths
Adjust `target_include_directories` to match the new structure:
```cmake
# Old
${CANVAS_DIR}/cpp
${FONT_DIR}/cpp

# New
${CANVAS_DIR}/core
${CANVAS_DIR}/first_party/vulkan_font_engine/core
```

### 5. Update Build Scripts
Search for hardcoded paths in build scripts (`.ps1`, `.bat`, `.sh`) and update them:
```powershell
# Old
$so = Join-Path $root "libs/onnxruntime/lib/arm64-v8a/libonnxruntime.so"

# New
$so = Join-Path $root "libs/thirdparty/onnxruntime/lib/arm64-v8a/libonnxruntime.so"
```

### 6. Update Documentation
Update references in `README.md`, `CLAUDE.md`, `GUIDE.md`, etc. to reflect:
- New library names (e.g., `vulkan_canvas_engine` → `Vk_Canvas_Lb_LAW`)
- New directory structure (`libs/firstparty/`, `libs/thirdparty/`)

### 7. Validate the Build
Run the full build to catch any remaining issues:
```bash
gradlew.bat assembleDebug assembleRelease
```

## Common Pitfalls

1. **Empty submodules after move**: `git mv` moves the directory but not the content. Always run `git submodule update --init --force` after moving.

2. **Stale commit references**: Some submodules may reference commits that no longer exist. Check with `git submodule status` and update if needed.

3. **Git-ignored prebuilts**: Some libraries (like `libonnxruntime.so`) may be git-ignored and need to be built from source or obtained separately.

4. **Nested submodule initialization**: Submodules within submodules need explicit initialization with `--recursive`.

5. **CMake cache**: After major structure changes, delete `.cxx/` and `build/` directories to force a clean CMake reconfiguration.

## When to Use Library Targets vs Individual Sources

**Use library targets** (`add_subdirectory` + `target_link_libraries`) when:
- The library provides a proper CMakeLists.txt
- You want cleaner dependency management
- The library handles its own compilation flags

**Use individual sources** when:
- You need specific files not included in the library target
- The library marks certain files as "WIP" or experimental
- You're integrating legacy code without CMake support

## Replacing a Library with a New Version

When a library is replaced entirely (e.g., `vulkan_canvas_engine` → `Vk_Canvas_Lb_LAW`):

### Remove the old submodule
```bash
git submodule deinit -f libs/old_lib
git rm -f libs/old_lib
```

### Add the new submodule
```bash
git submodule add https://github.com/org/new_lib libs/firstparty/new_lib
```

### API Migration

New library versions often introduce platform abstraction seams. For example, a library might change from:
```cpp
// Old: raw pointers
Renderer(ANativeWindow* window, AAssetManager* assets);
renderer->set_wake_looper(looper);
```

To a platform-agnostic interface:
```cpp
// New: abstract interfaces
Renderer(SurfaceProvider& surface, AssetReader& assets);
// Platform adapters in platform/android/android_platform.hh:
//   AndroidSurfaceProvider, AndroidAssetReader, AndroidFrameWaker
```

**Migration steps:**
1. Read the new library's headers to understand the interface (check `core/` or `platform/` directories)
2. Add `#include` for the platform adapter headers
3. Store adapter objects as members (they must outlive the Renderer)
4. Create adapters in initialization code:
   ```cpp
   surface_provider_ = new AndroidSurfaceProvider(state_->window);
   asset_reader_     = new AndroidAssetReader(state_->activity->assetManager);
   renderer_ = new Renderer(*surface_provider_, *asset_reader_);
   ```
5. Clean up adapters in destruction code (after deleting the consumer)

### Shader Compilation Changes

New libraries may split shader compilation into separate targets:
```cmake
# Old: single shader target
vce_compile_slang(compile_shaders ${SHADER_OUT_DIR} ${FONT_DIR}/shaders_src
    composite_vert composite_frag tiling coverage overlay_vert overlay_frag)

# New: separate font and canvas shaders
vce_compile_slang(compile_font_shaders ${SHADER_OUT_DIR}
    ${CANVAS_DIR}/first_party/vulkan_font_engine/shaders_src
    composite_vert composite_frag tiling coverage)
vce_compile_slang(compile_canvas_shaders ${SHADER_OUT_DIR}
    ${CANVAS_DIR}/shaders_src
    overlay_vert overlay_frag image_vert image_frag shape_vert shape_frag)

add_dependencies(camera_recorder compile_font_shaders compile_canvas_shaders)
```

### Function Signature Changes

Check for changed function signatures in helper utilities:
```cpp
// Old
vce::platform::enable_immersive(app);

// New (added enum parameter)
vce::platform::enable_immersive(app, vce::platform::ImmersiveMode::kFullImmersive);
```

## Non-Submodule Libraries

Not all libraries under `libs/` are git submodules. Some are vendored source or prebuilt binaries:
- **Vendored source** (e.g., `kiss_fft`, `soxr`): regular directories, use `move` not `git mv`
- **Prebuilt imports** (e.g., `onnxruntime`): may have git-ignored `.so`/`.dll` files

```bash
# For non-submodule directories
move "libs\kiss_fft" "libs\thirdparty\kiss_fft"
move "libs\onnxruntime" "libs\thirdparty\onnxruntime"
```

## Custom Git Wrappers

Some projects use custom git wrappers (e.g., `git_wrapper.exe`) for:
- Enforcing commit identity (specific author/email)
- Pushing submodules before the main repo
- Stripping unwanted trailers from commit messages

Always check if the project has a custom wrapper and use it for commits/pushes:
```bash
project_root\git_wrapper.exe save "commit message"
# or
project_root\git_wrapper.exe commit "message"
project_root\git_wrapper.exe push
```
