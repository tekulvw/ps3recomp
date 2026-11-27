# RSX first-use build counters

`include/ps3emu/rsx_build_stats.h` reports the one-off work a draw can cause the first time the
draw engine sees it, plus texture uploads. An embedding application polls the counters to tell
what a slow frame was spent on, and to compare runs with and without the disk caches below.

## Why

A guest draw with a program/state combination the engine has not seen does a lot of work on the
thread that walks the FIFO:

1. the draw engine decompiles the vertex and fragment programs to HLSL;
2. the backend translates each shader (Vulkan: glslang to SPIR-V, 20-150 ms each; Metal: MSL
   through spirv-cross and a Metal library compile);
3. the host pipeline object is created (Vulkan: `vkCreateGraphicsPipelines`, once per topology,
   at the first draw that uses it; Metal: the render pipeline state, inside step 2's build).

None of this is logged, so a hitch made of it looked like any other slow frame. The counters make
it measurable without a profiler.

## What is counted

Always on. Each counted event reads the monotonic clock twice and does a few relaxed atomic adds.
Nothing is counted per draw or per FIFO method. The counters are cumulative for the process.

| field | counted where | when |
|---|---|---|
| `programs`, `program_ns` | `rsx_draw_engine.c` | a new pipeline key: decompile plus the backend's `pipeline_create` |
| `shader_translations`, `shader_translation_ns` | `rsx_shader_spirv.cpp`, `rsx_metal_backend.m` | a shader actually translated, failures included (not a cache hit) |
| `shader_memory_hits`, `shader_disk_hits` | same | a translation served from memory, or from `PS3RECOMP_SPIRV_CACHE` |
| `pipelines`, `pipeline_ns`, `pipeline_worst_ns` | `rsx_vulkan_backend.c` (every graphics pipeline), `rsx_metal_backend.m` (guest program pipelines) | one host pipeline object created |
| `texture_uploads`, `texture_upload_bytes`, `texture_upload_ns`, `slow_texture_uploads` | `rsx_draw_engine.c` | a texture decoded and uploaded (new, or its contents changed); slow is 4 ms or more |
| `texture_upload_new`, `texture_upload_new_bytes` | `rsx_draw_engine.c` | an upload of a texture the cache did not hold: its first use, or a use after eviction |
| `texture_upload_changed`, `texture_upload_changed_bytes`, `texture_upload_in_place` | `rsx_draw_engine.c` | an upload because a cached texture's guest bytes changed; in place when it went into the texture the cache already had (`texture_reusable`, below) |
| `texture_evictions` | `rsx_draw_engine.c` | the texture cache (1024 entries) evicted its least recently used entry |
| `texture_upload_size_n[4]`, `texture_upload_size_bytes[4]` | `rsx_fifo_busy.c` | every upload by guest size: up to 64 KiB, up to 1 MiB, up to 4 MiB, larger |
| `texture_upload_wait_ns` | `rsx_draw_engine.c` | of `texture_upload_ns`, the time those uploads waited for the GPU to free a recording slot |
| `gpu_slot_waits`, `gpu_slot_wait_ns` | `rsx_vulkan_backend.c` | every wait for a batch slot under batched recording, whatever the work |
| `pipeline_cache_saves`, `pipeline_cache_save_ns` | `rsx_vulkan_backend.c` | the `PS3RECOMP_VK_PIPELINE_CACHE` file written (every 32 new pipelines, and at shutdown) |
| `pipeline_cache_loaded_bytes` | `rsx_vulkan_backend.c` | cache bytes loaded at device creation (0: empty, refused or off) |
| `flags` | | `RSX_BUILD_FLAG_SPIRV_DISK_CACHE`, `RSX_BUILD_FLAG_VK_PIPELINE_CACHE` |
| `backend` | | `"vulkan"` or `"metal"` once that device is up |

Version 2 appended the texture fields after `backend`; a caller built against version 1 reads the
same first 144 bytes. `texture_uploads` is `texture_upload_new` plus `texture_upload_changed`.
`texture_upload_wait_ns` is part of `texture_upload_ns`, not added to it: an upload that opens a new
Vulkan batch while every slot is still on the GPU waits there, which is GPU backpressure rather than
upload work.

The `*_ns` fields are exclusive and add up. A program's time leaves out the translations and
pipeline creations inside it, which a per-thread accumulator subtracts (`rsx_build_inner_ns`). On
Vulkan, program time covers decompiling and shader-module creation. On Metal, it covers
decompiling and the vertex descriptor.

The D3D12 backend is not instrumented. Under it, only the draw engine's programs and texture
uploads count.

The counters live in `libs/video/rsx_fifo_busy.c`. Every program that links the draw engine or a
host backend already links that file, so no build or test needs another source.

## The disk caches these measure

Both are opt-in and off by default.

- `PS3RECOMP_SPIRV_CACHE=<existing directory>`: glslang's output, one file per shader, keyed by the
  stage and the whole HLSL text. A file from another glslang version or translator build is a miss.
  When the Vulkan backend's guest programs come up, a worker reads every usable file into memory
  (`rsx_hlsl_to_spirv_cache_preload_start`), so a shader an earlier session translated is a memory
  hit at first draw instead of a file read on the FIFO thread; a draw that comes before the worker
  reaches its file reads it as before. It logs `[RSX spirv] cache preload: N shaders (K KB) in M ms,
  S skipped`. `PS3RECOMP_SPIRV_CACHE_PRELOAD=0` turns it off. `RsxSpirvCacheStats` counts what it
  loaded, which entries requests then used, and the time requests still spent reading files.
- `PS3RECOMP_VK_PIPELINE_CACHE=<existing directory>`: one `VkPipelineCache`, in
  `vk-pipelines-<vendor>-<device>.bin`. A file from another device or driver build is not loaded.

A port can fill the SPIR-V cache before a title runs (`rsx_shader_prewarm.h`, Vulkan builds):
`rsx_shader_prewarm_vertex` and `rsx_shader_prewarm_fragment` take a program and a described draw
state, make the text with the same builders the engine uses (`rsx_vp_build_text`,
`rsx_fp_build_text`) and translate it through `rsx_hlsl_to_spirv`, so the file they write is the
one the engine's draw of that program in that state reads. Per-unit state reaches the text only on
units the program samples, so a texture left bound on another unit names the same file.
`rsx_hlsl_to_spirv_cache_name` gives a text's file name and `rsx_hlsl_to_spirv_cache_file_current`
says whether a file is this translator's (a file from another translator is safe to delete).

## Tests

- `tools/test_rsx_build_stats_header.py`: the struct layout from C and C++, C linkage, and each
  counting rule against the real counters.
- `libs/video/tests/test_shader_spirv.c`: glslang runs, memory and disk hits and the disk-cache flag;
  the preload (used, skipped and racing files, entries already in memory, the off switch).
- `libs/video/tests/test_shader_prewarm.c`: a warmed program's file is the one its text names, the
  next process reads it, and stale and foreign files are told apart.
- `libs/video/tests/test_engine_shader_text.c`: the engine's text equals the builders' text from the
  raw draw state, and unsampled cube, sRGB and vertex-texture state change neither text nor key.
- `libs/video/tests/test_rsx_draw_engine.c`, "build counters": through the engine, one program
  and one upload for a first draw, nothing for repeats, and another program for a new key.
