"""Run a list of run_experiment.py invocations sequentially with a cool-down.

Usage: python run_batch.py BATCH.json
BATCH.json: {"out_root": "...", "cooldown_s": 30, "cwd_template": "...", "common": [...args...],
             "runs": [{"id": "A1", "label": "...", "args": [...], "cwd": optional}, ...]}
Unless a run names its own "cwd", it gets a fresh working directory
<out_root>/cwd/<id> copied from cwd_template (saves only), so every run starts
from the same runtime state with a cold on-disk pipeline cache. Each run's own
arguments are appended after "common". Stops at the first failure.
"""
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
batch = json.loads(Path(sys.argv[1]).read_text())
root = Path(batch["out_root"])
root.mkdir(parents=True, exist_ok=True)
for i, run in enumerate(batch["runs"]):
    if i:
        time.sleep(batch.get("cooldown_s", 30))
    cwd = Path(run["cwd"]) if "cwd" in run else root / "cwd" / run["id"]
    if "cwd" not in run:
        if cwd.exists():
            raise SystemExit(f"{cwd} already exists; refusing to reuse a run directory")
        shutil.copytree(batch["cwd_template"], cwd)
    cmd = [sys.executable, str(TOOLS / "run_experiment.py"), "--label", run["label"],
           "--out", str(root / run["id"]), "--cwd", str(cwd)] + batch["common"] + run.get("args", [])
    print(f"=== {run['id']}: {run['label']}", flush=True)
    r = subprocess.run(cmd)
    if r.returncode != 0:
        print(f"run {run['id']} failed with {r.returncode}", flush=True)
        if not batch.get("continue_on_failure", False):
            sys.exit(r.returncode)
print("batch complete", flush=True)
