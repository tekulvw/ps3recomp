# Live D3D12 RGBA32F fragment textures

NV4097 format `0x9B` (`W32_Z32_Y32_X32_FLOAT`) stores four big-endian IEEE-754 binary32 components in X,Y,Z,W order, sampled as R,G,B,A. `0xBB` adds the linear-layout flag. Each source texel occupies 16 bytes. The shared `rsx_texture_layout` and `rsx_texture_decode` rules own pitch, Morton addressing, and per-component byte swapping.

The live fragment-texture path creates `DXGI_FORMAT_R32G32B32A32_FLOAT` resources and views. Finite negative values, fractional values, values above 1, and alpha survive upload without normalization, clipping, or RGBA8 conversion. The existing shared texture crossbar determines selection and forced zero/one; apply it once in the SRV. No game-specific format substitution or shader changes.

Linear texture pitch must cover a complete 16-byte-per-texel row; existing linear mip-level behavior stays unchanged. Swizzled levels use their existing Morton layout. Cube faces use the shared 128-byte aligned face stride. Guest spans must cover the addressed bytes and reject unreadable ranges. The whole texture must fit the remaining upload ring before any GPU copy is recorded. Existing texture cache identity, refresh, immutable picture handling, and checked GPU lifetime rules remain in force.

Validation samples a synthetic guest texture containing negative/HDR values through a real fragment program that maps them into a visible UNORM range. Crossbar selection and constants must be distinguishable from identity and double remapping. This contract establishes generic format support; it makes no claim about any particular title's rendering.

On Windows, build the SDK static runtime with `/MT`, then run in a Visual Studio developer environment:

```
python tools/test_live_draw_rgba32f.py --runtime-library <build>/ps3recomp_runtime.lib
```

The fixture uses synthetic guest bytes, a hidden window and actual D3D12 readback in all four vertex modes. It checks affine sampling, alpha and forced zero/one crossbars, padded rows, Morton layout, mip levels, cube face alignment, source bounds, and refresh. It also checks an ordinary RGBA8 texture through the same shader.
