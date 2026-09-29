# Cloud state (update at every checkpoint)

## Source identity (verified 2026-09-29, cloud checkout)
- Branch `claude/eloquent-meitner-6tzgiw`, base HEAD `9ceebc4` (= `main` = `origin/main`).
- Full performance lineage present: `9c52585` (depth read-only), `e49dd65` (fault handler), handoff docs.
  The unborn-`floppa`-branch concern in the uploaded REVIEW.md does not apply to this checkout.
- Submodules: all 14 initialised (shallow). Toolchain: clang/lld/ninja/cmake, glslang, mesa-vulkan (lavapipe).
- Build dir: `_Build/linux-no-qt` (Release, launcher off). 4 cores, 15 GB RAM: build with `-j3`.

## Build notes
- Re-configuring an existing build dir fails in the zarchive FetchContent step (non-idempotent
  `git apply` PATCH_COMMAND, 3rdparty/CMakeLists.txt). Workaround:
  `git -C _Build/linux-no-qt/_deps/zarchive_source-src checkout -- .` then reconfigure.
- `resource_materialization_tests` / `resource_tracking_tests` did not link `Tracy::TracyClient`, so
  `common/profiler.h` failed to find `tracy/Tracy.hpp` on Linux. Fixed in CMakeLists.txt.

## Status
- W0: build in progress (see CLOUD-TEST-RESULTS.md when done).
- W1.2: analysis done, diagnostics added (see CLOUD-SESSION-LOG.md).
- Next exact step: finish the build, run ctest, record counts, commit.

## Local edits not yet committed
- CMakeLists.txt (Tracy link), src/loader/runtimeLinker.cpp (fault report), these docs.
