# WPOS in sample-grid surfaces

The shared renderer stores antialiased surfaces at a larger sample-grid extent and scales the host viewport accordingly. Fragment `SV_POSITION.xy` consequently measures host samples. Guest WPOS consumers need logical window coordinates, consistent with the logical viewport used by the vertex path.

For generated fragment programs that read WPOS, the engine divides X and Y by the respective sample-grid factors. Z and W remain direct component reads. The factors enter pipeline identity only when this transformation is active. Non-AA shaders, shaders without WPOS, and the fixed pipeline retain their previous keys and source. No constant-buffer layout changes.

This corrects coordinate units in the renderer's current supersampling representation. It does not implement exact hardware multisample locations or change pixel-center/origin conventions.

Run `python3 tools/test_rsx_wpos_scale.py` for synthetic production-engine cache transitions, bounded program walking, generated source, and failure atomicity. The headless Metal fixture `libs/video/tests/test_rsx_wpos_scale.m` consumes four HLSL files emitted by the C fixture when passed an output directory. It checks all pixels of 1x1, 2x1, 1x2, and 2x2 grids, including nontrivial unchanged Z/W. It uses the production HLSL-to-MSL translator and requires a GPU-capable macOS session.
