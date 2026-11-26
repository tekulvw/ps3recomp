#!/usr/bin/env python3
"""include/ps3emu/rsx_build_stats.h: one layout and C linkage from C and C++, and the counting
rules of libs/video/rsx_build_counters.h (docs/RSX_BUILD_STATS.md).

A C++ caller, as an embedding application is, links the real counters (libs/video/rsx_fifo_busy.c)
and checks what each note adds: the slow-upload threshold, the worst pipeline, cache hits, the
pipeline-cache saves, the calling thread's nested translation and pipeline time, flags and the
backend name (first one wins). The draw engine's own calls are
checked in libs/video/tests/test_rsx_draw_engine.c ("build counters")."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CALLER = r'''
#include "ps3emu/rsx_build_stats.h"
#include "rsx_build_counters.h"
#include <cstddef>
#include <cstdio>
#include <cstring>
static_assert(sizeof(struct ps3recomp_rsx_build_stats) == 144, "size");
static_assert(offsetof(struct ps3recomp_rsx_build_stats, programs) == 8, "programs");
static_assert(offsetof(struct ps3recomp_rsx_build_stats, pipelines) == 56, "pipelines");
static_assert(offsetof(struct ps3recomp_rsx_build_stats, texture_uploads) == 80, "texture_uploads");
static_assert(offsetof(struct ps3recomp_rsx_build_stats, pipeline_cache_save_ns) == 128, "save_ns");
static_assert(offsetof(struct ps3recomp_rsx_build_stats, backend) == 136, "backend");
static_assert(PS3RECOMP_RSX_BUILD_STATS_VERSION == 1, "version");
#define T(c) do { if (!(c)) { std::printf("FAIL %s (line %d)\n", #c, __LINE__); return 1; } } while (0)
int main() {
    struct ps3recomp_rsx_build_stats s;
    T(ps3recomp_rsx_build_stats(&s) == 0 && s.version == 1 && s.programs == 0 && s.backend[0] == 0);
    const uint64_t a = rsx_build_now_ns(), b = rsx_build_now_ns();
    T(a > 0 && b >= a);
    rsx_build_note(RSX_BUILD_PROGRAM, 30000000, 0);
    rsx_build_note(RSX_BUILD_TRANSLATION, 20000000, 0);
    rsx_build_note(RSX_BUILD_TRANSLATION, 5000000, 0);
    rsx_build_note(RSX_BUILD_PIPELINE, 7000000, 0);
    rsx_build_note(RSX_BUILD_PIPELINE, 90000000, 0);
    rsx_build_note(RSX_BUILD_PIPELINE, 1000000, 0);
    rsx_build_note(RSX_BUILD_TEXTURE_UPLOAD, RSX_BUILD_SLOW_UPLOAD_NS - 1, 4096);
    rsx_build_note(RSX_BUILD_TEXTURE_UPLOAD, RSX_BUILD_SLOW_UPLOAD_NS, 1u << 20);
    rsx_build_note(RSX_BUILD_EVENT_COUNT, 1, 1);   /* not an event: ignored */
    rsx_build_note(-1, 1, 1);
    rsx_build_note_shader_hit(0); rsx_build_note_shader_hit(0); rsx_build_note_shader_hit(1);
    rsx_build_note_pipeline_cache_save(3000000);
    rsx_build_set_flag(RSX_BUILD_FLAG_VK_PIPELINE_CACHE);
    rsx_build_set_flag(RSX_BUILD_FLAG_SPIRV_DISK_CACHE);
    rsx_build_set_pipeline_cache_loaded(123456);
    rsx_build_set_backend("vulkan");
    rsx_build_set_backend("metal");   /* the first name stays */
    ps3recomp_rsx_build_stats(&s);
    T(s.programs == 1 && s.program_ns == 30000000);
    T(s.shader_translations == 2 && s.shader_translation_ns == 25000000);
    T(s.shader_memory_hits == 2 && s.shader_disk_hits == 1);
    T(s.pipelines == 3 && s.pipeline_ns == 98000000 && s.pipeline_worst_ns == 90000000);
    T(s.texture_uploads == 2 && s.texture_upload_bytes == 4096 + (1u << 20) && s.slow_texture_uploads == 1);
    T(s.texture_upload_ns == 2 * RSX_BUILD_SLOW_UPLOAD_NS - 1);
    T(s.pipeline_cache_saves == 1 && s.pipeline_cache_save_ns == 3000000);
    T(rsx_build_inner_ns() == 25000000 + 98000000);   /* translations + pipelines, this thread */
    T(s.pipeline_cache_loaded_bytes == 123456);
    T(s.flags == (RSX_BUILD_FLAG_VK_PIPELINE_CACHE | RSX_BUILD_FLAG_SPIRV_DISK_CACHE));
    T(std::strcmp(s.backend, "vulkan") == 0);
    rsx_build_stats_reset();
    rsx_build_set_backend("a-name-longer-than-seven");
    ps3recomp_rsx_build_stats(&s);
    T(s.programs == 0 && s.pipelines == 0 && s.pipeline_worst_ns == 0 && s.flags == 0 && rsx_build_inner_ns() == 0);
    T(std::strcmp(s.backend, "a-name-") == 0);
    std::puts("PASS rsx build stats: C and C++ layout, C linkage, counting rules");
    return 0;
}
'''


def main():
    cc = os.environ.get('CC', 'clang')
    cxx = os.environ.get('CXX', 'clang++' if 'clang' in cc else 'g++')
    with tempfile.TemporaryDirectory(prefix='rsx-build-stats-') as tmp:
        tmp = Path(tmp)
        (tmp / 'caller.cpp').write_text(CALLER)
        includes = ['-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'libs/video')]
        warn = ['-Wall', '-Wextra', '-Werror']
        subprocess.run([cc, '-std=gnu17', *warn, *includes, '-c', str(ROOT / 'libs/video/rsx_fifo_busy.c'),
                        '-o', str(tmp / 'counters.o')], check=True, timeout=300)
        subprocess.run([cxx, '-std=c++17', *warn, '-pedantic', *includes, str(tmp / 'caller.cpp'),
                        str(tmp / 'counters.o'), '-pthread', '-o', str(tmp / 'caller')], check=True, timeout=300)
        result = subprocess.run([str(tmp / 'caller')], capture_output=True, text=True, timeout=60)
        print(result.stdout, end='')
        if result.returncode:
            raise SystemExit(f'FAIL caller exited {result.returncode}')


if __name__ == '__main__':
    main()
