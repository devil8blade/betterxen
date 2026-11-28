# Console shader route: emit step and upscaler substitution (2026-11-28)

Two checks behind the plan for running a title's console shaders on D3DMetal. They follow
[Target Title Shader Formats](target-title-shader-formats.md), which found that a target title's
shaders are mostly console-only. This note changes no gate and adds no dependency. Anything it
proposes to reuse still goes through [AGENTS.md](../../AGENTS.md) §4 (per-file licence, upstream
registry, reuse matrix) before integration.

The plan it supports: translate RDNA 2 console code into HLSL, compile the HLSL with Microsoft's
DirectX Shader Compiler (DXC) into DXIL, and hand the DXIL to D3DMetal through the existing M2
gate ([ADR 0008](../adr/0008-m2-shader-format-gate.md), Branch D2). Where an open-source
replacement for a whole component exists, substitute it instead of translating its shaders.

## 1. The emit step: DXC inside the guest

| Question | Result | Evidence |
|---|---|---|
| Is there a macOS DXC? | **No.** The DXC releases ship Windows and Linux packages only, and there is no Homebrew formula. | GitHub releases `v1.9.2609`, `v1.10.2605.37`, `v1.9.2607` |
| Does Windows DXC run where the shims run? | **Yes.** The shims are Windows DLLs inside CrossOver, and `dxc.exe` from DXC `v1.9.2609` already runs under the CrossOver Wine loader (see `test_d3dmetal_gpu_limits/shaders/compile.sh`). A translator in `d3d12_xs` can load `dxcompiler.dll` in-process. | Repository script; run on 2026-11-28 |
| Is `dxil.dll` needed to sign DXIL? | **No.** `fill_cs66.hlsl` compiled with `-T cs_6_6` from a directory holding `dxc.exe`, `dxcompiler.dll` **and** `dxil.dll`, and from one holding only `dxc.exe` and `dxcompiler.dll`, gives **byte-identical** 3092-byte containers with the container hash set. `dxcompiler.dll` validates and signs on its own. | Run on Apple Silicon, CrossOver 26.3, 2026-11-28 (evidence class A) |
| Does D3DMetal accept that DXIL? | **Yes.** The repository's committed shaders come from the same DXC release, and the shader-gate fixtures pass in a D3DMetal bottle. | `test_shader_gate` live runs |

**Licensing.** The `v1.9.2609` package carries three licence files: `LICENCE-MIT.txt`,
`LICENSE-LLVM.txt` and `LICENSE-MS.txt`. `LICENSE-MS.txt` allows redistributing listed
distributables, but only if distributors and end users agree to terms that protect Microsoft and
the distributor indemnifies Microsoft. It also restricts combining with "Excluded Licenses". It is
not clear which binaries it covers, so per §4 BetterXen should **not bundle the prebuilt DXC
binaries**. Two clean options:

- **User-supplied**, as D3DMetal is: the user downloads DXC and BetterXen loads it from a
  configured path.
- **Built from source:** the DXC repository's source is under LLVM/NCSA and MIT.

## 2. Substituting the FidelityFX upscaler

The upscaler's shaders are console-only, so substituting the component is cheaper than translating
it.

- **API surface.** The title imports `ffxCreateContext`, `ffxDestroyContext`, `ffxDispatch` and
  `ffxQuery` from the FidelityFX **loader** DLL, which loads the console upscaler as its provider.
  Both console DLLs export the FFX API set: those four plus `ffxConfigure`. This is the same
  structure as AMD's PC FidelityFX SDK, where a DX12 loader loads a DX12 provider. **Substitution
  point:** a BetterXen loader that implements the five FFX API entry points and forwards to a PC
  build compiled to DXIL.
- **Licensing of PC FidelityFX.**
  - The current FidelityFX SDK (2.x) is under an AMD licence that allows redistribution but forbids
    reverse engineering, decompilation and disassembly, and ships the upscalers as signed prebuilt
    DLLs. It is not a clean fit for a GPL-3.0-or-later project.
  - **FidelityFX SDK `v1.1.4` is MIT** and contains the full **FSR 3.1** upscaler source (29 shader
    and header files) and the FFX API loader. That is the candidate source.
- **Version gap.** The console upscaler implements **FSR 4**, AMD's ML upscaler (FP8 and INT8
  model passes), and also exposes the FSR 3 effect. FSR 4 is not available as source, so a
  substitute can offer FSR 3.1 only. This relies on the title accepting FSR 3 through the FFX API's
  version query and selection, which is **not yet verified**.
- **Console descriptors.** No Xbox FidelityFX headers are in the authorized GDK material. The
  console backend descriptor types the title passes have to come from call-site observation when it
  first creates an upscaler context.

## Next steps

1. When bring-up reaches pipeline creation, record the title's `shader_gate` kinds to confirm the
   main shader set (see the companion note).
2. Before integrating FidelityFX `v1.1.4`, record it in the upstream registry and reuse matrix
   (§4), and observe the title's `ffxCreateContext` and `ffxQuery` calls to pin the console
   descriptors and the version it asks for.
