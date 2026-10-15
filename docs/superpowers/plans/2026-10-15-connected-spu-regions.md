# Connected SPU Regions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Safe opt-in connected SPU regions and verified native progress toward30 displayedFPS.

**Architecture:** Preserve logical translation and original wrappers, emit grouped multi-entry bodies, and retain exact validation and transfer semantics. Sensitive paths deopt through existing dispatcher. Keep control and candidate in one binary.

**Tech Stack:** Python lifter, C17 runtime/generated code, ARM64 macOS/Metal native validation.

**Spec:** ../specs/2026-10-15-connected-spu-regions.md

## Global Constraints

- MAGBCUS98110v1.0; immutable source/input hashes; no original bytes/generated code/captures in Git.
- Muted SDL dummy output; retain guest audio/SPU processing.
- Preserve arithmetic, code identity/mutation, guest return/IRQ and synchronization semantics.
- No RPCS3/services control without fresh owner handoff.
- Source checkpoints local; existing progress gist separately authorized.

## Review Focus

- Call-return targets/mid-body entry cannot execute caller continuation twice.
- Internal gotos cannot bypass pending IRQ, stack reset, scheduling or code mutation checks.
- Data/gaps/unknown opcodes remain fail-closed and cannot become no-op entries.
- Entry registry identity/capacity and image precedence remain exact.
- Display acceptance needs unique guest-frame correspondence and valid timestamps.

### Task1: Region emission and runtime checkpoints

Files: tools/spu_lifter.py; new tools/spu_regions.py; runtime/spu/spu_context.h; runtime/spu/spu_drain.c; neutral tests/runner.

- [ ] Add failing neutral emission and execution differential tests for direct loops, fall-through, middle entry, calls, IRQ-sensitive fallback and mutation.
- [ ] Implement opt-in grouping and emitter preserving original logical classifier/translation.
- [ ] Implement local checkpoint eligibility with original sensitive transfers returned to dispatcher.
- [ ] Verify optimized/UBSan and existing guard/return/IRQ controls; mutation negative controls.
- [ ] Independent review and focused source commit.

### Task2: Private preparation and native trial

Files: MAG private preparation helper and synthetic checks, builds/framework.json and research receipts.

- [ ] Prepare original geometry/audio exact mappings with both legacy and region variants; independently validate entry census/guards/provenance.
- [ ] Build exact same-binary A/B; verify owner before muted launches.
- [ ] Measure strongest candidate, save immutable results and failure limitations; reverse order if warranted.
- [ ] Preserve output correctness evidence and source-only checkpoints/gist.

### Task3: Continue to actual frame goal

- [ ] Add unique frame sequence and controlled wall-window telemetry if current counters cannot establish acceptance.
- [ ] Investigate synchronous report/FIFO waits preserving GPU completion ordering.
- [ ] Use measured critical-path evidence to choose next optimization; keep goal active until verified >=30FPS.
