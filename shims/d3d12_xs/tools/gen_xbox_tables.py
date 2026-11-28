#!/usr/bin/env python3
"""Generate the Xbox-layout function tables of the d3d12_xs proxy objects.

A title built against the Xbox D3D12 headers calls every method through a function table whose
order is not the PC D3D12 order (Xbox-only methods sit between and inside the PC ones). This
script reads two headers:

  * the Xbox Series header (d3d12_xs.h from the GDKX, used under Microsoft's authorization). Only
    facts are taken from it: interface names and IIDs, the slot order of each function table, and
    each method's name, return type and parameter types. Nothing from it is copied into the output
    verbatim beyond those names, and the header itself is never committed.
  * the toolchain's PC <d3d12.h>, which the shim is compiled against.

For every slot of the chosen Xbox interfaces it emits one of:
  * a typed forwarder to the PC method of the same name, when the Xbox and PC signatures are
    equivalent (the PC types are used, so the compiler handles struct returns and the calling
    convention); interface-pointer parameters are unwrapped and (REFIID, void**) results wrapped;
  * a shared IUnknown/GetDevice slot implemented in xbox_facade.cpp;
  * a stub that logs BX_VTABLE_STUB_LOG and returns a neutral value, for Xbox-only methods and for
    methods whose signatures differ.

Usage: python3 shims/d3d12_xs/tools/gen_xbox_tables.py /path/to/d3d12_xs.h [path/to/pc/d3d12.h]
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_proxy  # noqa: E402  (PC header parser shared with the PC proxy generator)

OUT_DIR = Path(__file__).resolve().parents[1]

# Xbox leaf interface -> (PC interface the host object must provide, C++ family tag)
FAMILIES = {
    "ID3D12Device11": ("ID3D12Device10", "Device"),
    "ID3D12Resource2": ("ID3D12Resource2", "Resource"),
    "ID3D12Heap1": ("ID3D12Heap1", "Heap"),
    "ID3D12Fence1": ("ID3D12Fence", "Fence"),          # D3DMetal answers ID3D12Fence, not Fence1
    "ID3D12DescriptorHeap": ("ID3D12DescriptorHeap", "DescriptorHeap"),
    "ID3D12RootSignature": ("ID3D12RootSignature", "RootSignature"),
    "ID3D12PipelineState": ("ID3D12PipelineState", "PipelineState"),
    "ID3D12CommandAllocator": ("ID3D12CommandAllocator", "CommandAllocator"),
    "ID3D12QueryHeap": ("ID3D12QueryHeap", "QueryHeap"),
    "ID3D12CommandSignature": ("ID3D12CommandSignature", "CommandSignature"),
    "ID3D12CommandQueue": ("ID3D12CommandQueue", "CommandQueue"),
    "ID3D12GraphicsCommandList8": ("ID3D12GraphicsCommandList7", "GraphicsCommandList"),
}
# Xbox-only slots implemented by hand in xbox_facade.cpp: family -> {method: function}.
CUSTOM = {
    "Device": {"CreateCommandQueueX": "bx_xbox_device_create_command_queue_x",
               "CreateCommandListX": "bx_xbox_device_create_command_list_x",
               "SetFrameIntervalX": "bx_xbox_device_set_frame_interval_x",
               "ScheduleFrameEventX": "bx_xbox_device_schedule_frame_event_x",
               "WaitFrameEventX": "bx_xbox_device_wait_frame_event_x"},
    "GraphicsCommandList": {"ResourceBarrier": "bx_xbox_list_resource_barrier",
                            "CopyTextureRegion": "bx_xbox_list_copy_texture_region"},
}
SHARED = {"QueryInterface": "bx_xbox_query_interface", "AddRef": "bx_xbox_add_ref",
          "Release": "bx_xbox_release", "GetDevice": "bx_xbox_get_device"}
# Interfaces whose pointers a forwarded call must unwrap from an Xbox object to its host.
WRAPPABLE = re.compile(r"^(const\s+)?(ID3D12\w+|IUnknown)\s*\*$")
WRAPPABLE_ARRAY = re.compile(r"^(ID3D12\w+)\s*\*\s*const\s*\*$")


def strip_sal(text):
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    # SAL annotations, including arguments with nested parentheses such as sizeof(UINT).
    out, i = [], 0
    for m in re.finditer(r"\b_[A-Za-z]\w*_\b", text):
        if m.start() < i:
            continue
        out.append(text[i:m.start()])
        j = m.end()
        while j < len(text) and text[j] == " ":
            j += 1
        if j < len(text) and text[j] == "(":
            depth = 0
            while j < len(text):
                depth += {"(": 1, ")": -1}.get(text[j], 0)
                j += 1
                if depth == 0:
                    break
        i = j
    out.append(text[i:])
    return " ".join("".join(out).split())


def canon(t):
    """Canonical spelling for comparing an Xbox and a PC parameter or return type."""
    t = " ".join(t.split())
    t = re.sub(r"\s*\*\s*", "*", t)
    t = re.sub(r"\s*&\s*", "&", t)
    t = t.replace("LPCWSTR", "const WCHAR*").replace("LPCSTR", "const char*")
    t = re.sub(r"\bWINBOOL\b", "BOOL", t)
    t = re.sub(r"\bFLOAT\b", "float", t)
    t = re.sub(r"^const IID&$", "REFIID", t)
    t = re.sub(r"^const GUID&$", "REFGUID", t)
    if "*" not in t and "&" not in t and "[" not in t:
        t = re.sub(r"^const ", "", t)          # top-level const on a by-value parameter
    return t


def xbox_param_type(p):
    p = " ".join(p.split())
    m = re.match(r"^(.*?)(\w+)(\[[^\]]*\])$", p)
    if m:
        return canon(m.group(1)) + m.group(3)
    m = re.match(r"^(.*[\s\*&])(\w+)$", p)
    return canon(m.group(1) if m else p)


def parse_xbox(text):
    iids = {m.group(1): m.groups()[1:] for m in re.finditer(
        r"^D3DINTERFACE\((\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+),"
        r"\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+)\)", text, re.M)}
    bases = {m.group(1): m.group(2) for m in re.finditer(
        r"^D3DINTERFACE\((\w+),[^)]*\)\s*:\s*public\s+(\w+)", text, re.M)}
    tables = {}
    for m in re.finditer(r"struct\s+(\w+)_Function_Table\s*\{((?:(?!struct\s+\w+_Function_Table\b).)*?)\n\s*\};", text, re.S):
        entries = []
        for decl in re.split(r";\s*\n", m.group(2) + "\n"):
            decl = strip_sal(decl).rstrip(";").strip()
            b = re.match(r"^(\w+)_Function_Table\s+\w+$", decl)
            if b:
                entries.append(("base", b.group(1)))
                continue
            f = re.match(r"^(.*?)\(\s*D3DAPI\s*\*\s*(\w+)\s*\)\s*\((.*)\)$", decl, re.S)
            if f:
                params = [x.strip() for x in f.group(3).split(",") if x.strip() and x.strip() != "void"]
                entries.append(("fn", (f.group(2), canon(f.group(1)), [xbox_param_type(p) for p in params])))
        tables[m.group(1)] = entries
    return iids, bases, tables


def flatten(tables, name):
    out = []
    for kind, v in tables.get(name, []):
        out += flatten(tables, v) if kind == "base" else [v]
    return out


def table_name(iface):
    return "D3D12" + iface[len("ID3D12"):] if iface.startswith("ID3D12") else iface


def pc_methods(interfaces, leaf):
    methods = {}
    for iface in gen_proxy.chain_of(interfaces, leaf):
        for ret, name, params in interfaces[iface][1]:
            methods.setdefault(name, (iface, ret, params))
    return methods


def classify(xret, xparams, pc):
    """Returns ('forward', struct_return) when the Xbox slot can call the PC method, else None."""
    _, pret, pparams = pc
    pp = [canon(t) + a for t, a in pparams]
    xp = xparams[1:]                                       # drop the explicit Xbox `this`
    pr = canon(pret)
    if xp == pp and (xret == pr or (xret == "void" and pr in ("void", "HRESULT"))):
        return ("forward", False)
    if pr.endswith("*") and pp and pp[0] == pr and pr[:-1] == xret and xp == pp[1:]:
        return ("forward", True)                           # Xbox returns by value, PC by pointer
    if xret == "void" and pr.endswith("*") and pp and pp[0] == pr and xp == pp:
        return ("forward", False)                          # Xbox out-pointer, PC returns that pointer
    return None


def emit_forward(family, slot, name, pc, struct_return):
    iface, pret, pparams = pc
    params = list(pparams)
    ret = pret[:-1].strip() if struct_return else pret
    if struct_return:
        params = params[1:]
    decl, call, pre, post = [], [], [], []
    for i, (t, arr) in enumerate(params):
        decl.append(f"{t} a{i}{arr}")
        if WRAPPABLE.match(t) and not arr:
            call.append(f"bx_xbox_unwrap_as<{WRAPPABLE.match(t).group(2)}>(a{i})")
        elif WRAPPABLE_ARRAY.match(t) and not arr:
            elem = WRAPPABLE_ARRAY.match(t).group(1)
            pre.append(f"BxXboxUnwrapArray<{elem}> u{i}(a{i}, {find_count(params, i, name)});")
            pre.append(f"if (!u{i}.ok()) return {failure_value(ret)};")
            call.append(f"u{i}.data()")
        else:
            call.append(f"a{i}")
    wrap_out = len(params) >= 2 and canon(params[-2][0]) == "REFIID" and canon(params[-1][0]) == "void**"
    host = f"bx_xbox_host<{iface}>(self)"
    lines = [f"static {ret} STDMETHODCALLTYPE x_{family}_{slot}(void* self{''.join(', ' + d for d in decl)}) {{",
             f'    BX_D3D_TRACE("Xbox{family}", "{name}");']
    lines += ["    " + p for p in pre]
    if struct_return:
        lines += [f"    {ret} result{{}};", f"    {host}->{name}(&result{''.join(', ' + c for c in call)});",
                  "    return result;"]
    elif ret == "void":
        lines.append(f"    {host}->{name}({', '.join(call)});")
    elif wrap_out and ret == "HRESULT":
        n = len(params)
        lines += [f"    HRESULT hr = {host}->{name}({', '.join(call)});",
                  f"    return bx_xbox_wrap_result(hr, a{n - 2}, a{n - 1});"]
    else:
        lines.append(f"    return {host}->{name}({', '.join(call)});")
    return lines + ["}"]


def find_count(params, index, name=""):
    # The element count is the nearest UINT parameter, before the array or (as in
    # SetEventOnMultipleFenceCompletion) after it.
    for j in list(range(index - 1, -1, -1)) + list(range(index + 1, len(params))):
        if canon(params[j][0]) in ("UINT", "UINT32"):
            return f"a{j}"
    sys.exit(f"no element count before array parameter {index} of {name}: {params}")


def failure_value(ret):
    return {"HRESULT": "E_OUTOFMEMORY", "void": ""}.get(ret, "{}")


STRUCT_TYPES = set()   # filled from the Xbox header's `typedef struct` names


def emit_stub(family, slot, name, xret, reason):
    stub = f'BX_VTABLE_STUB_LOG("Xbox{family}", {slot}u, "{name}");'
    if xret == "HRESULT":
        return [f"/* {reason} */",
                f"static HRESULT STDMETHODCALLTYPE x_{family}_{slot}(void*) {{ {stub} return E_NOTIMPL; }}"]
    if xret in STRUCT_TYPES:
        # A struct returned by value comes back through a hidden pointer passed first; it is
        # handed back unfilled (the stub log says so). Structs of 8 bytes or less are returned in
        # RAX instead; none of the stubbed slots returns one today.
        return [f"/* {reason}; returns struct {xret} */",
                f"static void* STDMETHODCALLTYPE x_{family}_{slot}(void* result, void*) {{ {stub} return result; }}"]
    # Integers, enums, BOOL and pointers come back zero; a void slot ignores the value.
    return [f"/* {reason} */",
            f"static UINT64 STDMETHODCALLTYPE x_{family}_{slot}(void*) {{ {stub} return 0; }}"]


def guid_literal(parts):
    g = [p.lower() for p in parts]
    return (f"{{0x{g[0]}, 0x{g[1]}, 0x{g[2]}, "
            f"{{0x{g[3]}, 0x{g[4]}, 0x{g[5]}, 0x{g[6]}, 0x{g[7]}, 0x{g[8]}, 0x{g[9]}, 0x{g[10]}}}}}")


def chain(bases, leaf):
    out, cur = [], leaf
    while cur in bases:
        out.append(cur)
        cur = bases[cur]
    return out + [cur]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    xtext = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")  # parts of the header use CRLF
    pc_header = sys.argv[2] if len(sys.argv) > 2 else gen_proxy.find_header()
    interfaces = gen_proxy.parse_interfaces(Path(pc_header).read_text(encoding="utf-8", errors="replace"))
    iids, bases, tables = parse_xbox(xtext)
    STRUCT_TYPES.update(re.findall(r"typedef\s+struct\s+(\w+)", xtext))
    STRUCT_TYPES.update(t for t in re.findall(r"^struct\s+(\w+)\s*$", xtext, re.M))

    note = ("// Generated by shims/d3d12_xs/tools/gen_xbox_tables.py from the Xbox Series D3D12 header (slot\n"
            "// order, names and types only) and the toolchain's <d3d12.h>. Do not edit by hand.")
    h = [note, "#ifndef BETTERXEN_D3D12_XS_XBOX_TABLES_GENERATED_H",
         "#define BETTERXEN_D3D12_XS_XBOX_TABLES_GENERATED_H", "", '#include "xbox_facade.h"', ""]
    c = [note, '#include "xbox_tables_generated.h"', "", "namespace {", ""]
    summary = []
    for leaf, (pc_leaf, family) in FAMILIES.items():
        slots = flatten(tables, table_name(leaf))
        if not slots:
            sys.exit(f"no Xbox function table for {leaf}")
        pc = pc_methods(interfaces, pc_leaf)
        counts = {"forward": 0, "shared": 0, "stub": 0}
        names = []
        for slot, (name, xret, xparams) in enumerate(slots):
            if name in SHARED or name in CUSTOM.get(family, {}):
                fn = SHARED.get(name) or CUSTOM[family][name]
                names.append(f"reinterpret_cast<const void*>(&{fn})")
                counts["shared"] += 1
                continue
            kind = classify(xret, xparams, pc[name]) if name in pc else None
            if kind:
                c += emit_forward(family, slot, name, pc[name], kind[1])
                counts["forward"] += 1
            else:
                reason = "signature differs from the PC method" if name in pc else "Xbox-only method"
                c += emit_stub(family, slot, name, xret, reason)
                counts["stub"] += 1
            names.append(f"reinterpret_cast<const void*>(&x_{family}_{slot})")
        c += ["", f"const void* const k{family}Table[] = {{"] + [f"    {n}," for n in names] + ["};", ""]
        family_iids = [i for i in chain(bases, leaf) if i in iids and i not in ("IGraphicsUnknown",)]
        c += [f"const IID k{family}Iids[] = {{"] + [f"    {guid_literal(iids[i])}, /* {i} */" for i in family_iids]
        c += ["};", ""]
        h.append(f"extern const BxXboxIface bx_xbox_{family};")
        summary.append(f"{family}: {len(slots)} slots, {counts}")
        c.append(f"/* {leaf}: {len(slots)} slots ({counts['forward']} forwarded, {counts['shared']} shared, "
                 f"{counts['stub']} stubs); hosted on {pc_leaf}. */")
        c += [f"const IID kHost{family} = __uuidof({pc_leaf});", ""]
    c += ["} // namespace", ""]
    for leaf, (pc_leaf, family) in FAMILIES.items():
        size = len(flatten(tables, table_name(leaf)))
        c.append(f'extern const BxXboxIface bx_xbox_{family} = {{"{leaf}", k{family}Iids, '
                 f"sizeof(k{family}Iids) / sizeof(IID), &kHost{family}, k{family}Table, {size}}};")
    c += ["", "extern const BxXboxIface* const bx_xbox_ifaces[] = {"]
    c += [f"    &bx_xbox_{family}," for _, family in FAMILIES.values()] + ["};",
          f"extern const size_t bx_xbox_iface_count = {len(FAMILIES)};"]
    h += ["", "extern const BxXboxIface* const bx_xbox_ifaces[];", "extern const size_t bx_xbox_iface_count;",
          "", "#endif"]
    (OUT_DIR / "xbox_tables_generated.h").write_text("\n".join(h) + "\n")
    (OUT_DIR / "xbox_tables_generated.cpp").write_text("\n".join(c) + "\n")
    print("\n".join(summary))


if __name__ == "__main__":
    main()
