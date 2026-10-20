"""Neutral exact GetInfo API tests; no actual DNS or service interaction."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="netctl-ipv4-") as directory:
    binary = Path(directory) / "fixture"
    subprocess.run(["cc", "-std=gnu17", "-O2", "-UNDEBUG", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                    "-Iinclude", "-Ilibs/system", "tests/test_netctl_ipv4_override.c", "-o", str(binary)], cwd=root, check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
