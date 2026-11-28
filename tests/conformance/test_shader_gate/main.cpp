/*
 * M2 Shader Format Gate (#466): synthetic conformance test for the PSO hook in d3d12_xs.dll.
 *
 * Through D3D12XboxCreateDevice it:
 *  1. creates a compute PSO from a real DXC 1.9.2609 SM 6.6 DXIL blob (fill_cs66, from
 *     d3dmetal-harness), dispatches it and checks all 256 output words on the CPU;
 *  2. submits a synthetic ACCC-only container (BetterXen FSPX v1, compute) to
 *     CreateComputePipelineState and to CreateGraphicsPipelineState and requires E_NOTIMPL
 *     with no object and no call reaching the host device;
 *  3. creates PSOs from the #453 [WaveSize(64)] and 64-bit typed-atomic blobs and requires the
 *     gate to log their constraints;
 *  4. checks the structured `shader_gate` JSONL records, including the field order and
 *     space-free values bx-telemetry's key=value parser relies on (#535).
 *
 * It then runs a copy of itself with --no-bridge from a directory holding d3d12_xs.dll but not
 * the bridge, which must still run DXIL and refuse ACCC-only with E_NOTIMPL.
 *
 * Expected values come from the D3D12 contract, the DXBC layout and the documented kernel, not
 * from the shim. Needs bx_shader_bridge.dll next to the shims and BX_LOG_FILE (bx-cli --log).
 * It also checks that the bridge was loaded from d3d12_xs.dll's directory; run it with the EXE
 * in a different directory from the shims to exercise that lookup.
 */
#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#endif
#include <windows.h>
#include <unknwn.h>
#include <d3d12.h>

#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "atomic64_image.h"  // g_atomic64_image_cs, #453
#include "fill_cs66.h"       // g_fill_cs66, d3dmetal-harness
#include "wave64_lanes.h"    // g_wave64_lanes_cs, #453

extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(PVOID params, REFIID riid, void** ppv);
extern "C" __declspec(dllimport) HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc, ID3DBlob** blob, ID3DBlob** error);

namespace {

int g_checks = 0;
int g_failures = 0;

__attribute__((format(gnu_printf, 2, 3)))
bool check(bool ok, const char* fmt, ...) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        char msg[320];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::fflush(stderr);
    }
    return ok;
}

unsigned long ul(HRESULT hr) { return static_cast<unsigned long>(hr); }

int finish() {
    if (g_failures) {
        std::fprintf(stderr, "FAIL: test_shader_gate (%d of %d checks failed)\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: test_shader_gate (%d checks)\n", g_checks);
    return 0;
}

/* The kernel in fill_cs66.hlsl: Out[i] = hash(i) for 4 groups of 64 threads. */
constexpr uint32_t kWords = 256;
uint32_t expected_word(uint32_t i) {
    uint32_t v = i * 0x9E3779B1u;
    v ^= v >> 15;
    return v + 0x01234567u;
}

void put32(std::vector<uint8_t>& out, size_t at, uint32_t value) { std::memcpy(out.data() + at, &value, 4); }

/* A DXBC container (version 1) holding one ACCC chunk with a BetterXen FSPX v1 table whose only
 * section is the stage configuration: compute, shader model 6.0. No DXIL. */
std::vector<uint8_t> accc_only_container() {
    std::vector<uint8_t> fspx(40, 0);
    std::memcpy(fspx.data(), "FSPX", 4);
    put32(fspx, 4, 1);   // version
    put32(fspx, 8, 1);   // section count
    put32(fspx, 16, 1);  // section id 1: stage configuration
    put32(fspx, 20, 28); // offset
    put32(fspx, 24, 12); // size
    put32(fspx, 28, 6);  // stage: compute
    put32(fspx, 32, 6);  // shader model 6.0
    put32(fspx, 36, 0);
    const uint32_t total = 32 + 4 + 8 + static_cast<uint32_t>(fspx.size());
    std::vector<uint8_t> out(total, 0);
    std::memcpy(out.data(), "DXBC", 4);
    put32(out, 20, 1);
    put32(out, 24, total);
    put32(out, 28, 1);
    put32(out, 32, 36);
    std::memcpy(out.data() + 36, "ACCC", 4);
    put32(out, 40, static_cast<uint32_t>(fspx.size()));
    std::memcpy(out.data() + 44, fspx.data(), fspx.size());
    return out;
}

std::string read_log() {
    char path[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("BX_LOG_FILE", path, sizeof(path));
    if (n == 0 || n >= sizeof(path)) return {};
    std::string text;
    if (FILE* file = std::fopen(path, "rb")) {
        char buffer[4096];
        size_t got;
        while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
        std::fclose(file);
    }
    return text;
}

/* True when one JSONL line holds every needle. */
bool log_has(const std::string& log, std::initializer_list<const char*> needles) {
    size_t start = 0;
    while (start < log.size()) {
        size_t end = log.find('\n', start);
        if (end == std::string::npos) end = log.size();
        const std::string line = log.substr(start, end - start);
        bool all = true;
        for (const char* needle : needles) all = all && line.find(needle) != std::string::npos;
        if (all) return true;
        start = end + 1;
    }
    return false;
}

/* Every JSONL line whose text holds `needle`, so shape checks can loop over
 * the records the shim actually wrote rather than the lines typed by hand in
 * the trace-summary fixtures. */
std::vector<std::string> log_lines_with(const std::string& log, const char* needle) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < log.size()) {
        size_t end = log.find('\n', start);
        if (end == std::string::npos) end = log.size();
        const std::string line = log.substr(start, end - start);
        if (line.find(needle) != std::string::npos) lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

/* The `detail` value of one JSONL line, or empty when there is none. */
std::string detail_of(const std::string& line) {
    const char* key = "\"detail\":\"";
    const size_t at = line.find(key);
    if (at == std::string::npos) return {};
    const size_t begin = at + std::strlen(key);
    size_t end = begin;
    while (end < line.size() && !(line[end] == '"' && line[end - 1] != '\\')) ++end;
    return line.substr(begin, end - begin);
}

/* The structured part of a `shader_gate` detail: everything before the first
 * token that has no `=`. The gap record ends in prose, so it stops there. */
std::vector<std::string> structured_tokens(const std::string& detail, bool stop_at_free_text) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < detail.size()) {
        while (i < detail.size() && detail[i] == ' ') ++i;
        if (i >= detail.size()) break;
        size_t end = detail.find(' ', i);
        if (end == std::string::npos) end = detail.size();
        const std::string token = detail.substr(i, end - i);
        if (token.find('=') == std::string::npos && stop_at_free_text) break;
        tokens.push_back(token);
        i = end;
    }
    return tokens;
}

ID3D12RootSignature* make_root(ID3D12Device* device, const D3D12_ROOT_PARAMETER& param) {
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    desc.Desc_1_0.NumParameters = 1;
    desc.Desc_1_0.pParameters = &param;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
    if (error) error->Release();
    if (!check(SUCCEEDED(hr) && blob, "D3D12SerializeVersionedRootSignature hr=0x%08lx", ul(hr))) return nullptr;
    ID3D12RootSignature* root = nullptr;
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                     reinterpret_cast<void**>(&root));
    blob->Release();
    check(SUCCEEDED(hr) && root, "CreateRootSignature hr=0x%08lx", ul(hr));
    return root;
}

ID3D12Resource* make_buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                            D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES props = {};
    props.Type = heap;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kWords * 4;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ID3D12Resource* buffer = nullptr;
    const HRESULT hr = device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                       __uuidof(ID3D12Resource), reinterpret_cast<void**>(&buffer));
    check(SUCCEEDED(hr) && buffer, "CreateCommittedResource heap=%d hr=0x%08lx", static_cast<int>(heap), ul(hr));
    return buffer;
}

HRESULT create_compute(ID3D12Device* device, ID3D12RootSignature* root, const void* code, size_t size,
                       ID3D12PipelineState** pso) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    desc.CS.pShaderBytecode = code;
    desc.CS.BytecodeLength = size;
    *pso = nullptr;
    return device->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(pso));
}

/* Builds a stream PSO using the REAL subobject layout: alignas(void*) on the class, so a
 * 4-byte-aligned payload sits at offset 4, not 8. Encoding the layout from the compiler,
 * rather than from the shim's own assumption, is what lets these tests catch a desync in the
 * shim's walker (#576 review). */
template<class Payload>
struct alignas(void*) TestSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    Payload payload;
};

struct StreamBuilder {
    std::vector<uint8_t> bytes;
    template<class Payload>
    void add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const Payload& payload) {
        using Record = TestSubobject<Payload>;
        const size_t at = bytes.size();
        bytes.resize(at + sizeof(Record), 0);
        std::memcpy(bytes.data() + at + offsetof(Record, type), &type, sizeof(type));
        std::memcpy(bytes.data() + at + offsetof(Record, payload), &payload, sizeof(payload));
    }
    D3D12_PIPELINE_STATE_STREAM_DESC desc() const {
        D3D12_PIPELINE_STATE_STREAM_DESC d = {};
        d.SizeInBytes = bytes.size();
        d.pPipelineStateSubobjectStream = const_cast<uint8_t*>(bytes.data());
        return d;
    }
};

StreamBuilder compute_stream(const void* code, size_t size, ID3D12RootSignature* root) {
    StreamBuilder stream;
    D3D12_SHADER_BYTECODE cs = {code, size};
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, cs);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root);
    return stream;
}

HRESULT create_stream(ID3D12Device* device, const D3D12_PIPELINE_STATE_STREAM_DESC& desc,
                      ID3D12PipelineState** pso) {
    *pso = nullptr;
    auto* device2 = static_cast<ID3D12Device2*>(device);
    return device2->CreatePipelineState(&desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(pso));
}

/* Runs fill_cs66 and checks every output word. */
void run_dxil(ID3D12Device* device, ID3D12RootSignature* root, ID3D12PipelineState* pso) {
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HRESULT hr = device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    if (!check(SUCCEEDED(hr), "CreateCommandQueue hr=0x%08lx", ul(hr))) return;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                        reinterpret_cast<void**>(&allocator));
    if (check(SUCCEEDED(hr), "CreateCommandAllocator hr=0x%08lx", ul(hr))) {
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso,
                                       __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list));
        check(SUCCEEDED(hr), "CreateCommandList hr=0x%08lx", ul(hr));
    }
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    check(SUCCEEDED(hr), "CreateFence hr=0x%08lx", ul(hr));
    ID3D12Resource* out = make_buffer(device, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* readback =
        make_buffer(device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (list && fence && out && readback) {
        list->SetComputeRootSignature(root);
        list->SetComputeRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
        list->Dispatch(kWords / 64, 1, 1);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = out;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        list->CopyResource(readback, out);
        hr = list->Close();
        check(SUCCEEDED(hr), "Close hr=0x%08lx", ul(hr));
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(SUCCEEDED(queue->Signal(fence, 1)), "Signal");
        if (fence->GetCompletedValue() < 1) {
            check(SUCCEEDED(fence->SetEventOnCompletion(1, done)), "SetEventOnCompletion");
            check(WaitForSingleObject(done, 10000) == WAIT_OBJECT_0, "fence did not complete within 10 s");
        }
        CloseHandle(done);
        check(fence->GetCompletedValue() != UINT64_MAX, "device removed (fence value UINT64_MAX)");

        void* mapped = nullptr;
        D3D12_RANGE range = {0, kWords * 4};
        hr = readback->Map(0, &range, &mapped);
        if (check(SUCCEEDED(hr) && mapped, "Map hr=0x%08lx", ul(hr))) {
            const auto* words = static_cast<const uint32_t*>(mapped);
            uint32_t mismatched = 0, first = kWords;
            for (uint32_t i = 0; i < kWords; ++i) {
                if (words[i] != expected_word(i)) {
                    if (!mismatched) first = i;
                    ++mismatched;
                }
            }
            check(mismatched == 0, "DXIL readback: %u of %u words wrong (first %u: got 0x%08x want 0x%08x)",
                  mismatched, kWords, first, first < kWords ? words[first] : 0u,
                  first < kWords ? expected_word(first) : 0u);
            D3D12_RANGE none = {0, 0};
            readback->Unmap(0, &none);
            if (!mismatched) std::printf("dxil: %u/%u words verified\n", kWords, kWords);
        }
    }
    if (readback) readback->Release();
    if (out) out->Release();
    if (fence) fence->Release();
    if (list) list->Release();
    if (allocator) allocator->Release();
    if (queue) queue->Release();
}

std::wstring directory_of(const wchar_t* path) {
    const std::wstring text(path);
    return text.substr(0, text.find_last_of(L'\\'));
}

/* --no-bridge: run from a directory that holds d3d12_xs.dll and its BetterXen imports but no
 * bx_shader_bridge.dll. DXIL must still run and ACCC-only must still be refused. */
int no_bridge_main() {
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    if (!check(SUCCEEDED(hr) && device, "no-bridge: D3D12XboxCreateDevice hr=0x%08lx", ul(hr))) return finish();
    D3D12_ROOT_PARAMETER uav = {};
    uav.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    uav.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    ID3D12RootSignature* root = make_root(device, uav);
    if (!root) return finish();
    ID3D12PipelineState* pso = nullptr;
    hr = create_compute(device, root, g_fill_cs66, sizeof(g_fill_cs66), &pso);
    check(SUCCEEDED(hr) && pso, "no-bridge: DXIL CreateComputePipelineState hr=0x%08lx", ul(hr));
    if (pso) {
        run_dxil(device, root, pso);
        pso->Release();
    }
    check(GetModuleHandleW(L"bx_shader_bridge.dll") == nullptr, "no-bridge: a bx_shader_bridge.dll was loaded anyway");
    const std::vector<uint8_t> accc = accc_only_container();
    hr = create_compute(device, root, accc.data(), accc.size(), &pso);
    check(hr == E_NOTIMPL && !pso, "no-bridge: ACCC-only compute hr=0x%08lx (want E_NOTIMPL)", ul(hr));
    if (pso) pso->Release();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics = {};
    graphics.pRootSignature = root;
    graphics.VS.pShaderBytecode = accc.data();
    graphics.VS.BytecodeLength = accc.size();
    graphics.SampleMask = UINT_MAX;
    graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphics.SampleDesc.Count = 1;
    hr = device->CreateGraphicsPipelineState(&graphics, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso));
    check(hr == E_NOTIMPL && !pso, "no-bridge: ACCC-only graphics VS hr=0x%08lx (want E_NOTIMPL)", ul(hr));
    if (pso) pso->Release();
    // The stream form refuses ACCC-only with or without the bridge (#576).
    StreamBuilder accc_stream = compute_stream(accc.data(), accc.size(), root);
    const D3D12_PIPELINE_STATE_STREAM_DESC accc_desc = accc_stream.desc();
    hr = create_stream(device, accc_desc, &pso);
    check(hr == E_NOTIMPL && !pso, "no-bridge: ACCC-only stream CS hr=0x%08lx (want E_NOTIMPL)", ul(hr));
    if (pso) pso->Release();
    root->Release();
    device->Release();
    return finish();
}

/* Copies this EXE and d3d12_xs.dll's directory minus the bridge into a scratch directory, runs
 * the copy with --no-bridge, and requires it to pass. */
void run_no_bridge_child() {
    wchar_t exe[MAX_PATH] = {}, shim[MAX_PATH] = {}, temp[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    GetModuleFileNameW(GetModuleHandleW(L"d3d12_xs.dll"), shim, MAX_PATH);
    GetTempPathW(MAX_PATH, temp);
    wchar_t dir[MAX_PATH];
    std::swprintf(dir, MAX_PATH, L"%lsbx_shader_gate_nobridge_%lu", temp, GetCurrentProcessId());
    if (!check(CreateDirectoryW(dir, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS,
               "no-bridge: cannot create %ls", dir)) {
        return;
    }
    const std::wstring from = directory_of(shim);
    const std::wstring to(dir);
    const wchar_t* files[] = {L"d3d12_xs.dll", L"xmem.dll", L"bx_runtime_veh.dll"};
    bool copied = CopyFileW(exe, (to + L"\\test_shader_gate.exe").c_str(), FALSE) != 0;
    for (const wchar_t* name : files) {
        copied = copied && CopyFileW((from + L"\\" + name).c_str(), (to + L"\\" + name).c_str(), FALSE) != 0;
    }
    if (check(copied, "no-bridge: copying the shims to %ls failed (error %lu)", dir, GetLastError())) {
        std::wstring command = L"\"" + to + L"\\test_shader_gate.exe\" --no-bridge";
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child = {};
        if (check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir, &startup, &child),
                  "no-bridge: CreateProcessW error %lu", GetLastError())) {
            DWORD code = 99;
            check(WaitForSingleObject(child.hProcess, 60000) == WAIT_OBJECT_0, "no-bridge: child did not finish in 60 s");
            GetExitCodeProcess(child.hProcess, &code);
            check(code == 0, "no-bridge: child exited %lu (a crash shows as an exception code)", code);
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
        }
    }
    DeleteFileW((to + L"\\test_shader_gate.exe").c_str());
    for (const wchar_t* name : files) DeleteFileW((to + L"\\" + name).c_str());
    RemoveDirectoryW(dir);
}

} // namespace

int main() {
    if (std::wcsstr(GetCommandLineW(), L" --no-bridge")) return no_bridge_main();

    // Self-check of the reference against values computed independently for d3dmetal-harness.
    check(expected_word(0) == 0x01234567u && expected_word(1) == 0x9f598b46u && expected_word(255) == 0x9a664b32u,
          "expected_word reference values");

    if (!check(GetEnvironmentVariableA("BX_LOG_FILE", nullptr, 0) > 0,
               "BX_LOG_FILE is not set; run through bx-cli run --log")) {
        return finish();
    }

    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    if (!check(SUCCEEDED(hr) && device, "D3D12XboxCreateDevice hr=0x%08lx", ul(hr))) return finish();

    D3D12_ROOT_PARAMETER uav = {};
    uav.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    uav.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    ID3D12RootSignature* root_uav = make_root(device, uav);
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER table = {};
    table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table.DescriptorTable.NumDescriptorRanges = 1;
    table.DescriptorTable.pDescriptorRanges = &range;
    table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    ID3D12RootSignature* root_table = make_root(device, table);
    if (!root_uav || !root_table) return finish();

    // 1. DXIL passes the gate and runs.
    ID3D12PipelineState* pso = nullptr;
    hr = create_compute(device, root_uav, g_fill_cs66, sizeof(g_fill_cs66), &pso);
    check(SUCCEEDED(hr) && pso, "DXIL SM 6.6 CreateComputePipelineState hr=0x%08lx", ul(hr));
    const HMODULE bridge = GetModuleHandleW(L"bx_shader_bridge.dll");
    if (!check(bridge != nullptr,
               "bx_shader_bridge.dll was not loaded beside d3d12_xs.dll: scripts/build-shims.sh stages it in "
               "build-shims/bin (#484)")) {
        if (pso) pso->Release();
        return finish();
    }
    // The shim must find the bridge beside itself, not through the EXE's directory (crate README).
    wchar_t bridge_path[MAX_PATH] = {}, shim_path[MAX_PATH] = {};
    GetModuleFileNameW(bridge, bridge_path, MAX_PATH);
    GetModuleFileNameW(GetModuleHandleW(L"d3d12_xs.dll"), shim_path, MAX_PATH);
    const auto dir_of = [](std::wstring path) { return path.substr(0, path.find_last_of(L'\\')); };
    check(_wcsicmp(dir_of(bridge_path).c_str(), dir_of(shim_path).c_str()) == 0,
          "bx_shader_bridge.dll was loaded from %ls, not beside d3d12_xs.dll (%ls)", bridge_path, shim_path);
    if (pso) {
        run_dxil(device, root_uav, pso);
        pso->Release();
    }

    // 2. An ACCC-only container is refused, for compute and graphics, without a host object.
    const std::vector<uint8_t> accc = accc_only_container();
    hr = create_compute(device, root_uav, accc.data(), accc.size(), &pso);
    check(hr == E_NOTIMPL && !pso, "ACCC-only compute PSO: hr=0x%08lx (want E_NOTIMPL) pso=%p", ul(hr),
          static_cast<void*>(pso));
    if (pso) pso->Release();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics = {};
    graphics.pRootSignature = root_uav;
    graphics.VS.pShaderBytecode = accc.data();
    graphics.VS.BytecodeLength = accc.size();
    graphics.SampleMask = UINT_MAX;
    graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphics.SampleDesc.Count = 1;
    hr = device->CreateGraphicsPipelineState(&graphics, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso));
    check(hr == E_NOTIMPL && !pso, "ACCC-only graphics VS: hr=0x%08lx (want E_NOTIMPL) pso=%p", ul(hr),
          static_cast<void*>(pso));
    if (pso) pso->Release();
    // The device is still usable after a refusal.
    hr = create_compute(device, root_uav, g_fill_cs66, sizeof(g_fill_cs66), &pso);
    check(SUCCEEDED(hr) && pso, "DXIL PSO after a refusal hr=0x%08lx", ul(hr));
    if (pso) pso->Release();

    // 2b. The stream form (ID3D12Device2::CreatePipelineState) is gated too (#576): the same
    //     ACCC-only container is refused through the stream, and DXIL passes and runs.
    {
        StreamBuilder refused = compute_stream(accc.data(), accc.size(), root_uav);
        const D3D12_PIPELINE_STATE_STREAM_DESC stream = refused.desc();
        hr = create_stream(device, stream, &pso);
        check(hr == E_NOTIMPL && !pso,
              "ACCC-only stream PSO: hr=0x%08lx (want E_NOTIMPL) pso=%p", ul(hr), static_cast<void*>(pso));
        if (pso) pso->Release();

        StreamBuilder accepted = compute_stream(g_fill_cs66, sizeof(g_fill_cs66), root_uav);
        const D3D12_PIPELINE_STATE_STREAM_DESC stream_dxil = accepted.desc();
        hr = create_stream(device, stream_dxil, &pso);
        check(SUCCEEDED(hr) && pso, "DXIL stream PSO hr=0x%08lx", ul(hr));
        if (pso) {
            run_dxil(device, root_uav, pso);
            pso->Release();
        }

        // A truncated stream, and an unknown subobject type, are rejected before the host.
        StreamBuilder truncated = compute_stream(g_fill_cs66, sizeof(g_fill_cs66), root_uav);
        D3D12_PIPELINE_STATE_STREAM_DESC short_desc = truncated.desc();
        short_desc.SizeInBytes -= 8;
        hr = create_stream(device, short_desc, &pso);
        check(hr == E_INVALIDARG && !pso, "truncated stream hr=0x%08lx (want E_INVALIDARG)", ul(hr));
        if (pso) pso->Release();

        StreamBuilder unknown = compute_stream(g_fill_cs66, sizeof(g_fill_cs66), root_uav);
        /* Append a record whose type tag is not a known subobject. */
        const size_t at = unknown.bytes.size();
        const uint32_t bogus = 0x7f;
        unknown.bytes.resize(at + 8, 0);
        std::memcpy(unknown.bytes.data() + at, &bogus, sizeof(bogus));
        const D3D12_PIPELINE_STATE_STREAM_DESC bogus_desc = unknown.desc();
        hr = create_stream(device, bogus_desc, &pso);
        check(hr == E_INVALIDARG && !pso, "unknown-subobject stream hr=0x%08lx (want E_INVALIDARG)", ul(hr));
        if (pso) pso->Release();

        // A graphics stream whose non-shader payloads are 4-byte aligned (SAMPLE_MASK,
        // RENDER_TARGET_FORMATS, RASTERIZER, BLEND, FLAGS) must still reach the host. The
        // first version of this change assumed every payload sat at offset 8 and desynced
        // here, rejecting a valid stream with E_INVALIDARG (#576 review).
        {
            // A *compute* stream carrying 4-byte-aligned payloads. The first version used
            // `g_fill_cs66` as a VS, which is a compute execution model and not a valid
            // vertex shader, so the host could reject it for a reason unrelated to payload
            // alignment (review on #577). NODE_MASK and FLAGS are UINT payloads at offset 4
            // and are legal subobjects for a compute PSO, so this exercises the layout
            // without depending on a vertex-stage fixture.
            StreamBuilder aligned_stream;
            aligned_stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root_uav);
            D3D12_SHADER_BYTECODE cs = {g_fill_cs66, sizeof(g_fill_cs66)};
            aligned_stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, cs);
            UINT node_mask = 0;
            aligned_stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, node_mask);
            UINT stream_flags = 0;
            aligned_stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, stream_flags);
            const D3D12_PIPELINE_STATE_STREAM_DESC aligned_desc = aligned_stream.desc();
            hr = create_stream(device, aligned_desc, &pso);
            check(SUCCEEDED(hr) && pso, "compute stream with 4-byte-aligned payloads hr=0x%08lx", ul(hr));
            if (pso) pso->Release();
        }

        // A duplicated subobject type is rejected by ID3D12Device2's own rules.
        StreamBuilder duplicate = compute_stream(g_fill_cs66, sizeof(g_fill_cs66), root_uav);
        D3D12_SHADER_BYTECODE again = {g_fill_cs66, sizeof(g_fill_cs66)};
        duplicate.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, again);
        const D3D12_PIPELINE_STATE_STREAM_DESC dup_desc = duplicate.desc();
        hr = create_stream(device, dup_desc, &pso);
        check(hr == E_INVALIDARG && !pso, "duplicate-CS stream hr=0x%08lx (want E_INVALIDARG)", ul(hr));
        if (pso) pso->Release();
    }

    // 3. Constraint-carrying DXIL still passes; its constraints are logged (results not dispatched).
    // A wave wider than 32 lanes is refused at creation (#591): D3DMetal creates the PSO
    // and never runs it, so an S_OK here would be a silent no-op. The fixture asserts the
    // failure rather than only reading the log, because the log alone cannot tell a caller
    // what happened.
    hr = create_compute(device, root_uav, g_wave64_lanes_cs, sizeof(g_wave64_lanes_cs), &pso);
    check(hr == E_NOTIMPL && !pso,
          "wave64 PSO is refused at creation: hr=0x%08lx (want E_NOTIMPL) pso=%p", ul(hr),
          static_cast<void*>(pso));
    if (pso) pso->Release();
    hr = create_compute(device, root_table, g_atomic64_image_cs, sizeof(g_atomic64_image_cs), &pso);
    std::printf("atomic64 typed PSO hr=0x%08lx\n", ul(hr));
    if (pso) pso->Release();

    // 4. Without the bridge (a separate process from a directory without it).
    run_no_bridge_child();

    // 5. The JSONL records.
    const std::string log = read_log();
    // The trace-summary fixtures in bx-telemetry/bx-cli are typed by hand from
    // reading shader_gate.cpp. These checks pin the shape the parser assumes,
    // against the log this fixture actually wrote, so the two cannot drift
    // apart silently (#535).
    {
        const std::vector<std::string> gate_lines = log_lines_with(log, "\"event\":\"shader_gate\"");
        check(!gate_lines.empty(), "the fixture wrote at least one shader_gate record");

        bool order_ok = true;
        bool classified_has_kind = true;
        bool unclassified_lacks_kind = true;
        bool values_have_no_space = true;
        for (const std::string& line : gate_lines) {
            const std::string detail = detail_of(line);
            const size_t pso = detail.find("pso=");
            const size_t stage = detail.find("stage=");
            const size_t decision = detail.find("decision=");
            const size_t hr = detail.find("hr=");
            // The summarizer extracts these by key, so the order the emitter
            // writes them is the order it relies on. Every key must be present:
            // a missing one is npos, the largest value, which would satisfy `<`.
            const bool all_present = pso != std::string::npos && stage != std::string::npos &&
                                     decision != std::string::npos && hr != std::string::npos;
            if (!(all_present && pso < stage && stage < decision && decision < hr)) {
                order_ok = false;
            }
            const bool unclassified = detail.find("unclassified") != std::string::npos;
            const bool has_kind = detail.find("kind=") != std::string::npos;
            /* A record with an inspection always logs a kind; the no-bridge records
             * (`*_unclassified`) and `pass_uninspected` have none. */
            const bool classified = !unclassified &&
                                    (detail.find("decision=pass_dxil") != std::string::npos ||
                                     detail.find("decision=reject_accc_only") != std::string::npos ||
                                     detail.find("decision=pass_original") != std::string::npos ||
                                     detail.find("decision=prepare_failed") != std::string::npos ||
                                     detail.find("decision=copy_limit") != std::string::npos);
            if (unclassified && has_kind) unclassified_lacks_kind = false;
            if (classified && !has_kind) classified_has_kind = false;
            for (const std::string& token : structured_tokens(detail, false)) {
                if (token.find('=') == std::string::npos) values_have_no_space = false;
            }
        }
        check(order_ok, "every shader_gate record writes pso, stage, decision, hr in that order");
        check(classified_has_kind, "a classified shader_gate record carries kind=");
        check(unclassified_lacks_kind, "an unclassified record carries no kind=");
        check(values_have_no_space,
              "no key=value in a shader_gate record is split by a space, which is what "
              "parse_key_values(..., false) assumes");

        const std::vector<std::string> gap_lines = log_lines_with(log, "\"event\":\"shader_d3dmetal_gap\"");
        check(!gap_lines.empty(), "the fixture wrote at least one gap record");
        bool gap_ok = true;
        bool gap_values_have_no_space = true;
        for (const std::string& line : gap_lines) {
            const std::string detail = detail_of(line);
            if (detail.find("pso=") == std::string::npos ||
                detail.find("stage=") == std::string::npos ||
                detail.find("wave_lanes_min=") == std::string::npos ||
                detail.find(':') == std::string::npos) {
                gap_ok = false;
            }
            // parse_key_values(..., true) stops at the first token without
            // '=', so every structured token must be a key=value pair.
            for (const std::string& token : structured_tokens(detail, true)) {
                if (token.find('=') == std::string::npos) gap_values_have_no_space = false;
            }
        }
        check(gap_ok,
              "every gap record carries pso=, stage=, wave_lanes_min= and the colon "
              "that ends its structured part");
        check(gap_values_have_no_space,
              "no key=value in a gap record's structured part is split by a space, which is "
              "what parse_key_values(..., true) assumes");
    }

    char fill_bytes[32];
    std::snprintf(fill_bytes, sizeof(fill_bytes), "bytes=%u ", static_cast<unsigned>(sizeof(g_fill_cs66)));
    check(log_has(log, {"\"event\":\"shader_gate\"", "pso=compute", "decision=pass_dxil", "kind=dxil", "sm=6.6",
                        fill_bytes, "atomic64= ", "wave_lanes=0-4294967295"}),
          "shader_gate record for the DXIL SM 6.6 compute shader");
    check(log_has(log, {"\"event\":\"shader_gate\"", "pso=compute", "decision=reject_accc_only", "hr=0x80004001",
                        "kind=accc_only", "sm=6.0"}),
          "shader_gate record rejecting the ACCC-only compute shader");
    check(log_has(log, {"\"event\":\"shader_gate\"", "pso=graphics", "stage=VS", "decision=reject_accc_only"}),
          "shader_gate record rejecting the ACCC-only vertex shader");
    check(log_has(log, {"\"event\":\"shader_gate\"", "pso=stream", "stage=CS", "decision=reject_accc_only"}),
          "shader_gate record rejecting the ACCC-only stream compute shader");
    check(log_has(log, {"\"event\":\"shader_gate\"", "pso=stream", "stage=CS", "decision=pass_dxil"}),
          "shader_gate record passing the DXIL stream compute shader");
    check(log_has(log, {"\"event\":\"pso_stream\"", "shaders_not_parsed=0"}),
          "pso_stream record says the stream's shaders were parsed");
    check(log_has(log, {"\"event\":\"shader_gate\"", "decision=reject_wave_wide", "hr=0x80004001"}),
          "shader_gate record refusing the wave64 stage");
    // The wave64 stage is now refused before `pass_dxil` is logged (#591), so the record to
    // assert is the rejection, carrying the same wave constraints the gap was measured on.
    check(log_has(log, {"\"event\":\"shader_gate\"", "decision=reject_wave_wide", "wave_ops=1",
                        "wave_lanes=64-64"}),
          "shader_gate record with WaveSize(64), refused");
    check(log_has(log, {"\"event\":\"shader_d3dmetal_gap\"", "wave_lanes_min=64"}),
          "shader_d3dmetal_gap record for WaveSize(64)");
    check(log_has(log, {"\"event\":\"shader_gate\"", "decision=pass_dxil", "int64_ops=1", "atomic64=typed,"}),
          "shader_gate record with 64-bit typed atomics");
    check(log_has(log, {"\"event\":\"shader_d3dmetal_gap\"", "atomic64_flags=0x400000"}),
          "shader_d3dmetal_gap record for 64-bit typed atomics");
    check(log_has(log, {"\"event\":\"shader_bridge_missing\"", "not found beside d3d12_xs.dll"}),
          "shader_bridge_missing record from the no-bridge child");
    check(log_has(log, {"\"event\":\"shader_gate\"", "pso=compute", "decision=reject_accc_only_unclassified",
                        "hr=0x80004001"}),
          "no-bridge child refused the ACCC-only compute shader");
    check(log_has(log, {"\"event\":\"shader_gate\"", "pso=graphics", "decision=reject_accc_only_unclassified"}),
          "no-bridge child refused the ACCC-only vertex shader");
    check(log_has(log, {"\"event\":\"shader_gate\"", "pso=compute", "decision=pass_unclassified"}),
          "no-bridge child passed DXIL unclassified");

    root_table->Release();
    root_uav->Release();
    device->Release();
    return finish();
}
