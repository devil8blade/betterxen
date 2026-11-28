/*
 * Synthetic conformance test for the inferred Xbox Series DirectStorage COM
 * interfaces (IDStorageFactoryX, IDStorageFileX, IDStorageStatusArrayX,
 * IDStorageQueueX) in XCRDAPI.dll.
 */
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "bx_log.h"
/* Public DirectStorage header (SDK 1.4.0, MIT): used here only for the
 * PC-sized canary and the existing public DirectStorage error constants. */
#include "dstorage.h"

extern "C" __declspec(dllimport) HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv);

struct XboxQueueDesc {
    UINT64 source_type;
    UINT16 capacity;
    INT8 priority;
    UINT8 reserved_pad[5];
    PCSTR name;
    UINT64 flags;
};

struct XboxRequest {
    UINT64 options;
    void* destination;
    UINT64 destination_pad;
    UINT32 destination_size;
    UINT32 reserved1c;
    void* source_ptr;
    UINT64 file_offset;
    UINT32 source_size;
    UINT32 reserved34;
};

struct IXboxDStorageFile : IUnknown {
    virtual void STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFileInformation(BY_HANDLE_FILE_INFORMATION* info) = 0;
};

struct IXboxDStorageStatusArray : IUnknown {
    virtual bool STDMETHODCALLTYPE IsComplete(UINT32 index) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetHResult(UINT32 index) = 0;
};

struct IXboxDStorageQueue : IUnknown {
    virtual void STDMETHODCALLTYPE EnqueueRequest(const XboxRequest* request) = 0;
    virtual void STDMETHODCALLTYPE EnqueueStatus(IXboxDStorageStatusArray* status_array, UINT32 index) = 0;
    virtual void STDMETHODCALLTYPE EnqueueSignal(void* fence, UINT64 value) = 0;
    virtual void STDMETHODCALLTYPE Submit() = 0;
    virtual void STDMETHODCALLTYPE CancelRequestsWithTag(UINT64 mask, UINT64 value) = 0;
    virtual void STDMETHODCALLTYPE Close() = 0;
    virtual HANDLE STDMETHODCALLTYPE GetErrorEvent() = 0;
    virtual void STDMETHODCALLTYPE RetrieveErrorRecord(void* record) = 0;
    virtual void STDMETHODCALLTYPE Query(void* info) = 0;
};

struct IXboxDStorageFactory : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateQueue(const void* desc, REFIID riid, void** ppv) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenFile(const WCHAR* path, REFIID riid, void** ppv) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateStatusArray(UINT32 capacity, PCSTR name, REFIID riid, void** ppv) = 0;
    virtual void STDMETHODCALLTYPE SetDebugFlags(UINT32 flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE StubSlot7() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetStagingBufferSize(UINT32 size) = 0;
    virtual HRESULT STDMETHODCALLTYPE StubSlot9() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateQueueX(const XboxQueueDesc* desc, REFIID riid, void** ppv) = 0;
};

namespace {

constexpr IID kXboxFactoryIid{0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr IID kXboxFileIid{0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};
constexpr IID kXboxStatusArrayIid{0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
constexpr IID kXboxQueueIid{0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};

int g_failed = 0;

void check(bool cond, const char* fmt, ...) {
    if (cond) return;
    ++g_failed;
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "FAIL: ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

bool wait_complete(IXboxDStorageStatusArray* status, UINT32 index) {
    for (int i = 0; i < 500; ++i) {
        if (status->IsComplete(index)) return true;
        Sleep(10);
    }
    return false;
}

/* Reads only what this run appended: bx-cli run appends to a supplied BX_LOG_FILE,
 * so records from an earlier run must not satisfy these checks. */
long g_log_start = 0;

long log_size(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fclose(f);
    return size < 0 ? 0 : size;
}

bool file_contains(const char* path, const char* needle) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, g_log_start, SEEK_SET);
    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
    }
    std::fclose(f);
    return text.find(needle) != std::string::npos;
}

} // namespace

int main() {
    /* BX_LOG_FILE must name the log before launch (bx-cli run --log): the log checks
     * below are the only evidence for the BX_VTABLE_STUB_LOG records. */
    char log_path[MAX_PATH] = {};
    const DWORD log_len = GetEnvironmentVariableA("BX_LOG_FILE", log_path, sizeof(log_path));
    if (log_len == 0 || log_len >= sizeof(log_path)) {
        std::fprintf(stderr, "FAIL: BX_LOG_FILE must be set before launch (run under bx-cli run --log)\n");
        return 1;
    }
    g_log_start = log_size(log_path);

    IXboxDStorageFactory* factory = nullptr;
    HRESULT hr = DStorageGetFactory(kXboxFactoryIid, reinterpret_cast<void**>(&factory));
    check(hr == S_OK && factory != nullptr, "DStorageGetFactory(Xbox) must succeed (hr=0x%08lx)",
          static_cast<unsigned long>(hr));
    if (!factory) return 1;

    /* Slot 8: SetStagingBufferSize (32 MiB, the size observed at target-build startup). */
    hr = factory->SetStagingBufferSize(0x2000000u);
    check(hr == S_OK, "SetStagingBufferSize(32 MiB) must return S_OK (hr=0x%08lx)", static_cast<unsigned long>(hr));

    /* Negative queue creation checks. */
    XboxQueueDesc bad_cap{};
    bad_cap.source_type = 0;
    bad_cap.capacity = 1;
    IXboxDStorageQueue* bad_q = nullptr;
    check(FAILED(factory->CreateQueueX(&bad_cap, kXboxQueueIid, reinterpret_cast<void**>(&bad_q))) && !bad_q,
          "CreateQueueX with capacity 1 must fail");

    XboxQueueDesc bad_src{};
    bad_src.source_type = 2;
    bad_src.capacity = 64;
    check(FAILED(factory->CreateQueueX(&bad_src, kXboxQueueIid, reinterpret_cast<void**>(&bad_q))) && !bad_q,
          "CreateQueueX with source_type 2 must fail");

    /* Create temp file for file-to-memory read test. */
    WCHAR temp_dir[MAX_PATH] = {};
    WCHAR temp_file[MAX_PATH] = {};
    check(GetTempPathW(MAX_PATH, temp_dir) > 0, "GetTempPathW must succeed");
    check(GetTempFileNameW(temp_dir, L"bxx", 0, temp_file) != 0, "GetTempFileNameW must succeed");

    uint8_t payload[256];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xFFu);
    }
    HANDLE raw = CreateFileW(temp_file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(raw != INVALID_HANDLE_VALUE, "CreateFileW for test payload must succeed");
    if (raw != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        check(WriteFile(raw, payload, sizeof(payload), &written, nullptr) && written == sizeof(payload),
              "WriteFile must write full payload");
        CloseHandle(raw);
    }

    /* Slot 4: OpenFile. */
    IXboxDStorageFile* file = nullptr;
    hr = factory->OpenFile(temp_file, kXboxFileIid, reinterpret_cast<void**>(&file));
    check(hr == S_OK && file != nullptr, "OpenFile(Xbox) must succeed (hr=0x%08lx)", static_cast<unsigned long>(hr));
    if (file) {
        BY_HANDLE_FILE_INFORMATION info{};
        hr = file->GetFileInformation(&info);
        check(hr == S_OK && info.nFileSizeLow == sizeof(payload) && info.nFileSizeHigh == 0,
              "GetFileInformation must report exact payload size");
    }

    /* Slot 5: CreateStatusArray. */
    IXboxDStorageStatusArray* status = nullptr;
    hr = factory->CreateStatusArray(2, "xbox_test_status", kXboxStatusArrayIid, reinterpret_cast<void**>(&status));
    check(hr == S_OK && status != nullptr, "CreateStatusArray(Xbox) must succeed (hr=0x%08lx)",
          static_cast<unsigned long>(hr));
    if (status) {
        check(!status->IsComplete(0) && status->GetHResult(0) == E_PENDING,
              "fresh status slot must be pending");
    }

    /* Slot 10: CreateQueueX (file source) and file-to-memory transfer. */
    XboxQueueDesc file_q_desc{};
    file_q_desc.source_type = 0;
    file_q_desc.capacity = 64;
    file_q_desc.priority = 0;
    file_q_desc.name = "xbox_file_queue";
    file_q_desc.flags = 1;
    IXboxDStorageQueue* file_q = nullptr;
    hr = factory->CreateQueueX(&file_q_desc, kXboxQueueIid, reinterpret_cast<void**>(&file_q));
    check(hr == S_OK && file_q != nullptr, "CreateQueueX(file) must succeed (hr=0x%08lx)",
          static_cast<unsigned long>(hr));

    if (file_q && file && status) {
        uint8_t dest[128] = {};
        XboxRequest req{};
        req.options = 0;
        req.destination = dest;
        req.destination_size = sizeof(dest);
        req.source_ptr = file;
        req.file_offset = 64;
        req.source_size = sizeof(dest);
        file_q->EnqueueRequest(&req);
        file_q->EnqueueStatus(status, 0);
        file_q->Submit();
        check(wait_complete(status, 0), "file read status must complete");
        check(status->GetHResult(0) == S_OK, "file read status must be S_OK (hr=0x%08lx)",
              static_cast<unsigned long>(status->GetHResult(0)));
        check(std::memcmp(dest, payload + 64, sizeof(dest)) == 0, "file read bytes must match payload[64..192]");

        /* Compressed request must fail with E_NOTIMPL and log stub. */
        XboxRequest comp_req = req;
        comp_req.options = 1;
        file_q->EnqueueRequest(&comp_req);
        file_q->EnqueueStatus(status, 1);
        file_q->Submit();
        check(wait_complete(status, 1), "compressed request status must complete");
        check(status->GetHResult(1) == E_NOTIMPL, "compressed request must report E_NOTIMPL (hr=0x%08lx)",
              static_cast<unsigned long>(status->GetHResult(1)));

        /* The Xbox output layout is unverified. A PC-sized test buffer
         * detects any accidental writes while keeping the test memory safe. */
        DSTORAGE_ERROR_RECORD record;
        std::memset(&record, 0xa5, sizeof(record));
        unsigned char before[sizeof(record)];
        std::memcpy(before, &record, sizeof(record));
        file_q->RetrieveErrorRecord(&record);
        check(std::memcmp(&record, before, sizeof(record)) == 0,
              "Xbox RetrieveErrorRecord must leave the caller's buffer unchanged");
        file_q->Close();
        file_q->Release();
    }

    /* Slot 10: CreateQueueX (memory source) and memory-to-memory transfer. */
    XboxQueueDesc mem_q_desc{};
    mem_q_desc.source_type = 1;
    mem_q_desc.capacity = 64;
    mem_q_desc.priority = 1;
    mem_q_desc.name = "xbox_mem_queue";
    mem_q_desc.flags = 1;
    IXboxDStorageQueue* mem_q = nullptr;
    hr = factory->CreateQueueX(&mem_q_desc, kXboxQueueIid, reinterpret_cast<void**>(&mem_q));
    check(hr == S_OK && mem_q != nullptr, "CreateQueueX(memory) must succeed (hr=0x%08lx)",
          static_cast<unsigned long>(hr));

    if (mem_q && status) {
        uint8_t dest[64] = {};
        XboxRequest req{};
        req.options = 0x400000ull;
        req.destination = dest;
        req.destination_size = sizeof(dest);
        req.source_ptr = payload + 32;
        req.source_size = sizeof(dest);
        mem_q->EnqueueRequest(&req);
        mem_q->EnqueueStatus(status, 0);
        mem_q->Submit();
        check(wait_complete(status, 0), "memory copy status must complete");
        check(status->GetHResult(0) == S_OK, "memory copy status must be S_OK (hr=0x%08lx)",
              static_cast<unsigned long>(status->GetHResult(0)));
        check(std::memcmp(dest, payload + 32, sizeof(dest)) == 0, "memory copy bytes must match payload[32..96]");

        /* A rejected null fence still signals the queue's error event. */
        mem_q->EnqueueSignal(nullptr, 1);
        mem_q->Submit();
        const HANDLE error_event = mem_q->GetErrorEvent();
        check(error_event != nullptr && WaitForSingleObject(error_event, 5000) == WAIT_OBJECT_0,
              "the error event must signal after the rejected signal is processed");

        XboxRequest null_src = req;
        null_src.source_ptr = nullptr;
        mem_q->EnqueueRequest(&null_src);
        mem_q->EnqueueStatus(status, 1);
        mem_q->Submit();
        check(wait_complete(status, 1) && status->GetHResult(1) == E_INVALIDARG,
              "a null memory source must report E_INVALIDARG through queue status");
        mem_q->Close();
        mem_q->Release();
    }

    /* Error paths on a fresh file queue. The fixture checks exact rejected
     * methods and HRESULTs in the fresh log without using an Xbox record layout. */
    IXboxDStorageQueue* err_q = nullptr;
    XboxQueueDesc err_desc = file_q_desc;
    err_desc.name = "xbox_error_queue";
    hr = factory->CreateQueueX(&err_desc, kXboxQueueIid, reinterpret_cast<void**>(&err_q));
    check(hr == S_OK && err_q != nullptr, "CreateQueueX(error paths) must succeed (hr=0x%08lx)",
          static_cast<unsigned long>(hr));
    if (err_q && status) {
        const HANDLE error_event = err_q->GetErrorEvent();
        /* A memory-source request (option 0x400000, as on the memory queue above) on a
         * file queue is rejected before its source pointer is touched. */
        uint8_t dest[16] = {};
        XboxRequest bad_src{};
        bad_src.options = 0x400000ull;
        bad_src.destination = dest;
        bad_src.destination_size = sizeof(dest);
        bad_src.source_ptr = payload;
        bad_src.source_size = sizeof(dest);
        err_q->EnqueueRequest(&bad_src);
        err_q->Submit();
        check(error_event != nullptr && WaitForSingleObject(error_event, 5000) == WAIT_OBJECT_0,
              "error event must signal for a memory request on a file queue");

        /* A source/destination size mismatch on an otherwise valid file request. */
        XboxRequest mismatch{};
        mismatch.destination = dest;
        mismatch.destination_size = sizeof(dest);
        mismatch.source_ptr = file;
        mismatch.source_size = sizeof(dest) - 1;
        err_q->EnqueueRequest(&mismatch);
        err_q->Submit();

        /* A status index past the array's capacity (2). */
        err_q->EnqueueStatus(status, 5);
        err_q->Submit();

        /* An enqueue after Close is dropped and logged, not run. */
        err_q->Close();
        uint8_t after_close[16] = {};
        XboxRequest late{};
        late.destination = after_close;
        late.destination_size = sizeof(after_close);
        late.source_ptr = file;
        late.source_size = sizeof(after_close);
        err_q->EnqueueRequest(&late);
        err_q->Release();
    }

    if (status) status->Release();
    if (file) {
        file->Close();
        file->Release();
    }
    factory->Release();
    DeleteFileW(temp_file);

    {
        check(file_contains(log_path, "\"event\":\"dstorage_get_factory\",\"detail\":\"dispatch xbox_com=IDStorageFactoryX\""),
              "log must record Xbox factory dispatch");
        check(file_contains(log_path, "xbox size=33554432 has no effect"),
              "log must record that Xbox SetStagingBufferSize has no effect");
        check(file_contains(log_path, "dstorage_queue_closed"),
              "log must record the request enqueued after Close as dropped");
        check(file_contains(log_path, "IDStorageQueueX[3]::EnqueueRequest_compressed"),
              "log must record compressed request stub");
        check(file_contains(log_path, "IDStorageQueueX[10]::RetrieveErrorRecord"),
              "log must record the unimplemented Xbox error-record output");
        const auto check_rejection = [&](const char* method, HRESULT hr) {
            char needle[96];
            std::snprintf(needle, sizeof(needle), "xbox %s hr=0x%08lx", method, static_cast<unsigned long>(hr));
            check(file_contains(log_path, needle), "log must record: %s", needle);
        };
        check_rejection("EnqueueSignal", E_DSTORAGE_INVALID_FENCE);
        check_rejection("EnqueueRequest", E_DSTORAGE_INVALID_SOURCE_TYPE);
        check_rejection("EnqueueRequest", E_DSTORAGE_INVALID_DESTINATION_SIZE);
        check_rejection("EnqueueStatus", E_DSTORAGE_INDEX_BOUND);
    }

    if (g_failed != 0) {
        std::fprintf(stderr, "FAIL: test_xcrdapi_xbox (%d failed)\n", g_failed);
        return 1;
    }
    std::printf("PASS: test_xcrdapi_xbox\n");
    return 0;
}
