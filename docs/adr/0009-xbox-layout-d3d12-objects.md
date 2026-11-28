# ADR 0009: Xbox-layout D3D12 objects in d3d12_xs

Status: **Proposed** (2026-11-28). Phase 1 is implemented with this ADR; the maintainers decide whether the direction stands. The evidence is from local Apple Silicon runs by the implementer and has not been independently reproduced.

## Context

`d3d12_xs.dll` handed titles PC D3D12 objects: C++ proxies (`proxy.h`, `proxy_generated.cpp`) whose vtables follow the PC `<d3d12.h>` order. A title built against the Xbox Series D3D12 header (`d3d12_xs.h`, part of the GDK material Microsoft authorized the project to use) calls through each object's **function table**, and that order is different:

| Interface | Xbox slots | PC slots | Orders agree up to |
| --- | --- | --- | --- |
| `ID3D12Device` | 83 | 44 | slot 43. Xbox then adds 39 methods (`CreatePlacedResourceX`, `CreateCommandQueueX`, ...) before `ID3D12Device1`. |
| `ID3D12Device11` (chain) | 139 | — | `ID3D12Device11` is the interface a project-authorized target build asks for. |
| `ID3D12GraphicsCommandList` | 107 | 60 | slot 27 |
| `ID3D12CommandQueue` | 38 | 19 | slot 13 |

So every call past those slots reached the wrong PC method. On the target build, the first call on the device after `QueryInterface(ID3D12Device11)` jumped to address 0.

From the same header:
- **Base layout:** every object starts with the table pointer, a 32-bit flags word and `m_RefCount`, 16 bytes in all.
- **`ID3D12Device` fields:** a 64-bit null constant-buffer address at +16 and a 64-byte union at +24. The union's first byte, `m_OutOfLineFlags`, makes the two inline device helpers (`CopyDescriptorsSimple`, `CreateConstantBufferView`) call out through the table when it is non-zero. That holds unless a title is compiled with `D3DCOMPILE_NO_DEBUG`, in which case they write descriptors inline.
- **Command lists:** they add a put buffer and CPU state. Of their 76 inline methods, 71 call the table. The other five write command packets directly when the next write fits below `m_Putter`'s limits, and call the table otherwise.
- **IIDs:** the interface IDs of shared interfaces are the PC ones (`ID3D12Device` `189819f1-…`, `ID3D12Resource` `696442be-…`).

## Decision

1. **Xbox-layout objects:** titles get `BxXboxObject`s (`xbox_facade.h`). They have the Xbox object layout, with `m_OutOfLineFlags` set, and an Xbox-order function table. Each object hosts a PC object, normally one of the existing proxies, so their logging, shader gate, heap hooks and wrapping stay in force.
2. **Generated tables:** `tools/gen_xbox_tables.py` reads the Xbox header locally and the toolchain's `<d3d12.h>`, and writes `xbox_tables_generated.cpp`. It takes only slot order, names, IIDs and parameter types from the Xbox header. The header is never committed.
   - **Forwarders:** a slot whose Xbox and PC signatures are equivalent gets a typed forwarder written with the PC types. That includes the free-function versus member struct-return difference, so the compiler produces the right calling convention. Interface pointers passed in are unwrapped, and `(REFIID, void**)` results are wrapped.
   - **Stubs:** every other slot is a `BX_VTABLE_STUB_LOG` stub.
   - **Hand-written slots:** Xbox-only methods are implemented in `xbox_facade.cpp` as titles need them.
3. **Who gets which layout:** titles set `Version` in the `D3D12XBOX_CREATE_DEVICE_PARAMETERS` block they pass to `D3D12XboxCreateDevice`, and the documentation requires it. Those calls get the Xbox layout. BetterXen's synthetic fixtures are PC-layout clients built against `<d3d12.h>`, pass a zeroed block, and keep the PC proxy. The log records which layout was chosen (`xbox_layout`).
4. **Command lists (Phase 2):** command lists get an Xbox object whose zeroed put-buffer pointers fail every inline room check, so every call reaches the table.

## Consequences

- **Phase 1 (this change):**
  - The device chain up to `ID3D12Device11` is covered: 139 slots, 75 forwarded, 4 shared or hand-written, 60 stubs.
  - So are the objects it creates: resources, heaps, fences, descriptor heaps, root signatures, pipeline states, command allocators, query heaps, command signatures and command queues.
  - `CreateCommandQueueX` maps the Xbox queue description onto PC `CreateCommandQueue`. The engine and queue indices are logged and not used.
  - On the target build, the title now creates its device and four compute queues, and stops at `CreateCommandListX`, which is Phase 2.
- **Phase 2:**
  - **Command lists:** `ID3D12GraphicsCommandList8`, 150 slots: 75 forwarded, 6 shared or hand-written, 69 stubs. They are created through `CreateCommandListX` and the PC `CreateCommandList`, and their Xbox field area is zeroed, so the five inline fast paths call through the table.
  - **Interface pointers inside structures:** `ResourceBarrier` and `CopyTextureRegion` unwrap them before the host call. The Xbox `ResourceBarrier` returns 0 actions.
  - **Frame pacing:** `SetFrameIntervalX`, `ScheduleFrameEventX` and `WaitFrameEventX` (`frame_pacing.cpp`) are emulated from the recorded interval by a host timer. It signals the origin, vblank and flip events and their ancillary handles, and gives out frame-pipeline tokens as documented.
  - **Generator fixes:** CRLF lines in parts of the header, a table definition whose end the boundary pattern ran past (it hid `ID3D12GraphicsCommandList1`), SAL annotations with nested parentheses, and `FLOAT`/`float`.
  - **Resources at title-chosen addresses** (`placed_x.cpp`), translation by copy:
    - `CreatePlacedResourceX` and `CreatePlacedResourceX1` create a host shadow resource. A buffer gets a committed buffer in a CPU-visible write-back heap, filled from the title's memory at creation; a texture gets a committed texture whose contents are not filled, which is logged.
    - The resource reports the title's address from `GetGPUVirtualAddress`.
    - Not yet done: keeping shadows in step with later CPU writes (planned: page-protection dirty tracking, copying at submit), copying GPU writes back, and translating title addresses in GPU calls.
  - **On the target build:** the title creates 581 command lists, sets a 30 Hz interval and schedules its origin and flip events. It then lays out its own descriptor memory with `xmem` placeholders (#643) and creates a 174 MB buffer at a title-chosen address through `CreatePlacedResourceX1`, now served by a shadow.
    - **Where it stops:** the title creates a descriptor heap and requires `GetCPUDescriptorHandleForHeapStart` to equal an address in its own descriptor memory. D3DMetal returns an opaque handle.
    - **What that needs:** BetterXen-owned descriptor heaps whose handles are addresses, translated to host descriptors when used. That is the descriptor-heap item under Consequences.
- **Fixtures:** the existing fixtures are unaffected, since they use the PC layout. `tests/conformance/test_d3d12_xbox_layout` calls Xbox slots by number, with the expected results taken from a PC-layout device. It fails against the previous `d3d12_xs.dll`.
- **Descriptor heaps:** a title compiled with `D3DCOMPILE_NO_DEBUG` writes console-format descriptors straight into descriptor-heap memory and computes handles with Xbox descriptor sizes. Supporting that needs BetterXen-owned descriptor heaps translated at bind time, which is not part of this ADR.
- **Identity:** QueryInterface on an Xbox object for another wrapped family returns that family's Xbox object over the same host. A host reached through another path, such as `GetDevice` returning the raw host device, may get a second Xbox object.
- **Replacing the PC proxy:** this ADR supersedes the PC proxy for titles. Whether the PC proxy is eventually removed in favour of fixtures that use the Xbox layout is left open.
