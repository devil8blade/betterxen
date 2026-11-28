# Shader formats in a target title's executable code (2026-11-28)

This note records which shader container formats a project-authorized target build carries in its
unencrypted executable code, and what that means for running it on D3DMetal. It is supporting
research for the M2 shader-format gate ([ADR 0008](../adr/0008-m2-shader-format-gate.md)); it
changes no exit gate.

Per [AGENTS.md](../../AGENTS.md) §6 the title and build are not named. No shader bytes, offsets,
hashes or file names from the build are recorded here, only counts and conclusions.

## Method

- **Scope:** every PE32+ file in the title's package (the executable and its DLLs). The title's
  data archives are encrypted and were **not** read: decrypting them would be circumvention, which
  §6 and [docs/legal.md](../legal.md) rule out.
- **Extraction:** each embedded container was located by its `DXBC` magic and cut at the size in
  its header, into a local temporary directory that was deleted afterwards.
- **Classification:** chunk tags were read directly from each container's chunk table, the `DXIL`
  program header was read for stage and shader model, and the first bitcode bytes were checked
  (LLVM `BC` magic or the `NOBC` header-only placeholder). Every container was also passed to
  `bx-cli shader` (`dxbc-accc` as on `main` on 2026-11-28).
- **Host:** Apple Silicon, macOS 27.2, run on 2026-11-28. Evidence class **A** (`docs/claims.toml`)
  for the counts below. The conclusion about the title's main shaders is an inference.

## Results

91 containers in three files.

| Component | Containers | Chunks | Shader content | Runs on D3DMetal today |
|---|---|---|---|---|
| Raytracing acceleration-structure builder DLL | 52 | `DXIL HASH ILDN ISG1 OSG1 PSV0 RTS0 SFI0 STAT` | SM 6.5 compute, real DXIL bitcode | **Yes**: `bx-cli shader` reports `Kind: Dxil` |
| AMD FidelityFX upscaler DLL (console build) | 29 | `ACCC HASH ISG1 OSG1 PSV0 RTS0 SFI0` | Console code only, no `DXIL` chunk | **No**: needs RDNA 2 translation. `bx-cli shader` rejects all 29 on the FSPX section table (#599) |
| Title executable | 10 | `DXIL HASH ISG1 OSG1 PRIV PSV0 SFI0` | SM 6.6 pixel (9) and vertex (1) shaders whose `DXIL` is the `NOBC` header-only placeholder; no `ACCC`; a 12-byte `PRIV` chunk | **No code in the container**: `bx-cli shader` reports `Kind: Unknown` |

## What this means

- **The acceleration-structure builder is ready.** Its shaders are ordinary DXIL that the M2 gate
  passes to D3DMetal (`pass_dxil`). Whether D3DMetal supports the raytracing work they feed is a
  separate question.
- **The upscaler is console-only.** If the title cannot run without it, it blocks a first frame
  until an RDNA 2 translator exists (M3, Branch D2 of ADR 0008).
- **The executable's shaders carry no code.** Their bitcode is deliberately stripped (`NOBC`), there
  is no console chunk, and each has a 12-byte `PRIV` chunk the size of an identifier. The likely
  reading, which is an **inference** and has not been measured, is that the real code ships
  separately, keyed by that identifier, as precompiled console binaries inside the encrypted
  archives. If so, the title's main shader set is console-only too, and a first frame depends on
  the RDNA 2 translator rather than on the existing DXIL path.

## How to confirm

The title's main shaders can only be seen at runtime. The first time it creates a pipeline, the
M2 gate logs a `shader_gate` record per stage with the container's classified kind (DXIL,
dual, ACCC-only or unknown). Once bring-up gets the title past device setup (#644, #645), `bx-cli trace` on that run
answers the question directly.
