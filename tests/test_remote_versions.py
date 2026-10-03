#!/usr/bin/env python3
"""Run the production version parser/scorer on a host without the PS5 SDK."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="cheatrunner-versions-") as tmp:
    executable = Path(tmp) / "remote-versions"
    dead_strip = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    # Drop unrelated PS5/network code while testing the actual source functions.
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-ffunction-sections", "-fdata-sections", dead_strip,
        "-I", str(root / "src"),
        str(root / "tests/remote_versions.c"),
        str(root / "src/cr_remote_sources.c"),
        str(root / "src/cr_version.c"),
        "-o", str(executable),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)
