# Cloud session log

Format per commit: date/time, commit, work item (W#.#), change, evidence, verification (Linux build,
Windows build if possible, unit tests, review), risk, flag, recipe entry.

## Starting point (local session ended 2026-09-29 ~21:00)
- HEAD 9c52585 on `local/merge-upstream-0929` (upstream KytyPS5 main merged to 59a1760).
- Builds on Windows clang-cl (`perf-handoff/build/build-clangcl-release.cmd`; zstd and ZArchive from a
  local deps cache on the owner's PC). Linux build not tried for our changes yet: do that first.
- Build of HEAD for the owner: `perf-handoff/builds/9c52585/` (exe, manifest, zipped pdb).
- Next: W1.1 (re-entrant fault handler), then the rest of the backlog in order.
