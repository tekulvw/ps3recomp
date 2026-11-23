#!/usr/bin/env python3
"""include/ps3emu/audio_host_stats.h: one layout and C linkage from C and C++.

The counters themselves are checked against the real mixing thread in
tools/test_posix_audio_pacing.py; this checks that an embedding C++ application
sees the same struct and links the C getter."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LAYOUT = r'''
#include "ps3emu/audio_host_stats.h"
#include <stddef.h>
_Static_assert(sizeof(struct ps3recomp_audio_host_stats) == 88, "size");
_Static_assert(offsetof(struct ps3recomp_audio_host_stats, periods) == 8, "periods");
_Static_assert(offsetof(struct ps3recomp_audio_host_stats, clock_ns) == 64, "clock_ns");
_Static_assert(offsetof(struct ps3recomp_audio_host_stats, backend) == 72, "backend");
_Static_assert(PS3RECOMP_AUDIO_HOST_STATS_VERSION == 1, "version");
'''
GETTER = LAYOUT + r'''
#include <string.h>
int ps3recomp_audio_host_stats(struct ps3recomp_audio_host_stats* out)
{ memset(out, 0, sizeof *out); out->version = 1; out->sample_rate = 48000; out->clock_frames = 480000;
  out->clock_ns = 10000000000ull; strcpy(out->backend, "dummy"); return 0; }
'''
CALLER = LAYOUT.replace('_Static_assert', 'static_assert') + r'''
#include <cstdio>
#include <cstring>
int main() {
    struct ps3recomp_audio_host_stats s;  // the tag names the type; the getter shares the name
    if (ps3recomp_audio_host_stats(&s) != 0 || s.version != 1 || s.sample_rate != 48000) return 1;
    const double drift = double(s.clock_frames) - double(s.clock_ns) * s.sample_rate / 1e9;
    if (drift != 0.0 || std::strcmp(s.backend, "dummy")) return 2;
    std::puts("PASS audio host stats header: C and C++ layout, C linkage");
    return 0;
}
'''


def main():
    cc = os.environ.get('CC', 'clang')
    cxx = os.environ.get('CXX', 'clang++' if 'clang' in cc else 'g++')
    with tempfile.TemporaryDirectory(prefix='audio-host-stats-') as tmp:
        tmp = Path(tmp)
        (tmp / 'getter.c').write_text(GETTER)
        (tmp / 'caller.cpp').write_text(CALLER)
        include = '-I' + str(ROOT / 'include')
        warn = ['-Wall', '-Wextra', '-Werror', '-pedantic']
        subprocess.run([cc, '-std=c11', *warn, include, '-c', str(tmp / 'getter.c'), '-o', str(tmp / 'getter.o')],
                       check=True, timeout=300)
        subprocess.run([cxx, '-std=c++17', *warn, include, str(tmp / 'caller.cpp'), str(tmp / 'getter.o'),
                        '-o', str(tmp / 'caller')], check=True, timeout=300)
        result = subprocess.run([str(tmp / 'caller')], capture_output=True, text=True, timeout=60)
        print(result.stdout, end='')
        if result.returncode:
            raise SystemExit(f'FAIL caller exited {result.returncode}')


if __name__ == '__main__':
    main()
