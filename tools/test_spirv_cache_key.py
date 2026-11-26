#!/usr/bin/env python3
"""PS3RECOMP_SPIRV_CACHE across rebuilds (libs/video/rsx_shader_spirv.cpp, the translator key).

The disk cache's files carry a translator key. CMake sets it to a digest of the translator's
source and binding contract (PS3RECOMP_SPIRV_TRANSLATOR_KEY), so a rebuild of the same translator
keeps the cache warm; a build without it uses its compile time. This builds the translator three
times, a second apart so __TIME__ differs, and checks against one cache directory:
  - key A, then key A again (a rebuild): the second build is served from disk;
  - key B: a miss (another translator), and its file replaces A's;
  - no key, twice: each build misses (the compile-time fallback).
Needs glslang (headers and libraries); skips without it."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
DRIVER = r'''
#include "rsx_shader_spirv.h"
#include <stdio.h>
static const char kPS[] =
    "struct PSInput { float4 pos : SV_Position; float4 col : COLOR0; };\n"
    "float4 main(PSInput i) : SV_Target0 { return i.col.bgra; }\n";
int main(void)
{
    static u32 words[1 << 16];
    u32 n = 0;
    if (rsx_hlsl_to_spirv(kPS, RSX_SHADER_STAGE_FRAGMENT, words, 1u << 16, &n, NULL, 0) || !n) return 2;
    RsxSpirvCacheStats s;
    rsx_hlsl_to_spirv_cache_stats(&s);
    printf("translations=%u disk_hits=%u\n", (unsigned)s.translations, (unsigned)s.disk_hits);
    return 0;
}
'''


def glslang_flags():
    """Compile and link flags for an installed glslang: its include directory, and each of its
    libraries the install has (shared builds have two, static builds several)."""
    for prefix in (os.environ.get('GLSLANG_PREFIX'), '/opt/homebrew', '/usr/local', '/usr'):
        if not prefix or not (Path(prefix) / 'include/glslang/Public/ShaderLang.h').exists():
            continue
        dirs = [Path(prefix) / d for d in ('lib', 'lib64', 'lib/x86_64-linux-gnu', 'lib/aarch64-linux-gnu')]
        dirs = [d for d in dirs if d.is_dir()]
        have = lambda name: any(next(d.glob(f'lib{name}.*'), None) is not None for d in dirs)
        libs = ['-l' + n for n in ('glslang', 'glslang-default-resource-limits', 'SPIRV', 'MachineIndependent',
                                   'GenericCodeGen', 'OSDependent', 'SPIRV-Tools-opt', 'SPIRV-Tools') if have(n)]
        if '-lglslang' in libs:
            return ['-I' + str(Path(prefix) / 'include')], ['-L' + str(d) for d in dirs] + libs
    return None


def main():
    if os.name == 'nt':
        print('SKIP: POSIX toolchain only')
        return
    flags = glslang_flags()
    cxx = shutil.which(os.environ.get('CXX', 'c++')) or shutil.which('clang++') or shutil.which('g++')
    if not flags or not cxx:
        print('SKIP: glslang or a C++ compiler not found')
        return
    cflags, lflags = flags
    with tempfile.TemporaryDirectory(prefix='spirv-cache-key-') as tmp:
        tmp = Path(tmp)
        (tmp / 'driver.cpp').write_text(DRIVER)
        cache = tmp / 'cache'
        cache.mkdir()
        # The translator reports to the build counters (libs/video/rsx_build_counters.h), which
        # live in rsx_fifo_busy.c.
        cc = shutil.which(os.environ.get('CC', 'cc')) or shutil.which('clang') or shutil.which('gcc')
        counters = tmp / 'counters.o'
        subprocess.run([cc, '-std=gnu17', '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'libs/video'), '-c',
                        str(ROOT / 'libs/video/rsx_fifo_busy.c'), '-o', str(counters)], check=True, timeout=300)

        def build(name, key):
            out = tmp / name
            defines = ['-DPS3RECOMP_HAVE_SPIRV_TRANSLATION=1'] + ([f'-DPS3RECOMP_SPIRV_TRANSLATOR_KEY={key}'] if key else [])
            subprocess.run([cxx, '-std=c++17', *defines, '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'libs/video'), *cflags,
                            str(ROOT / 'libs/video/rsx_shader_spirv.cpp'), str(tmp / 'driver.cpp'), str(counters), *lflags, '-pthread',
                            '-o', str(out)], check=True, timeout=600)
            time.sleep(1.1)   # the next build gets another __TIME__
            return out

        def run(binary):
            r = subprocess.run([str(binary)], env=dict(os.environ, PS3RECOMP_SPIRV_CACHE=str(cache)),
                               capture_output=True, text=True, timeout=120)
            if r.returncode:
                raise SystemExit(f'FAIL {binary.name} exited {r.returncode}: {r.stderr}')
            return r.stdout.strip()

        key_a, key_b = 'k' + '0123456789abcdef012345', 'k' + 'fedcba9876543210fedcba'
        checks = [
            ('key A, first build: translated', build('a1', key_a), 'translations=1 disk_hits=0'),
            ('key A, rebuilt: served from disk', build('a2', key_a), 'translations=0 disk_hits=1'),
            ('key B: another translator, a miss', build('b', key_b), 'translations=1 disk_hits=0'),
            ('no key, first build: a miss', build('n1', None), 'translations=1 disk_hits=0'),
            ('no key, rebuilt: a miss again', build('n2', None), 'translations=1 disk_hits=0'),
        ]
        failed = 0
        for what, binary, want in checks:
            got = run(binary)
            ok = got == want
            failed += not ok
            print(f"{'PASS' if ok else 'FAIL'} {what} ({got})")
        if failed:
            raise SystemExit(f'FAIL {failed} check(s)')
        print('PASS spirv cache key: a rebuild of the same translator keeps the cache')


if __name__ == '__main__':
    main()
