# Console compute shader entry ABI (probe results)

Recorded 2026-11-28 (CLAIM-XBOX-CS-ENTRY-ABI). This page lists facts derived from Microsoft's Series X|S HLSL compiler, used under an authorization Microsoft gave the project for its GDK material. The compiler, its output and the GDK files are not in this repository. No listing, binary or documentation text is reproduced. Only the observations below are. It complements [RDNA 2 notes](rdna2.md), which cover AMD's public ISA, and [Shader containers](shader-containers.md).

What this is:
- **Measured:** what the compiler emitted for ten compute shaders the project wrote, compiled on one Apple Silicon host under CrossOver 26.3.
- **Inferred:** what each register holds. The interpretations below are read from the generated code. They agree with the user-SGPR, thread-group-ID and thread-ID fields of AMD's publicly documented `COMPUTE_PGM_RSRC2` register, which the compiler reports for each shader.

What this is not: a console run, a description of the ACCC/FSPX container, or anything about graphics stages. The compiler used here writes plain DXIL objects. The console code appears only in its assembly listing, so how that code is stored in an ACCC container is still unknown (CLAIM-XBOX-ISA-ABI).

## Method

Each probe is a `cs_6_5` compute shader (`cs_6_6` for the 64-bit atomic) with an explicit `[RootSignature]`. The compiler needs a root signature to generate console code. It reported "GFX10" for the target. The probes change one thing at a time:

| Probe | Root signature | Shader |
| --- | --- | --- |
| p00 | `SRV(t0), SRV(t1), UAV(u0)` | `o[i] = a[i] + b[i]`, 64 threads |
| p01 | `RootConstants(num32BitConstants=4, b0), UAV(u0)` | uses all four constants |
| p02 | `CBV(b0), UAV(u0)` | reads two constants |
| p03 | `DescriptorTable(SRV(t0, numDescriptors=2)), DescriptorTable(UAV(u0))` | p00 through tables |
| p04 | `DescriptorTable(SRV(t0)), DescriptorTable(Sampler(s0)), UAV(u0)` | `Texture2D.SampleLevel` |
| p05 | `UAV(u0), SRV(t0), SRV(t1)` | p00 with the parameters reordered |
| p06 | `UAV(u0)` | `[numthreads(8,8,1)]`, writes group and thread IDs |
| p07 | `SRV(t0), UAV(u0)` | `WaveActiveSum` |
| p08 | `SRV(t0), UAV(u0)` | 256 bytes of `groupshared` and a barrier |
| p09 | `UAV(u0)` | 64-bit `InterlockedAdd` |
| p10 | `RootConstants(num32BitConstants=20, b0), UAV(u0)` | sums all 20 constants |

## Observations

**Wave size.** Every probe was compiled for 64-lane waves: the compiler reported wave32 as disabled. In p07 `WaveActiveSum` became a full 64-lane reduction that combines the two 32-lane halves and uses a 64-bit execution mask. D3DMetal runs 32-lane waves only ([D3DMetal GPU limits](d3dmetal-gpu-limits.md)). So console-compiled compute code has to be treated as wave64 in general, not only when a shader declares `WaveSize(64)`.

**Root arguments in registers.** Root-signature data starts at `s4`, in root-parameter order. p05 moves the registers with the parameters.
- A root constant takes one SGPR (p01: `s4`–`s7`).
- A root CBV, SRV or UAV takes two SGPRs, a 64-bit GPU address (p00: `s4:s5`, `s6:s7`, `s8:s9`).
- A descriptor table takes one SGPR, a 32-bit pointer. The shader clears the upper half itself (p03, p04).
- The thread-group IDs follow the root data, x before y (p00: `s10`; p06: `s6`, `s7`). Thread IDs arrive in `v0` (x) and `v1` (y), as the reported thread-ID count indicates.

**Overflow.** `s0`–`s3` come before the root data. When the root data does not fit in user SGPRs (p10: 20 constants and a UAV), only those four are passed, and `s0:s1` holds the address of the root data in memory. The shader loads the constants from offset 0 and the UAV address from offset `0x50`, which is root dword 20. Whether `s0:s1` points at the same data when the arguments also fit in registers was not tested.

**Root buffer access.** For a root SRV, UAV or CBV, the shader builds the buffer descriptor itself from the root address:
- **Stride:** in word 1, bits 16 and up, taken from the element type: 4 bytes for `uint`, 8 for `uint64_t`, 16 for the root CBV.
- **Record count:** `-1`, so accesses through root descriptors are not bounds-checked.
- **Word 3:** the constant `0x11014fac` in every probe. That value is from the PIX shader compiler's listing (its NewBE code generator). The ACO compiler path (`xdxc`), which emits the ACCC containers titles ship, generates different code for the same HLSL, and its word 3 is `0x31016fac` ([#647](https://github.com/MacosPorters/betterxen/pull/647)). These observations describe the NewBE path; register placement (the root UAV in `s[4:5]`) agreed in #647's ACO outputs.

**Descriptor tables.** Descriptors are loaded from the table address:
- buffer descriptors 32 bytes apart (p03 loads at offsets `0` and `0x20`);
- a texture descriptor of 32 bytes and a sampler descriptor of 16 bytes (p04).

For each such load, the compiler also reported the instruction offset and the HLSL binding it came from: class, register, space and resource kind.

**Other.** p08 uses LDS (`ds_write_b32`, `ds_read_b32`), and its size shows in the reported LDS allocation. p09 is a native `buffer_atomic_add_x2`. The shader hash from the container's `HASH` part is embedded at the start of the generated code.

## Consequences

- **Branch D2** ([ADR 0008](../adr/0008-m2-shader-format-gate.md)) gains a concrete compute entry ABI to translate against: where root constants, root descriptors, table pointers and IDs are, and how buffer descriptors are formed.
- **Binding recovery** may not need data-flow analysis if ACCC containers carry the same per-load binding table. That depends on the container format, which is still unknown.
- **Wave64** is the default. The D1 path also needs to know whether a title's DXIL assumes 64 lanes.
- **Bounds:** out-of-range accesses through root descriptors are not discarded on the console the way bounds-checked descriptors would discard them.

## Open

- **Graphics stages** (vertex and pixel inputs, exports) were not probed.
- **ACCC output:** which compiler or option emits ACCC-only containers like those in the target workload's upscaler component (see the [roadmap](../roadmap.md) scan entry). The compiler used here does not.
- **`s0`–`s3`:** what they hold when root data fits in registers.
- **One compiler build, one host:** a different GDK version may differ.
