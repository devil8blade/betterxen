# BetterXen Documentation

## Getting Started & Architecture

- [Project README](../README.md) — Overview, quick start, `bx-cli` commands, and verified scope.
- [Roadmap](roadmap.md) — Single authoritative plan (Milestones M0–M6) and completion gates.
- [Parallel Track Architecture](architecture/parallel-tracks.md) — 6-track directory ownership, ABI freeze rule, and multi-agent Git workflow.
- [Wave 1 Parallel Backlog](issues/wave-1-backlog.md) — Non-overlapping starter issues across Tracks A–F.
- [Frozen ABI Contracts](../abi/README.md) — Cross-DLL C headers (`abi/include/`) and canonical `.def` export tables (`abi/defs/`).
- [Synthetic Conformance Suite](../tests/conformance/README.md) — Auto-discovered PE32+ conformance binaries.
- [Changelog](../CHANGELOG.md) — Unreleased and historical changes.

## Architecture Decision Records

- [ADR 0005: CrossOver-based hybrid direction](adr/0005-crossover-hybrid-direction.md) — Pivot to CrossOver + Rosetta 2 + D3DMetal as the external foundation.
- [ADR 0006: Retire in-tree native-first workspace](adr/0006-retire-in-tree-native-workspace.md) — Retirement of paused native code/scripts/docs from `main` (preserved at Git tag `native-first-final`).
- [ADR 0007: API-set win32_bridge shims under CrossOver](adr/0007-api-set-shims-under-crossover.md) — Accepted: CrossOver answers the six API-set import names itself, so the shims are not loaded for static imports.
- [ADR 0008: M2 Shader Format Gate](adr/0008-m2-shader-format-gate.md) — Proposed: Branch D1 (DXIL through D3DMetal, with the measured wave64 and 64-bit-atomic gaps as M3 work); ACCC-only containers refused with `E_NOTIMPL` until a lawful format source exists.

## Research, Testing & Licensing

- [Hybrid Workloads & Measurement Protocol](testing/hybrid-workloads.md) — Original Windows workloads, receipt fields, timing conventions, and negative controls.
- [CrossOver Licensing & Redistribution Boundaries](research/crossover-licensing.md) — Per-component license review for CrossOver, Wine, DXMT, DXVK, VKD3D-Proton, MoltenVK, D3DMetal, and Rosetta 2.
- [Upstream Research Registry](research/upstream-registry.md) & [Component Reuse Matrix](research/component-reuse-matrix.md) — Public upstream evaluation and provenance screening.
- [Primary-Source Verification (2026-11-25)](research/primary-source-verification-2026-11-25.md) & [Hybrid Backlog Research Record](research/hybrid-backlog-research-2026-11-25.md) — Vendor documentation verification notes.
- [D3D12 Swapchain References under D3DMetal](research/d3dmetal-swapchain-references.md) — Swapchain lifetime results across two Apple Silicon hosts and CrossOver builds.
- [D3DMetal GPU Limits](research/d3dmetal-gpu-limits.md) — Constraints a shader translator targeting D3DMetal must handle, measured by the `test_d3dmetal_gpu_limits` fixture (`WaveSize(64)` and SM 6.6 64-bit atomics not run, bounded lock emulation), plus unverified reports kept separate as leads (#453).
- [Public Technical References](research/sources.md), [Xbox GDK Notes](research/xbox-gdk.md), [RDNA 2 Notes](research/rdna2.md), and [Shader Containers](research/shader-containers.md) — Public specification references for shim and shader work.

## Governance, Security & Legal

- [Engineering & Agent Rules](../AGENTS.md) (`CLAUDE.md` symlink) — Mandatory rules for human contributors and coding agents.
- [Contributing Guidelines](../CONTRIBUTING.md) — Claiming, verification, clean-room policy, and DCO sign-off.
- [Claims & Evidence Classes](claims.toml) — Scoped claims with an evidence-class legend (A, B, D, E, U); classes describe evidence type, not Xbox hardware verification.
- [Legal Scope](legal.md), [Dependency Licensing](licenses.md), and [Third-Party Notices](../THIRD_PARTY_NOTICES.md) — Input policy, attribution, and redistribution boundaries.
- [Threat Model](threat-model.md) & [Security Policy](../SECURITY.md) — Isolation boundaries and vulnerability reporting.
