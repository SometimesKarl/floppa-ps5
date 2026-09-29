# KytyPS5 performance fork: agent instructions

This fork (branch `local/merge-upstream-0929`) carries performance and stability work for ASTRO BOT
(PPSA21564) and Demon's Souls (PPSA01342) on a Ryzen 5 5500 / RX 6650 XT 8 GB / 16 GB RAM Windows PC.

Before doing anything, read in this order:
1. `perf-handoff/CLOUD-AGENT-MASTER-PLAN.md`: mission, rules, build/test setup, prioritized backlog.
2. `perf-handoff/FINDINGS.md`: measurements, root causes, commit ledger (what is measured, verified,
   unverified, reverted, opt-in).
3. `perf-handoff/CLOUD-SESSION-LOG.md`: where the last agent session stopped.

Key rules (details in the master plan):
- No speed from skipping rendering, lowering quality silently or discarding guest data.
- Behaviour changes that are not provably equivalent go behind opt-in `KYTY_*` / `emulator-settings.ini`
  switches and get an entry in `perf-handoff/LOCAL-TEST-RECIPE.md`.
- Keep Windows (clang-cl) and Linux (clang) builds green; add unit tests under `tests/` where possible.
- Do not report performance you did not measure. The games cannot be run in the cloud.
- Never push to the upstream KytyPS5 repository. Commit small, reviewable steps with the evidence in
  the message, and update `perf-handoff/CLOUD-SESSION-LOG.md` with every commit.
- After any scripted edit, check C string literals for broken `\n` escapes before building.
