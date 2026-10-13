#!/usr/bin/env python3
"""Check the actual diagnostic clock against independent monotonic bounds."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name == "nt":
        print("SKIP POSIX regression; Windows uses its existing GetTickCount64")
        return
    source = (ROOT / "libs/video/cellGcmSys.c").read_text()
    start = source.index("unsigned long long ps3_ms_now(void)")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    body = source[start:end]
    fixture = r'''#include "win32_compat.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
static unsigned long long independent_ms(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
''' + body + r'''
int main(void)
{
    unsigned long long previous = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        unsigned long long before = independent_ms();
        unsigned long long actual = ps3_ms_now();
        unsigned long long after = independent_ms();
        assert(before <= actual && actual <= after);
        assert(actual >= previous);
        previous = actual;
    }
    unsigned long long before = ps3_ms_now();
    struct timespec delay = {0, 25000000};
    assert(nanosleep(&delay, NULL) == 0);
    unsigned long long elapsed = ps3_ms_now() - before;
    assert(elapsed >= 25 && elapsed < 2000);
    puts("PASS: 1000 monotonic millisecond bounds and advancing elapsed time");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="diagnostic-clock-") as tmp:
        path = Path(tmp)
        (path / "clock.c").write_text(fixture)
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-UNDEBUG",
                        "-I" + str(ROOT / "runtime/platform"), str(path / "clock.c"),
                        "-o", str(path / "clock")], check=True, timeout=30)
        subprocess.run([str(path / "clock")], check=True, timeout=5)


if __name__ == "__main__":
    main()
