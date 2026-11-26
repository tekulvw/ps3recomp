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
| `shader_translations`, `shader_translation_ns` | `rsx_shader_spirv.cpp`, `rsx_metal_backend.m` | a shader actually translated (not a cache hit) |
| `shader_memory_hits`, `shader_disk_hits` | same | a translation served from memory, or from `PS3RECOMP_SPIRV_CACHE` |
| `pipelines`, `pipeline_ns`, `pipeline_worst_ns` | `rsx_vulkan_backend.c` (every graphics pipeline), `rsx_metal_backend.m` (guest program pipelines) | one host pipeline object created |
| `texture_uploads`, `texture_upload_bytes`, `texture_upload_ns`, `slow_texture_uploads` | `rsx_draw_engine.c` | a texture decoded and uploaded (new, or its contents changed); slow is 4 ms or more |
| `pipeline_cache_saves`, `pipeline_cache_save_ns` | `rsx_vulkan_backend.c` | the `PS3RECOMP_VK_PIPELINE_CACHE` file written (every 32 new pipelines, and at shutdown) |
| `pipeline_cache_loaded_bytes` | `rsx_vulkan_backend.c` | cache bytes loaded at device creation (0: empty, refused or off) |
| `flags` | | `RSX_BUILD_FLAG_SPIRV_DISK_CACHE`, `RSX_BUILD_FLAG_VK_PIPELINE_CACHE` |
| `backend` | | `"vulkan"` or `"metal"` once that device is up |

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
- `PS3RECOMP_VK_PIPELINE_CACHE=<existing directory>`: one `VkPipelineCache`, in
  `vk-pipelines-<vendor>-<device>.bin`. A file from another device or driver build is not loaded.

## Tests

- `tools/test_rsx_build_stats_header.py`: the struct layout from C and C++, C linkage, and each
  counting rule against the real counters.
- `libs/video/tests/test_rsx_draw_engine.c`, "build counters": through the engine, one program
  and one upload for a first draw, nothing for repeats, and another program for a new key.
