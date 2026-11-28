#!/usr/bin/env python3
"""Generate the pass-through half of the d3d12_xs COM proxies.

Reads the C++ interface declarations in the mingw-w64 toolchain's <d3d12.h>
(the same header the shim is compiled against) and writes
proxy_generated.h / proxy_generated.cpp next to this script's parent directory.

Only method names, return types and parameter types are taken from the header:
they are what an override must repeat verbatim to compile. Parameters are
renamed positionally (a0, a1, ...). Methods listed in CUSTOM are declared but
left for proxy.cpp to implement by hand (wrapping, unwrapping, logging).
Methods listed in HANDOFF keep the generated trace and null-host stub, then pass
the host interface and the caller's arguments, unmodified, to a free function
whose declaration is generated from the same header signature.

Usage: python3 shims/d3d12_xs/tools/gen_proxy.py [path/to/d3d12.h]
       (default: located through `x86_64-w64-mingw32-g++ -E`)
"""
import re
import subprocess
import sys
from pathlib import Path

OUT_DIR = Path(__file__).resolve().parents[1]
PAD_SLOTS = 32

# proxy class -> (most derived interface, methods implemented by hand in proxy.cpp)
PROXIES = {
    "BxDeviceProxyGen": ("ID3D12Device10", {
        "QueryInterface", "AddRef", "Release",
        "CreateCommandQueue", "CreateCommandAllocator", "CreateCommandList",
        "CreateCommandList1", "CreateCommandQueue1",
        "CreateGraphicsPipelineState", "CreateComputePipelineState", "CreatePipelineState",
        "MakeResident", "Evict", "EnqueueMakeResident", "SetResidencyPriority",
    }),
    "BxCommandQueueProxyGen": ("ID3D12CommandQueue", {
        "QueryInterface", "AddRef", "Release", "GetDevice", "ExecuteCommandLists",
    }),
    "BxCommandAllocatorProxyGen": ("ID3D12CommandAllocator", {
        "QueryInterface", "AddRef", "Release", "GetDevice",
    }),
    "BxCommandListProxyGen": ("ID3D12GraphicsCommandList7", {
        "QueryInterface", "AddRef", "Release", "GetDevice",
    }),
}

# proxy class -> {method: free function}. The function receives the host interface of the method's
# level followed by the caller's arguments; nothing is unwrapped. vmem.cpp implements both below
# with the same placement validation and heap retention as CreatePlacedResource (slot 29).
HANDOFF = {
    "BxDeviceProxyGen": {
        "CreatePlacedResource1": "bx_vmem_create_placed_resource1",
        "CreatePlacedResource2": "bx_vmem_create_placed_resource2",
    },
}

# Interfaces that carry no host pointer of their own: their methods forward through the root level.
ABSTRACT = {"IUnknown", "ID3D12Object", "ID3D12DeviceChild", "ID3D12Pageable", "ID3D12CommandList"}
# A single-pointer parameter of one of these types may be one of our proxies and is unwrapped.
# ID3D12Heap covers CreatePlacedResource, CreatePlacedResource1 and CreatePlacedResource2.
# A wrapped heap passed straight through faults in the host (0xc0000005 on D3DMetal).
MAYBE_PROXY = {"ID3D12CommandQueue", "ID3D12CommandAllocator", "ID3D12CommandList",
               "ID3D12GraphicsCommandList", "ID3D12Pageable", "ID3D12DeviceChild", "ID3D12Object",
               "ID3D12Heap", "ID3D12Heap1"}

IUNKNOWN = [
    ("HRESULT", "QueryInterface", [("REFIID", ""), ("void **", "")]),
    ("ULONG", "AddRef", []),
    ("ULONG", "Release", []),
]


def find_header():
    out = subprocess.run(["x86_64-w64-mingw32-g++", "-E", "-x", "c++", "-"], input="#include <d3d12.h>\n",
                         capture_output=True, text=True, check=True).stdout
    m = re.search(r'"([^"]*[/\\]d3d12\.h)"', out)
    if not m:
        sys.exit("could not locate d3d12.h through the cross compiler")
    return m.group(1)


def parse_interfaces(text):
    """Return {name: (base, [(ret, method, [(type, array_suffix)])])} for the C++ declarations."""
    interfaces = {}
    for m in re.finditer(r'MIDL_INTERFACE\("[0-9a-fA-F-]+"\)\s*\n(\w+) : public (\w+)\s*\n\{(.*?)\n\};', text, re.S):
        name, base, body = m.group(1), m.group(2), m.group(3)
        # Keep the explicit-aggregate-return form (hidden pointer spelled out): it is the real x64 COM ABI.
        body = re.sub(r'#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS\n(.*?)#else\n.*?#endif\n', r'\1', body, flags=re.S)
        methods = []
        for mm in re.finditer(r'virtual\s+(.+?)\s+STDMETHODCALLTYPE\s+(\w+)\(\s*(.*?)\)\s*=\s*0;', body, re.S):
            ret, mname, raw = mm.group(1).strip(), mm.group(2), mm.group(3).strip()
            params = []
            if raw:
                for p in raw.split(","):
                    p = " ".join(p.split())
                    pm = re.match(r'^(.*?)(\w+)(\[\d*\])?$', p)
                    if not pm:
                        sys.exit(f"cannot parse parameter {p!r} of {name}::{mname}")
                    params.append((pm.group(1).strip(), pm.group(3) or ""))
            methods.append((ret, mname, params))
        interfaces[name] = (base, methods)
    return interfaces


def chain_of(interfaces, leaf):
    chain = []
    cur = leaf
    while cur != "IUnknown":
        if cur not in interfaces:
            sys.exit(f"interface {cur} not found in header")
        chain.append(cur)
        cur = interfaces[cur][0]
    return list(reversed(chain))


def default_return(ret, iface, slot, name):
    stub = f'BX_VTABLE_STUB_LOG("{iface}", {slot}u, "{name}");'
    if ret == "void":
        return f"{stub} return;"
    if ret == "HRESULT":
        return f"{stub} return E_NOTIMPL;"
    if ret.endswith("*"):
        return f"{stub} *a0 = {{}}; return a0;"
    return f"{stub} return {{}};"


def main():
    header = sys.argv[1] if len(sys.argv) > 1 else find_header()
    interfaces = parse_interfaces(Path(header).read_text(encoding="utf-8", errors="replace"))

    h = ["// Generated by shims/d3d12_xs/tools/gen_proxy.py from the toolchain's <d3d12.h>. Do not edit by hand.",
         "#ifndef BETTERXEN_D3D12_XS_PROXY_GENERATED_H", "#define BETTERXEN_D3D12_XS_PROXY_GENERATED_H", ""]
    h_pre = []
    c = ["// Generated by shims/d3d12_xs/tools/gen_proxy.py from the toolchain's <d3d12.h>. Do not edit by hand.",
         '#include "proxy.h"', ""]

    for cls, (leaf, custom) in PROXIES.items():
        chain = chain_of(interfaces, leaf)
        levels = [i for i in chain if i not in ABSTRACT]
        root = levels[0]
        seen_custom = set()
        handoff = HANDOFF.get(cls, {})
        seen_handoff = set()
        if custom & handoff.keys():
            sys.exit(f"{cls}: methods in both CUSTOM and HANDOFF: {sorted(custom & handoff.keys())}")

        h += [f"class {cls} : public {leaf} {{", "public:",
              f'    static constexpr const char* kLeafName = "{leaf}";']
        for lvl in levels:
            h.append(f"    {lvl}* h_{lvl} = nullptr;")
        h += ["", "    /* Takes one reference per interface level the host object implements. */",
              "    void bx_acquire_hosts(IUnknown* host);", "    void bx_release_hosts();",
              "    /* Returns this when `iid` names a level of this proxy that the host implements, else nullptr. */",
              "    void* bx_match_iid(REFIID iid);", ""]

        c += [f"void {cls}::bx_acquire_hosts(IUnknown* host) {{"]
        for lvl in levels:
            c.append(f"    host->QueryInterface(__uuidof({lvl}), reinterpret_cast<void**>(&h_{lvl}));")
        c += ["}", "", f"void {cls}::bx_release_hosts() {{"]
        for lvl in reversed(levels):
            c.append(f"    if (h_{lvl}) {{ h_{lvl}->Release(); h_{lvl} = nullptr; }}")
        c += ["}", "", f"void* {cls}::bx_match_iid(REFIID iid) {{",
              "    if (IsEqualGUID(iid, __uuidof(IUnknown))) return this;"]
        for iface in chain:
            cond = f"IsEqualGUID(iid, __uuidof({iface}))"
            if iface not in ABSTRACT:
                cond += f" && h_{iface}"
            c.append(f"    if ({cond}) return this;")
        c += ["    return nullptr;", "}", ""]

        slot = 0
        for iface, methods in [("IUnknown", IUNKNOWN)] + [(i, interfaces[i][1]) for i in chain]:
            h.append(f"    /* {iface} */")
            host = f"h_{root}" if iface in ABSTRACT else f"h_{iface}"
            for ret, name, params in methods:
                decl = ", ".join(f"{t} a{n}{arr}" for n, (t, arr) in enumerate(params))
                pure = " = 0" if name in custom else ""
                h.append(f"    {ret} STDMETHODCALLTYPE {name}({decl}) override{pure};")
                if name in custom:
                    seen_custom.add(name)
                    slot += 1
                    continue
                if name in handoff:
                    seen_handoff.add(name)
                    fn = handoff[name]
                    host_type = root if iface in ABSTRACT else iface
                    h_pre.append(f"{ret} {fn}({host_type}* host{', ' if decl else ''}{decl});")
                    call = f"{fn}({', '.join([host] + [f'a{n}' for n in range(len(params))])})"
                    c += [f"{ret} STDMETHODCALLTYPE {cls}::{name}({decl}) {{",
                          f'    BX_D3D_TRACE("{iface}", "{name}");',
                          f"    if (!{host}) {{ {default_return(ret, iface, slot, name)} }}",
                          f"    {'' if ret == 'void' else 'return '}{call};", "}", ""]
                    slot += 1
                    continue
                args = []
                for n, (t, arr) in enumerate(params):
                    bare = t.replace("const", "").replace(" ", "")
                    if bare.endswith("*") and bare[:-1] in MAYBE_PROXY:
                        args.append(f"bx_unwrap(a{n})")
                    elif any(bare.startswith(mp + "*") for mp in MAYBE_PROXY) and bare.count("*") > 1:
                        sys.exit(f"{iface}::{name} takes an array of possibly-proxied objects; add it to CUSTOM")
                    else:
                        args.append(f"a{n}")
                call = f"{host}->{name}({', '.join(args)})"
                c += [f"{ret} STDMETHODCALLTYPE {cls}::{name}({decl}) {{",
                      f'    BX_D3D_TRACE("{iface}", "{name}");',
                      f"    if (!{host}) {{ {default_return(ret, iface, slot, name)} }}",
                      f"    {'' if ret == 'void' else 'return '}{call};", "}", ""]
                slot += 1
        missing = custom - seen_custom
        if missing:
            sys.exit(f"{cls}: custom methods not found in header: {sorted(missing)}")
        missing = handoff.keys() - seen_handoff
        if missing:
            sys.exit(f"{cls}: hand-off methods not found in header: {sorted(missing)}")

        h += ["", f"    static constexpr unsigned kPublicSlotCount = {slot}u;",
              "    /*",
              "     * Slots past the public vtable. A caller that indexes beyond the interface it was handed",
              "     * (for example a console-only method this proxy does not know) lands on a logged stub",
              "     * instead of unrelated memory. Names and signatures of such methods are unknown here.",
              "     */"]
        for k in range(PAD_SLOTS):
            h.append(f"    virtual HRESULT STDMETHODCALLTYPE BxBeyondPublicVtable{k:02d}();")
            c += [f"HRESULT STDMETHODCALLTYPE {cls}::BxBeyondPublicVtable{k:02d}() {{",
                  f'    BX_VTABLE_STUB_LOG("{leaf}", {slot + k}u, "beyond_public_vtable");', "    return E_NOTIMPL;", "}", ""]
        h += ["", "protected:", f"    ~{cls}() = default;", "};", ""]

    h += ["#endif"]
    if h_pre:
        h[4:4] = ["/* HANDOFF targets: the host interface, then the caller's arguments unmodified. */"] + h_pre + [""]
    (OUT_DIR / "proxy_generated.h").write_text("\n".join(h) + "\n", encoding="utf-8")
    (OUT_DIR / "proxy_generated.cpp").write_text("\n".join(c), encoding="utf-8")
    print(f"wrote proxy_generated.h/.cpp from {header}")


if __name__ == "__main__":
    main()
