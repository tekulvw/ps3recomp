#!/usr/bin/env python3
"""Check native key events and guest pad reports without showing any windows."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

if sys.platform != "darwin":
    print("SKIP: native keyboard events require AppKit")
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="macos-keyboard-layout-") as tmp:
    tmp = Path(tmp)
    flags = ["-std=gnu17", "-O1", "-UNDEBUG", "-I", str(root / "include")]
    sdl = subprocess.check_output(["sdl2-config", "--cflags", "--libs"], text=True).split()
    flags += ["-I" + str(Path(x[2:]).parent) for x in sdl
              if x.startswith("-I") and Path(x[2:]).name == "SDL2"]
    subprocess.run(["clang", *flags, "-c", str(root / "libs/input/cellPad.c"),
                    *[x for x in sdl if x.startswith(("-I", "-D"))],
                    "-o", str(tmp / "pad.o")], check=True)
    subprocess.run(["clang", *flags, "-fobjc-arc",
                    str(root / "libs/input/tests/test_macos_keyboard_layout.m"),
                    str(root / "libs/input/pad_macos.m"), str(tmp / "pad.o"),
                    *sdl, "-framework", "AppKit", "-o", str(tmp / "test")], check=True)
    # Clear optional input injection so inherited development settings cannot
    # create false positives or interfere with the arbitration checks.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("PAD_", "YDKJ_"))}
    for args, extra in [([], {}), (["controller"], {}),
                        (["disabled"], {"PAD_NO_KEYBOARD": "1"})]:
        subprocess.run([str(tmp / "test"), *args], env={**env, **extra},
                       check=True, timeout=20)
