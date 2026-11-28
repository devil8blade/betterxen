# D3DMetal and Apple GPU limits for a shader translator

Constraints that a translator emitting shaders for D3DMetal on Apple Silicon has to
handle (#453). Writing the translator is out of scope.

The note has two parts:

- [Measured constraints](#measured-constraints) come from
  `tests/conformance/test_d3dmetal_gpu_limits`, an original synthetic D3D12 compute
  fixture checked against values computed on the CPU. These are the only items the
  roadmap links to. They are a local Apple Silicon run by the implementer, class A for
  the stated host only, not independently reproduced.
- [Unverified reports](#unverified-reports-not-evidence) are observations a contributor
  posted on #453 from local builds of another project running a commercial title. They are
  **not evidence** under [AGENTS.md](../../AGENTS.md) §5–6 and must not steer the plan
  until a synthetic workload reproduces them. They are kept only as leads for writing such
  workloads.

Nothing here is an Xbox hardware result, and nothing generalizes beyond the D3DMetal
version named.

## Measured constraints

### `WaveSize(64)` is created but not run

`wave.control` (no `WaveSize`) and `wave.wavesize64` (`[WaveSize(64)]`, SM 6.6) record,
for each thread, the wave width, its lane index and the dispatch index of its wave's first
lane.

The control passes the check D3D12 actually guarantees: every thread reports the same
width, and within each wave the lane indices are a permutation of `0..W-1`. No particular
thread-to-lane order is assumed. On D3DMetal 4.0b3:

- the device reports `WaveLaneCountMin = WaveLaneCountMax = 32`;
- the control runs at width 32;
- the wave64 pipeline is **created without error but never runs**: its output buffer keeps
  the sentinel.

So a translator cannot rely on pipeline creation failing. A program written for a 64-lane
wave has to be emitted as 32-lane code, with its two halves made to agree wherever the
original wave acted as one.

### Kernels using SM 6.6 64-bit atomics are created but not run

`atomic64.image` runs a 64-bit `InterlockedAdd` (low halves carrying into the high half)
and an `InterlockedMax` (decided by the high half) on a `RWTexture2D<uint64_t>` over
`DXGI_FORMAT_R32G32_UINT`. `atomic64.buffer` runs the same operations on a raw buffer. Each
kernel also writes a plain 64-bit marker next to its atomics, which shows whether the kernel
ran at all. On D3DMetal 4.0b3:

- The device reports `AtomicInt64OnTypedResourceSupported` and
  `AtomicInt64OnDescriptorHeapResourceSupported` as supported, and both pipelines are
  created without error.
- Neither kernel runs. The atomic results and the markers all read back as their initial
  zeros, on the texture and on the raw buffer alike.
- A throwaway variant with the atomics removed but the markers kept wrote 8/8 markers.
  That shows the readback path is sound and the skip comes from the atomics.

So these kernels are silently skipped, on raw buffers as well as images. This is
consistent with d3dmetal-native's `d3d12-atomic64-test` reading back zeros in the
[d3dmetal-harness](../../shader-transpiler/crates/d3dmetal-harness/README.md) evaluation
on D3DMetal 3.0. A translator has to lower 64-bit atomics to 32-bit operations.

### Lock-based emulation needs a sufficient retry cap and both fences

`lock64.cap64` and `lock64.cap1024` emulate both operations with a per-texel lock word,
with 32 contending threads per texel and a bounded number of tries:

1. take the lock with `InterlockedCompareExchange` 0→1;
2. `DeviceMemoryBarrier()` (**acquire**: the data reads are ordered after taking the lock;
   `globallycoherent` gives visibility, not ordering);
3. update the two 32-bit halves;
4. `DeviceMemoryBarrier()` (**release**);
5. free the lock with `InterlockedExchange`.

A thread that runs out of tries counts a drop instead of spinning on. The 1024-try run is
required to drop nothing and to match the CPU reference exactly. The 64-try run is
reported. On D3DMetal 4.0b3, in three runs each:

- **1024 tries:** nothing dropped, 16/16 values exact, about 15,000–18,000 tries in all.
- **64 tries:** 96–108 of 512 updates dropped.

So a low cap trades a hang for silently lost updates. With 32 contending threads, 64 tries
is already too low.

The loop shape matters. The critical section sits **inside** the retry loop, so a lane
that wins finishes it in the same iteration, and no lane waits for another lane to leave a
loop. The fixture does not run the classic shape (`while (!acquire()) {}` followed by the
critical section). There, the winning lane can be parked at the loop's reconvergence point
behind the lanes still spinning, and on a GPU without independent forward progress for
each thread that could hang the queue. Apple's shader converter prints `Warning:
Undefined: Shader might rely on forward progress/persistent thread` even for the safe
shape.

## Fixture results

A local run by the implementer on 2026-11-26. Class A for this host only; not
independently reproduced, not CrossOver 26.3, and not an Xbox hardware result.

| | |
| --- | --- |
| Host | Apple M3 Pro, macOS 27.2.1 (26B131), Rosetta 2 |
| Wine | CodeWeavers Wine 11.0 built from CodeWeavers' published source; CrossOver itself was not installed |
| D3DMetal | 4.0b3 from Apple's Game Porting Toolkit (x86_64; SHA-256 of the binary starts with `b16aab78127d`), user-supplied |
| Shaders | DXC 1.9.2609 via `shaders/compile.sh`; the same DXC reproduces the d3dmetal-harness `fill_cs66` container byte for byte (SHA-256 `7df11b63…`) |
| Command | `ROSETTA_ADVERTISE_AVX=1 WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=b" wine build-shims/bin/test_d3dmetal_gpu_limits.exe` in a fresh prefix |
| Runs | 3, identical outcomes apart from the 64-try drop counts; exit 0, `SUMMARY: 4 required checks, 0 failed` each time |

| Check | Outcome |
| --- | --- |
| `device.shader_model` | 6.6 |
| `wave.reported` | `WaveOps=1`, lane count min 32, max 32 |
| `wave.control` | ran at width 32, lanes a permutation within each wave (required: pass) |
| `wave.wavesize64` | created but not run (output still sentinel) |
| `atomic64.reported` | typed and descriptor-heap 64-bit atomics reported as supported |
| `atomic64.buffer` | not run: marker 0/8, atomic values 16/16 still zero |
| `atomic64.image` | not run: marker 0/8, atomic values 16/16 still zero |
| `lock64.cap64` | 108, 96 and 108 of 512 updates dropped (values not compared) |
| `lock64.cap1024` | 0 of 512 dropped, 16/16 values exact (required: pass) |

**Exit codes.** The fixture exits **0** with every required check passed, **1** on a failed
required check, and **77** with a `SKIP: <reason>` line when `d3d12.dll`, its exports, device
creation, or shader model 6.6 is unavailable. 77 is not a pass; see the exit-status section of
[`tests/conformance/README.md`](../../tests/conformance/README.md#exit-status-pass-fail-and-unavailable)
for a recording recipe. Through `bx-cli run` the guest status is returned unchanged.

Still to do for the issue's validation requirement: an independent run on CrossOver 26.3
(D3DMetal 3.0) by another contributor.

## Unverified reports (not evidence)

Reported on #453 from local VIctory-macOS-based builds running a commercial title on one
host. None of this is a BetterXen measurement, no code or shader from that project is
used, and none of it may be cited by the roadmap or a milestone gate. Each item names the
synthetic workload that would turn it into evidence.

- **Splitting wave64 programs.** Operations that need care when the two halves must agree:
  - wave reductions;
  - first-lane and indexed lane reads;
  - the 64-bit lane mask read as data;
  - cross-half permutes;
  - scalar values spilled into vector lanes;
  - group barriers, which are only safe where both halves provably reach them.

  The reporter found a uniformity analysis of the program's values, with barriers placed
  only after uniform branches, made most such programs translate exactly. Where agreement
  cannot be proven and each half runs as its own wave, a group sync that not every lane
  reaches must become a memory fence without the group wait. *Workload needed:* SM 6.6
  wave64-style kernels with each of these patterns, checked half by half.
- **Helper lanes in a spin lock.** In a pixel shader, helper lanes must not take part in
  such a lock: their atomics have no effect, so they never win and spin to the cap. This
  was reported as a hard queue hang with no macOS GPU timeout logged. A cap of 2^16 was
  reported to hang while 2^10 did not; whether the cap or the helper lanes caused it was not
  separated. *Workload needed:* a pixel shader with a lock-protected 64-bit update across
  triangle edges.
- **Mesh output size.** Metal refuses mesh pipelines whose per-vertex outputs exceed 32,768
  bytes, counting the aligned layout (distances, position, then outputs in register order,
  each aligned to its own size). A refused pipeline submitted anyway was reported to hang
  the queue under Wine. *Workload needed:* mesh shaders whose output size crosses 32 KiB
  only once alignment is counted.
- **Apple shader compiler crash.** `MTLCompilerService` was reported to crash in register
  allocation on mesh pipelines whose pixel shader writes several render targets. The
  reported cause was output stores merged before all their values are defined, and the
  reported fix was emitting each output row's stores after its values. *Workload needed:* a
  mesh and pixel pair with several render targets and late-computed outputs.
- **No zero-copy heaps.** D3DMetal 4.0b3 was reported to answer
  `ID3D12Device3::OpenExistingHeapFromAddress` with "Not implemented". *Workload needed:*
  a fixture that calls it on a page-aligned allocation.

## Regenerating the shaders

`tests/conformance/test_d3dmetal_gpu_limits/shaders/compile.sh` regenerates the committed
headers with Microsoft's DXC release v1.9.2609 (`dxc_2026_09_29.zip`, SHA-256
`ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1`, the same package the
d3dmetal-harness records), running `bin/x64/dxc.exe` under a Wine loader.
