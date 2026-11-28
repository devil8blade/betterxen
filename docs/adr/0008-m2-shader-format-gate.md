# ADR 0008: M2 Shader Format Gate — Branch D1 (DXIL normalization) for M3

Status: **Proposed** (2026-11-26, issue #466). The choice for M3 is the maintainers' to accept. The evidence below is from local Apple Silicon runs by the implementer and has not been independently reproduced.

## Context

The roadmap's M2 gate asks which shader path M3 builds:
- **Branch D1, DXIL normalization:** shaders reach D3DMetal as DXIL, and BetterXen rewrites only what D3DMetal cannot run.
- **Branch D2, `ACCC`/`FSPX` translation:** console-compiled RDNA 2 code in `ACCC` chunks is translated to DXIL or MSL.

The gate needs a classifier that guest pipeline creation actually reaches, plus measurements of what D3DMetal does with each form.

## What M2 built

- **Bridge (#445):** `bx_shader_bridge.dll` is the x86-64 Windows build of `dxbc-accc`, so guest code can call the inspector.
- **Gate (#466):** `d3d12_xs.dll`'s `CreateComputePipelineState` and `CreateGraphicsPipelineState` pass every stage through `bx_shader_inspect`. One `shader_gate` JSONL record per stage gives:
  - the container kind, stage, shader model and chunk sizes;
  - the `SFI0` feature flags (`D3D_SHADER_FEATURE_*` values from DirectX-Headers `d3dcommon.h`);
  - the `PSV0` wave lane range.
- **Decisions:**
  - **DXIL and dual containers:** go to the host as the bridge's prepared copy (`bx_shader_prepare_for_d3dmetal`).
  - **ACCC-only containers:** are refused with `E_NOTIMPL` before the host device is called.
  - **Legacy DXBC and unclassifiable input:** pass unchanged.
  - **A missing bridge:** is logged once (`shader_bridge_missing`). Shaders then pass unclassified, except that a container with an `ACCC` chunk and no `DXIL` chunk is still refused with `E_NOTIMPL`, found by a bounds-checked chunk scan, because D3DMetal crashes on it. The bridge is loaded only by full path from `d3d12_xs.dll`'s own directory.
- **Known D3DMetal gaps:** a stage that needs a wave wider than 32 lanes, or 64-bit atomics, also gets a `shader_d3dmetal_gap` record.

`tests/conformance/test_shader_gate` exercises all of this.

## Evidence

| Observation | Source | Host and class |
| --- | --- | --- |
| A real DXC 1.9.2609 SM 6.6 compute shader (`fill_cs66`) creates a PSO through `d3d12_xs`, runs, and all 256 output words match the CPU reference. | `test_shader_gate` | Apple M5 Pro, macOS 27.2.1, CrossOver 26.3 (`26.3.0.39832`) bottle with `CX_GRAPHICS_BACKEND=d3dmetal`; class A, this host only |
| The same shader, run outside Wine through `d3dmetal-native` against CrossOver's D3DMetal 3.0, matches 256/256. | [d3dmetal-harness](../../shader-transpiler/crates/d3dmetal-harness/README.md) (#417) | Same host; class A |
| An ACCC-only container is refused with `E_NOTIMPL` for compute and graphics, no host object is created, and the device stays usable. | `test_shader_gate` | Same bottle; class A |
| In a bare CrossOver 26.3 prefix (vkd3d), SM 6.6 DXIL PSOs fail with `E_FAIL`. vkd3d reports `Cannot load DXIL conversion library`. | `test_shader_gate`, same host | Class A; DXIL needs the D3DMetal backend under CrossOver |
| `[WaveSize(64)]` kernels and kernels with SM 6.6 64-bit atomics are created but never run on D3DMetal. | [D3DMetal GPU limits](../research/d3dmetal-gpu-limits.md#measured-constraints) (#453) | Apple M3 Pro, Wine 11.0 source build, D3DMetal 4.0b3; class A for that host |
| DXC records `[WaveSize(64)]` as `PSV0` lanes 64/64, and 64-bit atomics on typed resources as `SFI0` `0x400000`. 64-bit atomics on raw buffers set only `INT64_OPS` (`0x8000`), the same as other 64-bit integer use. | The #453 blobs, read by the gate and by a script | Class A |
| Public sources do not establish a console-native shader container layout. BetterXen's `FSPX` is a clean-room synthetic layout, not a description of a console compiler's format. | `CLAIM-XBOX-ISA-ABI` (class U) in [claims.toml](../claims.toml); `shader-transpiler/crates/dxbc-accc/src/fspx.rs` | Source review |
| A serialized console pipeline path exists for Series X\|S, with no public version-independent format. | `CLAIM-XBOX-ACO-SAMPLE` (class D) | Source review |
| A read-only structural scan of an authorized local test workload (2026-11-28) found 52 DXIL SM 6.5 compute containers with LLVM bitcode in its `XboxBvhBuilder` component, 29 ACCC-only `FSPX` containers in its AMD FidelityFX upscaler component, 10 SM 6.6 DXIL containers with four-byte `NOBC` payloads (nine pixel, one vertex) in its main executable, and no plaintext `DXBC` in its packaged shader archive. A second scan of the same files, with local extraction and the current classifier, is in [Target title shader formats](../research/target-title-shader-formats.md). | Static container scan on this host, re-run through `dxbc-accc`; private evidence note, workload not redistributed | This host; structure only, runtime use unmeasured |

Runtime gate telemetry still covers only synthetic programs; no target-workload run has reached pipeline creation, so what a run submits remains unmeasured. One static measurement now exists (2026-11-28): a read-only container scan of an authorized local test workload, re-run through `dxbc-accc`, with the counts in the table above. The scan measures what shipped components contain, not what any run submits, and the workload is not redistributed. Wherever the gate runs, its `shader_gate` records count it.

## Decision

Branch **D1** is proposed for M3:

1. **Pass DXIL through the gate to D3DMetal.** This is the only path with measured, end-to-end execution evidence.
2. **Spend M3 shader work on the two measured gaps,** within the [D3DMetal GPU limits](../research/d3dmetal-gpu-limits.md#measured-constraints):
   - lower `WaveSize(64)` programs to 32-lane code;
   - lower SM 6.6 64-bit atomics to 32-bit operations or bounded lock emulation.

   The gate's `shader_d3dmetal_gap` records show which pipelines need this.
3. **Keep refusing ACCC-only containers** with `E_NOTIMPL` and a structured log.

Branch **D2** is deferred. It needs a documented, lawfully available description of console-compiled containers and their RDNA 2 code, and none exists (`CLAIM-XBOX-ISA-ABI`). A translator built against the synthetic `FSPX` layout would prove nothing about real inputs.

Revisit this ADR in any of these cases:
- an authorized public source for that format appears;
- gate telemetry shows a required pipeline-creation path refusing an ACCC-only container at runtime — a static scan already established that the target workload's upscaler component embeds 29, so the open question is submission, not existence;
- D3DMetal starts running the gap cases.

## Consequences

- **Pipeline creation:** `CreateComputePipelineState` and `CreateGraphicsPipelineState` now depend on `bx_shader_bridge.dll` for classification. Without it, they still work, refuse ACCC-only containers, and say so in the log.
- **Stream PSOs (#576):** `ID3D12Device2::CreatePipelineState` stream descriptions are gated too. The stream is walked as 8-byte-aligned records (a 4-byte subobject type plus a type-dependent payload). Both the payload offset and the record size come from the compiler: `alignas(void*)` aligns the record, not the payload, so 4-byte-aligned payloads sit at offset 4, and `static_assert`s pin the measured values. Shader subobjects (`VS`, `PS`, `DS`, `HS`, `GS`, `CS`, `AS`, `MS`) go through `BxShaderGate::admit`; every other payload is copied verbatim. A subobject type at or above `D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MAX_VALID` from the toolchain header (mingw-w64 `d3d12.h`: `0x1a`, immediately after `MS`), a duplicated subobject type, a payload that overruns `SizeInBytes`, or a bytecode length with a null pointer returns `E_INVALIDARG` before the host is called. The shim edits its own copy of the stream, so prepared DXIL can be substituted without writing to the caller's memory. `pso_stream` now reports `shaders_not_parsed=0` when it parsed, and each stream stage logs its own `shader_gate` record with `pso=stream`. The `--no-bridge` path also refuses ACCC-only through the stream. `test_shader_gate.exe` covers the stream form but was cross-compiled only, not run under CrossOver on the authoring host.
- **Packaging:** `scripts/build-shims.sh` builds the bridge, links it with `abi/defs/bx_shader_bridge.def`, and stages it in `build-shims/bin` beside `d3d12_xs.dll` (#484).
- **Not covered by the gate yet:**
  - **Raw-buffer 64-bit atomics:** these cannot be flagged from `SFI0`; the DXIL bitcode would have to be read.
  - **Telemetry summary:** `bx-cli trace` now summarizes the gate's records (#496): per-decision, per-kind and per-stage counts for `shader_gate`, the `shader_d3dmetal_gap` list with its wave-width and 64-bit-atomic flags, and `shader_bridge_missing`. The summary only reflects records a run actually wrote; it adds no live run and no new compatibility claim.
  - **Observed `FSPX` tables:** the clean-room section-table layout does not describe the 29 containers embedded in the target workload's upscaler component. In the 2026-11-28 scan, `dxbc-accc` rejected them with `FspxSectionOutOfBounds` before any section body was read, so the current refusal is structural, not a measured D3DMetal incompatibility. The cause the scan recorded, a descriptor offset inside the region the layout reserves for the header and section table, is reported as `FspxLayoutMismatch` since #599, apart from a section past the payload end; the scan has not been re-run against that change. Over the C ABI both are still `BX_SHADER_ERR_INVALID_BLOB`. Extending the layout against measured containers is a Track D prerequisite for any D2 trial and a maintainer provenance decision, because no public format description exists.
  - **`NOBC` programs:** since the `NOBC` fix in `[Unreleased]`, a `DXIL` chunk whose bitcode payload is the four-byte placeholder `NOBC` is classified as header-only (`Kind: Unknown`, or ACCC-only with an `ACCC` chunk) and is no longer prepared as executable DXIL; a `NOBC`-only stage takes the logged `pass_original` path. Before that fix the classifier checked only the program header's sizes and labelled such containers executable DXIL. The target workload's main executable embeds 10 such containers (nine pixel, one vertex, SM 6.6). They carry no bitcode, so D1 cannot run them as-is; their signature and validation chunks are intact.
- **Wave size:** the gate logs constraints but changes no shader. A pipeline that D3DMetal creates but does not run still behaves that way.

## Not claimed

Nothing here describes any Xbox title's shaders, behaviour on Xbox hardware, or D3DMetal versions other than those named. Evidence class: local Apple Silicon runs, one local static container scan of an authorized test workload, and public-source review. The scan counts containers embedded in shipped components on this host; it does not establish what any run submits, and it documents no console-compiled format layout beyond the stated mismatch.
