# Claude review handoff

## Repository state

- Repository: `simipoB/Galar-Native-Android`
- Branch: `simipob-fix-white-screen`
- Remote tip: `db7f7d7`
- Remote branch: `origin/simipob-fix-white-screen`
- The worktree was clean when this handoff was written.
- Commits in this change:
  - `aefddf9` — startup/Vulkan hardening, fixed 30/60 pacing, performance-hint control, frame-generation groundwork, and focused tests.
  - `db4fc90` — make the Vulkan validation smoke test diagnostic and remove redundant forced layer activation.
  - `db7f7d7` — treat `VK_ERROR_INCOMPATIBLE_DRIVER` as a successful skip when CI has no Vulkan ICD.

## What changed

### White-screen and startup hardening

- `dm/android/app/jni/android_main.c`
  - Fails closed when required game files are absent.
  - Reports host-thread creation failure instead of silently continuing.
  - Adds `DM_PERF_HINT=off` as an opt-out for Android Performance Hint scheduling.
- `dm/gfx/nvn/nvn_vk.c`
  - Checks surface support, surface capabilities, formats, present modes, usage flags, swapchain images, and image-view creation.
  - Cleans up partially-created swapchain resources on failure.
  - Uses the absolute-deadline pacer instead of drifting accumulated present timestamps.
  - Uses the shared BC upload-slot sizing helper.
- `dm/gfx/nvn/tex_upload_layout.h`
  - Preserves the larger raw BC block size for staging slots. This prevents 1x1 BC6H cube-map layers from overlapping before GPU reads complete, the known Mali white-screen cause.

### Timing and performance

- `dm/gfx/nvn/hle_nvn.c`
  - Explicit live 30 FPS uses two 60 Hz simulation slots.
  - Explicit live 60 FPS uses one slot.
  - Hitches no longer change the selected simulation cadence.
- Existing mailbox presentation, Android display-rate selection, performance hints, optional fastest-core affinity, and game render-scale cap remain the default optimization paths.

### Tests and CI

- Startup failure-path tests: `dm/android/app/jni/tests/t_startup_gate.c`
- Swapchain cleanup tests: `dm/gfx/tests/t_swapchain_guard.c`
- BC staging regression: `dm/gfx/tex/tests/t_upload_layout.c`
- Vulkan smoke test: `dm/gfx/tests/t_vulkan_validation.c`
- CI workflow: `.github/workflows/native-validation.yml`
- Passing GitHub Actions run: `42879347947`
- Android native build passed with 141 compiled units and produced `libnxnative.so`.

### Frame-generation groundwork

- `dm/gfx/nvn/nvn_fgen.h`
- `dm/gfx/nvn/overlay/fg_*.frag`
- `dm/gfx/nvn/overlay/fg.vert`
- `dm/gfx/nvn/overlay/fgen_spv.h`
- `dm/gfx/tests/t_fgen.c`
- `dm/gfx/tests/t_fgen_vk.c`

This is an isolated experimental module and software-Vulkan-tested groundwork. It is **not wired into the production present path** and must not be described as shipped frame generation.

## Validation and limitations

- Focused startup, swapchain, BC upload, frame-step, pacing, and frame-generation tests passed during development.
- GitHub Actions native validation passed in run `42879347947`.
- A fresh test APK containing the rebuilt native library installed and launched on Samsung `SM-S936W` (`R3CY10A70CV`) without an Android crash.
- The test APK intentionally contained no game dump or encryption keys, so it could not exercise real gameplay or prove the post-setup Vulkan white-screen path on-device.
- The oversized menu/Pokemon view remains unresolved.
- No production frame-generation integration exists.

## Review boundaries

- Do not request or transfer commercial Switch dumps, encryption keys, or private game data.
- Review the pushed branch, not only `origin/main`.
- Pay particular attention to partial swapchain cleanup, fixed-rate frame stepping, BC staging-size invariants, and the fact that frame generation is not integrated.
- Treat the CI Vulkan smoke test's no-ICD path as an environment skip, not as evidence of runtime Vulkan support on Android.
