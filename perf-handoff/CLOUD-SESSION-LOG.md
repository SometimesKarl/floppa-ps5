# Cloud session log

Format per commit: date/time, commit, work item (W#.#), change, evidence, verification (Linux build,
Windows build if possible, unit tests, review), risk, flag, recipe entry.

## Starting point (local session ended 2026-09-29 ~21:00)
- HEAD 9c52585 on `local/merge-upstream-0929` (upstream KytyPS5 main merged to 59a1760).
- Builds on Windows clang-cl (`perf-handoff/build/build-clangcl-release.cmd`; zstd and ZArchive from a
  local deps cache on the owner's PC). Linux build not tried for our changes yet: do that first.
- Build of HEAD for the owner: `perf-handoff/builds/9c52585/` (exe, manifest, zipped pdb).
- Next: W1.1 (re-entrant fault handler), then the rest of the backlog in order.

## Local follow-up after the handoff (2026-09-29, owner's PC)
- e49dd65 (W1.1 done): fault handler no longer re-enters while reporting (thread-local guard, single
  line then std::_Exit(3)); host backtrace walk uses the no-unwind-data leaf guess only for the first
  frame and checks 512 readable bytes at rsp before each RtlVirtualUnwind. Built on Windows; staged as
  `Performance Experiments/builds/e49dd65-clangcl`. Not exercised (needs a fault). Next agent: W1.2.
