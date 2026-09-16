#!/usr/bin/env python3
"""Validate the CI workflow YAML before pushing.

Why this exists: every failed push costs a full CI round-trip. Checking the
workflow locally is far cheaper than discovering a YAML typo or a missing
matrix key on GitHub.

Usage (from the redis-lite directory):
    python3 scripts/validate_workflow.py
"""
import os
import sys

try:
    import yaml
except ImportError:
    print("PyYAML not installed; skipping (pip install pyyaml to enable)")
    sys.exit(0)

# The workflow lives at the repository root, two levels above this project dir.
_default = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "..", ".github", "workflows", "ci.yml")
path = sys.argv[1] if len(sys.argv) > 1 else _default

with open(path) as f:
    doc = yaml.safe_load(f)

print("YAML parse: OK")

jobs = doc.get("jobs", {})
print(f"jobs ({len(jobs)}): {list(jobs.keys())}")

build = jobs.get("build", {})
matrix = build.get("strategy", {}).get("matrix", {}).get("include", [])
print(f"matrix entries: {len(matrix)}")
for e in matrix:
    missing = [k for k in ("os", "cc", "cxx", "pkg", "label") if k not in e]
    flag = "  <-- MISSING " + ",".join(missing) if missing else ""
    print(f"   {e.get('label'):<18} {e.get('os'):<14} {e.get('cxx'):<12} pkg={e.get('pkg')}{flag}")

print(f"build.runs-on: {build.get('runs-on')}")
print(f"build.fail-fast: {build.get('strategy', {}).get('fail-fast')}")

# Report whether each job pins an explicit working-directory, since the project
# lives in a subdirectory and forgetting it silently breaks every run step.
for name, job in jobs.items():
    wd = job.get("defaults", {}).get("run", {}).get("working-directory")
    print(f"   job {name:<12} working-directory={wd}")

required_steps = {
    "sanitizer": ["Shutdown path under sanitizers", "Unit tests under sanitizers"],
}
problems = []
for job_name, steps in required_steps.items():
    actual = [s.get("name", "") for s in jobs.get(job_name, {}).get("steps", [])]
    for want in steps:
        if want not in actual:
            problems.append(f"{job_name} is missing step: {want}")

if problems:
    print("\nPROBLEMS:")
    for p in problems:
        print("  -", p)
    sys.exit(1)

print("\nworkflow structure: OK")
