# perf-handoff

Everything needed to continue the ASTRO BOT / Demon's Souls performance work on this fork.

| Path | What |
|---|---|
| `CLOUD-AGENT-MASTER-PLAN.md` | Mission, rules, build/test setup, prioritized backlog (W1-W8) for autonomous work |
| `FINDINGS.md` | Hardware, measurements per game and run, root causes, crash analysis, commit ledger with status |
| `LOCAL-TEST-RECIPE.md` | How the owner verifies each change on the real PC, with pass/fail criteria |
| `CLOUD-SESSION-LOG.md` | Running log of agent sessions (update with every commit) |
| `logs/` | Curated measurement lines of every important run (S11-S20 Demon's Souls, X43-X59 ASTRO BOT), crash report, symbolized GPU-thread profiles |
| `docs/` | Earlier plans and the full session worklog (WORKLOG.md), Demon's Souls notes, list of our commits |
| `tools/` | Test and analysis tools used on the PC (drive.py, ram_watchdog.py, route/travel/A-B scripts, stack sampling, RAM breakdown, guest disassembly, pipebench) |
| `build/` | Windows clang-cl build script (reproducible, offline deps cache) |
| `builds/9c52585/` | Windows build of the handoff commit: kyty_emulator.exe, build manifest (SHA-256), zipped PDB |

To run the Windows build: copy `kyty_emulator.exe` over the one in an existing KytyPS5 installation
folder (keep a backup), or use it from `Performance Experiments/builds/` with `tools/drive.py`.
Settings live in `emulator-settings.ini` next to the exe:

```ini
render_resolution=1080p      # legacy DRS steering (ASTRO BOT)
frame_cap=off                # off | 30 | 20
low_memory_stop_mib=400      # stop cleanly before Windows runs out of RAM (0 = off)
texture_quality=full         # full | reduced (opt-in, VRAM: large sampled textures without top mip)
texture_ram=keep             # keep | trim (opt-in, RAM: uploaded CPU-written textures leave RAM)
```
