# SPU registration trace

`SPU_REGISTRY_TRACE=1` enables a bounded startup diagnostic. Every other value, including unset, leaves output and registry behavior unchanged. Registration remains governed by the existing APIs and their return values; the trace must never admit, reject, execute or alter guest code.

Each registration batch ends when `spu_begin_image` is called again. Its summary identifies the image and batch, counts successful function insertions, duplicate address/image pairs, null function insertions, function capacity failures, and verified-region insertions/rejections. A verified-region row reports the actual insertion result, image, content EA, load LSA, span, and segment count. Rows include the current batch's function counts only when its image matches. Duplicate insertions retain the existing API behavior and are marked explicitly.

Emit success only after the existing insertion completes. Rejected region calls and dropped function insertions remain visible. Limit diagnostics to 512 rows plus one limit marker per process; truncation means coverage cannot be certified. No guest bytes or function-pointer values are printed.

These rows establish registration in the exact binary that emitted them. They do not establish dispatch, execution, completion or gameplay. Compare batch insertion counts with the generated registration table, region identities with its manifest, and keep binary/receipt/process identities bound separately. Existing residency counters and raw ftrace kind15 records describe residency lookup attempts and require their own qualifications.
