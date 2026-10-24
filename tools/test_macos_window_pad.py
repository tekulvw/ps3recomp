#!/usr/bin/env python3
"""Exercise the production Cocoa view and real big-endian cellPad reports."""
from pathlib import Path
import os, subprocess, sys, tempfile
if sys.platform != "darwin":
    print("SKIP: macOS window input requires AppKit and Metal")
    raise SystemExit(0)
root=Path(__file__).resolve().parents[1]
def run_fixture(args, env=None):
    try:
        subprocess.run(args,env=env,check=True,timeout=20)
    except subprocess.CalledProcessError as error:
        # Preserve a native backtrace on CI before the temporary executable is
        # removed. The fixture is rerun only after failure, never skipped.
        if error.returncode < 0:
            subprocess.run(["lldb","--batch","-o","run","-k","thread backtrace all","--",*args],
                           env=env,check=False,timeout=60)
        raise

with tempfile.TemporaryDirectory(prefix="macos-pad-") as tmp:
    tmp=Path(tmp)
    flags=["-std=gnu17","-O1","-g","-UNDEBUG","-ffunction-sections","-fdata-sections","-I",str(root/"include")]
    sdl=subprocess.check_output(["sdl2-config","--cflags","--libs"],text=True).split()
    flags += ["-I"+str(Path(x[2:]).parent) for x in sdl if x.startswith("-I") and Path(x[2:]).name=="SDL2"]
    subprocess.run(["clang",*flags,"-c",str(root/"libs/input/cellPad.c"),*filter(lambda x:x.startswith("-I") or x.startswith("-D"),sdl),"-o",str(tmp/"pad.o")],check=True)
    cmd=["clang",*flags,"-fobjc-arc", "-DPS3RECOMP_DISABLE_NATIVE_OSK",str(root/"libs/input/tests/test_macos_window_pad.m"),str(tmp/"pad.o")]
    bridge=root/"libs/input/pad_macos.m"
    if bridge.exists(): cmd.append(str(bridge))
    cmd += [*sdl,"-framework","AppKit","-framework","Metal","-framework","QuartzCore","-Wl,-dead_strip","-o",str(tmp/"test")]
    subprocess.run(cmd,check=True,timeout=60)
    if "--build-only" in sys.argv:
        print("native Metal window input fixture compiled (not launched)")
        raise SystemExit(0)
    run_fixture([str(tmp/"test")])
    run_fixture([str(tmp/"test"),"controller"])
    run_fixture([str(tmp/"test"),"disabled"],env={**os.environ,"PAD_NO_KEYBOARD":"1"})
