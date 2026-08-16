#!/usr/bin/env python3
"""Regenerate mac-prototype/r2_assets.py from the spherov2 clone.

Needs reference/sphero-r2d2 present (see README for the clone commands).
Run from the repo root:  python3 tools/gen_r2_assets.py
"""
import subprocess, sys, pathlib
root = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root / "reference/sphero-r2d2"))
try:
    from spherov2.toy.r2d2 import R2D2
except ImportError:
    sys.exit("reference/sphero-r2d2 not found — clone it first (see README)")
sha = subprocess.run(["git", "-C", str(root / "reference/sphero-r2d2"), "rev-parse", "HEAD"],
                     capture_output=True, text=True).stdout.strip()
print(f"spherov2 @ {sha}: {sum(1 for a in R2D2.Audio if a.name.startswith('R2_'))} R2_* sounds, "
      f"{len(list(R2D2.Animations))} animations")
print("This script documents provenance; the generator body lives in the commit that created "
      "r2_assets.py. Re-extract there if spherov2 changes.")
