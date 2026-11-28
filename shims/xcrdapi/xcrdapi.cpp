#include "synthetic_storage.h"

// Public PC DirectStorage contract: Microsoft.Direct3D.DirectStorage 1.4.0,
// MIT per file, vendored unmodified under dstorage/ (see dstorage/README.md).
#include "dstorage_mingw.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

#include "bx_log.h"

namespace {
using namespace bx::storage;

HRESULT win32_error() { return HRESULT_FROM_WIN32(GetLastError()); }

class Lock {
public:
    explicit Lock(SRWLOCK& mutex) : mutex_(mutex) { AcquireSRWLockExclusive(&mutex_); }
    ~Lock() { ReleaseSRWLockExclusive(&mutex_); }

private:
    SRWLOCK& mutex_;
};

template<class Interface, const IID& interface_id>
class Object : public Interface {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!IsEqualIID(riid, interface_id) && !IsEqualIID(riid, IID_IUnknown)) {
            return E_NOINTERFACE;
        }
        *result = static_cast<Interface*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = InterlockedDecrement(&references_);
        if (!references) delete this;
        return references;
    }

protected:
    virtual ~Object() = default;

private:
    LONG references_ = 1;
};

template<class Interface>
HRESULT publish(Interface* object, REFIID iid, void** result) {
    const HRESULT status = object->QueryInterface(iid, result);
    object->Release();
    return status;
}

HRESULT duplicate_handle(HANDLE handle, HANDLE* copy) {
    const HANDLE process = GetCurrentProcess();
    return DuplicateHandle(process, handle, process, copy, 0, FALSE, DUPLICATE_SAME_ACCESS)
        ? S_OK : win32_error();
}

class StorageFile final : public Object<File, file_iid> {
public:
    explicit StorageFile(HANDLE handle) : handle_(handle) {}

    HRESULT STDMETHODCALLTYPE Close() override {
        Lock lock(mutex_);
        if (handle_ == INVALID_HANDLE_VALUE) return S_OK;
        if (!CloseHandle(handle_)) return win32_error();
        handle_ = INVALID_HANDLE_VALUE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFileInformation(BY_HANDLE_FILE_INFORMATION* info) override {
        if (!info) return E_POINTER;
        Lock lock(mutex_);
        if (handle_ == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        return GetFileInformationByHandle(handle_, info) ? S_OK : win32_error();
    }

    HRESULT STDMETHODCALLTYPE GetHandle(HANDLE* copy) override {
        if (!copy) return E_POINTER;
        *copy = nullptr;
        Lock lock(mutex_);
        if (handle_ == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        return duplicate_handle(handle_, copy);
    }

private:
    ~StorageFile() override { Close(); }

    SRWLOCK mutex_ = SRWLOCK_INIT;
    HANDLE handle_;
};

class StatusArray final : public Object<Status, status_iid> {
public:
    struct Slot {
        HRESULT result = E_PENDING;
        bool busy = false;
    };

    StatusArray(Slot* slots, UINT count) : slots_(slots), count_(count) {}

    HRESULT STDMETHODCALLTYPE GetResult(UINT index) override {
        if (index >= count_) return E_INVALIDARG;
        Lock lock(mutex_);
        return slots_[index].result;
    }

    HRESULT begin(UINT index) {
        if (index >= count_) return E_INVALIDARG;
        Lock lock(mutex_);
        if (slots_[index].busy) return HRESULT_FROM_WIN32(ERROR_BUSY);
        slots_[index] = {E_PENDING, true};
        return S_OK;
    }

    void complete(UINT index, HRESULT result) {
        Lock lock(mutex_);
        slots_[index] = {result, false};
    }

private:
    ~StatusArray() override { delete[] slots_; }

    SRWLOCK mutex_ = SRWLOCK_INIT;
    Slot* slots_;
    UINT count_;
};

// `failure` carries a command that was rejected when it was enqueued through the
// public DirectStorage interfaces. Those methods return void, so the rejection is
// reported in queue order, like a request that fails while it runs.
enum class Kind { request, status, signal, event, failure };

struct Entry {
    Kind kind;
    Request request{};
    HRESULT failure = S_OK;
    Kind failed_kind = Kind::request;
    HANDLE handle = nullptr;
    StatusArray* status = nullptr;
    UINT index = 0;
    ID3D12Fence* fence = nullptr;
    UINT64 value = 0;
};

template<class T>
struct ComRelease {
    T* pointer = nullptr;
    ~ComRelease() { if (pointer) pointer->Release(); }
    void** out() { return reinterpret_cast<void**>(&pointer); }
};

struct WinEvent {
    HANDLE handle = nullptr;
    ~WinEvent() { if (handle) CloseHandle(handle); }
};

// Queue::EnqueueRequest is vtable slot 3: IUnknown occupies 0..2.
constexpr UINT kEnqueueRequestSlot = 3;

void log_compression(UINT compression) {
    const char* method = "compression";
    if (compression == compression_lz) method = "compression_lz";
    else if (compression == compression_bcpack) method = "compression_bcpack";
    else if (compression == compression_gdeflate) method = "compression_gdeflate";
    BX_VTABLE_STUB_LOG("Queue", kEnqueueRequestSlot, method);
}

bool cpu_visible_heap(const D3D12_HEAP_PROPERTIES& heap) {
    if (heap.Type == D3D12_HEAP_TYPE_UPLOAD || heap.Type == D3D12_HEAP_TYPE_READBACK) return true;
    if (heap.Type != D3D12_HEAP_TYPE_CUSTOM) return false;
    return heap.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE
        || heap.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 width) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = width;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!list || !resource || before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

HRESULT read_bytes(HANDLE file, UINT64 offset, void* destination, const void* source, DWORD size) {
    if (!size) return S_OK;
    if (!destination) return E_INVALIDARG;
    if (!file) {
        if (!source) return E_INVALIDARG;
        std::memmove(destination, source, size);
        return S_OK;
    }

    OVERLAPPED operation{};
    operation.Offset = static_cast<DWORD>(offset);
    operation.OffsetHigh = static_cast<DWORD>(offset >> 32);
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return win32_error();

    DWORD bytes = 0;
    HRESULT result = S_OK;
    if (!ReadFile(file, destination, size, &bytes, &operation)) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            result = HRESULT_FROM_WIN32(error);
        } else if (!GetOverlappedResult(file, &operation, &bytes, TRUE)) {
            result = win32_error();
        }
    }
    CloseHandle(operation.hEvent);
    if (SUCCEEDED(result) && bytes != size) return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
    return result;
}

HRESULT write_mapped(HANDLE file, const Request& request) {
    D3D12_RANGE no_read{0, 0};
    void* mapped = nullptr;
    const HRESULT map_hr = request.resource->Map(0, &no_read, &mapped);
    if (FAILED(map_hr)) return map_hr;
    if (!mapped) {
        request.resource->Unmap(0, nullptr);
        return E_FAIL;
    }
    auto* destination = static_cast<unsigned char*>(mapped) + static_cast<SIZE_T>(request.resource_offset);
    const HRESULT read_hr = read_bytes(file, request.file_offset, destination, request.source, request.size);
    D3D12_RANGE written{
        static_cast<SIZE_T>(request.resource_offset),
        static_cast<SIZE_T>(request.resource_offset + request.size)};
    request.resource->Unmap(0, &written);
    return read_hr;
}

struct CopyResult {
    HRESULT hr = S_OK;
    // The queue's enqueue AddRef on the destination must stay held.
    bool keep_destination = false;
};

// BX_XCRDAPI_COPY_TIMEOUT_MS: see synthetic_storage.h. Default 10000, clamp 0..60000.
DWORD staged_copy_timeout_ms() {
    wchar_t buf[32];
    const DWORD n = GetEnvironmentVariableW(L"BX_XCRDAPI_COPY_TIMEOUT_MS", buf, 32);
    if (n == 0 || n >= 32) return 10000;
    DWORD value = 0;
    for (DWORD i = 0; i < n; ++i) {
        if (buf[i] < L'0' || buf[i] > L'9') return 10000;
        const DWORD digit = static_cast<DWORD>(buf[i] - L'0');
        if (value > (60000u - digit) / 10u) return 60000;
        value = value * 10u + digit;
    }
    return value;
}

bool defer_staged_copy() {
    wchar_t buf[8];
    const DWORD n = GetEnvironmentVariableW(L"BX_XCRDAPI_DEFER_COPY", buf, 8);
    return n == 1 && buf[0] == L'1';
}

// Objects created for one staged copy. After ExecuteCommandLists, destroying
// them while the fence is still below 1 is a use-after-free on the GPU timeline.
struct StagedGpu {
    ComRelease<ID3D12Device> device;
    ComRelease<ID3D12Resource> upload;
    ComRelease<ID3D12CommandQueue> queue;
    ComRelease<ID3D12CommandAllocator> allocator;
    ComRelease<ID3D12GraphicsCommandList> list;
    ComRelease<ID3D12Fence> fence;
    ComRelease<ID3D12Fence> defer;
    WinEvent done;
    bool executed = false;
    bool event_armed = false;

    bool completion_unconfirmed() const {
        if (!executed) return false;
        // A removed device is no longer executing the copy. UINT64_MAX from
        // GetCompletedValue means the same thing, so the removed check is first.
        if (device.pointer && device.pointer->GetDeviceRemovedReason() != S_OK) return false;
        if (fence.pointer && fence.pointer->GetCompletedValue() >= 1) return false;
        return true;
    }

    void leak() {
        bx_log_emit(BX_LOG_WARN, "xcrdapi", "staged_copy",
                    "staged copy completion unconfirmed, keeping GPU objects alive");
        device.pointer = nullptr;
        upload.pointer = nullptr;
        queue.pointer = nullptr;
        allocator.pointer = nullptr;
        list.pointer = nullptr;
        fence.pointer = nullptr;
        defer.pointer = nullptr;
        if (event_armed) done.handle = nullptr;
    }

    // Decide the leak before signaling `defer`, or the unblocked copy can retire
    // the completion fence and look confirmed.
    CopyResult finish(HRESULT hr) {
        const bool keep = completion_unconfirmed();
        ID3D12Fence* deferred = defer.pointer;
        if (keep) leak();
        if (deferred) (void)deferred->Signal(1);
        return {hr, keep};
    }
};

// Default-heap buffers are not mapped. Stage the bytes in an upload resource and
// copy them on the device that owns `dest`, which must be in D3D12_RESOURCE_STATE_COMMON.
// Failures before ExecuteCommandLists release every object. After it, an unconfirmed
// completion deliberately leaks them (and tells the caller to keep `dest`).
CopyResult copy_staged(HANDLE file, const Request& request) {
    StagedGpu staged;
    HRESULT hr = request.resource->GetDevice(__uuidof(ID3D12Device), staged.device.out());
    if (FAILED(hr) || !staged.device.pointer) return {FAILED(hr) ? hr : E_FAIL, false};

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const D3D12_RESOURCE_DESC upload_desc = buffer_desc(request.size);
    hr = staged.device.pointer->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        __uuidof(ID3D12Resource), staged.upload.out());
    if (FAILED(hr) || !staged.upload.pointer) return {FAILED(hr) ? hr : E_FAIL, false};

    D3D12_RANGE no_read{0, 0};
    void* mapped = nullptr;
    hr = staged.upload.pointer->Map(0, &no_read, &mapped);
    if (FAILED(hr) || !mapped) {
        if (SUCCEEDED(hr)) staged.upload.pointer->Unmap(0, nullptr);
        return {FAILED(hr) ? hr : E_FAIL, false};
    }
    hr = read_bytes(file, request.file_offset, mapped, request.source, request.size);
    staged.upload.pointer->Unmap(0, nullptr);
    if (FAILED(hr)) return {hr, false};

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = staged.device.pointer->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), staged.queue.out());
    if (FAILED(hr) || !staged.queue.pointer) return {FAILED(hr) ? hr : E_FAIL, false};

    hr = staged.device.pointer->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       __uuidof(ID3D12CommandAllocator), staged.allocator.out());
    if (FAILED(hr) || !staged.allocator.pointer) return {FAILED(hr) ? hr : E_FAIL, false};

    hr = staged.device.pointer->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, staged.allocator.pointer,
                                                  nullptr, __uuidof(ID3D12GraphicsCommandList), staged.list.out());
    if (FAILED(hr) || !staged.list.pointer) return {FAILED(hr) ? hr : E_FAIL, false};

    transition(staged.list.pointer, request.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    staged.list.pointer->CopyBufferRegion(request.resource, request.resource_offset, staged.upload.pointer, 0,
                                          request.size);
    transition(staged.list.pointer, request.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    hr = staged.list.pointer->Close();
    if (FAILED(hr)) return {hr, false};

    // Test seam: block this queue until finish() signals the fence, so a 0 ms
    // wait cannot observe a copy the GPU already retired. A failed defer falls
    // through to the normal race. It is not itself a copy failure.
    if (defer_staged_copy()) {
        hr = staged.device.pointer->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), staged.defer.out());
        if (FAILED(hr) || !staged.defer.pointer
            || FAILED(staged.queue.pointer->Wait(staged.defer.pointer, 1))) {
            if (staged.defer.pointer) staged.defer.pointer->Release();
            staged.defer.pointer = nullptr;
        }
    }

    ID3D12CommandList* lists[] = {staged.list.pointer};
    staged.queue.pointer->ExecuteCommandLists(1, lists);
    staged.executed = true;

    hr = staged.device.pointer->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), staged.fence.out());
    if (FAILED(hr) || !staged.fence.pointer) return staged.finish(FAILED(hr) ? hr : E_FAIL);
    hr = staged.queue.pointer->Signal(staged.fence.pointer, 1);
    if (FAILED(hr)) return staged.finish(hr);

    staged.done.handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!staged.done.handle) return staged.finish(win32_error());
    hr = staged.fence.pointer->SetEventOnCompletion(1, staged.done.handle);
    if (FAILED(hr)) return staged.finish(hr);
    staged.event_armed = true;

    const DWORD waited = WaitForSingleObject(staged.done.handle, staged_copy_timeout_ms());
    if (waited == WAIT_OBJECT_0) {
        if (staged.defer.pointer) (void)staged.defer.pointer->Signal(1);
        return {S_OK, false};
    }
    if (waited == WAIT_TIMEOUT) return staged.finish(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
    return staged.finish(win32_error());
}

CopyResult fulfill(HANDLE file, const Request& request) {
    if (request.compression) {
        log_compression(request.compression);
        return {E_NOTIMPL, false};
    }
    if (!request.size) return {};
    if (!request.resource) {
        return {read_bytes(file, request.file_offset, request.destination, request.source, request.size), false};
    }

    const D3D12_RESOURCE_DESC desc = request.resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return {E_INVALIDARG, false};
    if (request.resource_offset > desc.Width
        || static_cast<UINT64>(request.size) > desc.Width - request.resource_offset) {
        return {E_INVALIDARG, false};
    }

    D3D12_HEAP_PROPERTIES heap{};
    const HRESULT heap_hr = request.resource->GetHeapProperties(&heap, nullptr);
    if (SUCCEEDED(heap_hr)) {
        return cpu_visible_heap(heap) ? CopyResult{write_mapped(file, request), false}
                                      : copy_staged(file, request);
    }
    const HRESULT mapped = write_mapped(file, request);
    return SUCCEEDED(mapped) ? CopyResult{mapped, false} : copy_staged(file, request);
}

// Each queue is drained serially, in FIFO order, by a single threadpool work item.
// A `blocking` queue makes a full enqueue wait for a free slot (submitting what is
// pending first) instead of failing with ERROR_BUSY. A non-zero `auto_submit`
// submits once that many entries are waiting. The public DirectStorage queues use
// both; the BetterXen synthetic queues use neither.
class WorkQueue final : public Object<Queue, queue_iid> {
public:
    WorkQueue(Entry* entries, UINT capacity, bool blocking = false, UINT auto_submit = 0)
        : entries_(entries), capacity_(capacity), blocking_(blocking), auto_submit_(auto_submit) {}

    bool create_work() {
        work_ = CreateThreadpoolWork(run, this, nullptr);
        return work_ != nullptr;
    }

    HRESULT STDMETHODCALLTYPE EnqueueRequest(const Request* request) override {
        constexpr UINT64 max_file_offset = std::numeric_limits<LONGLONG>::max();
        if (!request
            || (request->file && request->source)
            || (request->destination && request->resource)
            || (request->size && !request->destination && !request->resource)
            || (request->size && !request->source && !request->file)
            || (request->file && request->file_offset > max_file_offset - request->size)
            || (request->resource && request->size
                && request->resource_offset > std::numeric_limits<UINT64>::max() - request->size)) {
            return E_INVALIDARG;
        }
        Entry entry{Kind::request, *request};
        if (request->resource) request->resource->AddRef();
        if (request->file && !request->compression) {
            const HRESULT result = request->file->GetHandle(&entry.handle);
            if (FAILED(result)) {
                if (request->resource) request->resource->Release();
                return result;
            }
        }
        const HRESULT result = enqueue(entry);
        if (FAILED(result)) {
            if (entry.handle) CloseHandle(entry.handle);
            if (request->resource) request->resource->Release();
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE EnqueueStatus(Status* status, UINT index) override {
        auto* array = dynamic_cast<StatusArray*>(status);
        if (!array) return E_INVALIDARG;

        Lock lock(mutex_);
        const HRESULT available = reserve_locked();
        if (FAILED(available)) return available;
        const HRESULT result = array->begin(index);
        if (FAILED(result)) return result;

        array->AddRef();
        Entry entry{Kind::status};
        entry.status = array;
        entry.index = index;
        append(entry);
        auto_submit_locked();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* fence, UINT64 value) override {
        if (!fence) return E_POINTER;
        Entry entry{Kind::signal};
        entry.fence = fence;
        entry.value = value;
        fence->AddRef();
        const HRESULT result = enqueue(entry);
        if (FAILED(result)) fence->Release();
        return result;
    }

    HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE event) override {
        if (!event || event == INVALID_HANDLE_VALUE) return E_INVALIDARG;
        Entry entry{Kind::event};
        const HRESULT duplicated = duplicate_handle(event, &entry.handle);
        if (FAILED(duplicated)) return duplicated;
        const HRESULT result = enqueue(entry);
        if (FAILED(result)) CloseHandle(entry.handle);
        return result;
    }

    HRESULT STDMETHODCALLTYPE Submit() override {
        Lock lock(mutex_);
        if (closed_) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        submit();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RetrieveErrorRecord(ErrorRecord* out) override {
        if (!out) return E_POINTER;
        Lock lock(mutex_);
        *out = errors_;
        errors_ = {};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Query(QueueInfo* info) override {
        if (!info) return E_POINTER;
        Lock lock(mutex_);
        *info = {capacity_, capacity_ - size_};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Close() override {
        {
            Lock lock(mutex_);
            closed_ = true;
            submit();
            WakeAllConditionVariable(&space_);
        }
        if (work_) WaitForThreadpoolWorkCallbacks(work_, FALSE);
        return S_OK;
    }

    // Records a command rejected at enqueue time, in queue order. A rejected
    // request counts toward the next status entry, as a failed request does.
    HRESULT enqueue_failure(HRESULT failure, Kind failed_kind) {
        Entry entry{Kind::failure};
        entry.failure = failure;
        entry.failed_kind = failed_kind;
        return enqueue(entry);
    }

    bool closed() {
        Lock lock(mutex_);
        return closed_;
    }

    // Moves the error record out, with the kind of the first failed command.
    void take_errors(ErrorRecord* record, Kind* first_kind) {
        Lock lock(mutex_);
        *record = errors_;
        *first_kind = first_failed_kind_;
        errors_ = {};
        if (error_event_) ResetEvent(error_event_);
    }

    // Lazily created manual-reset event, set on every failure. Owned by the queue.
    HANDLE error_event() {
        Lock lock(mutex_);
        if (!error_event_) {
            error_event_ = CreateEventW(nullptr, TRUE, errors_.failure_count ? TRUE : FALSE, nullptr);
        }
        return error_event_;
    }

    void counts(UINT* empty_slots, UINT* until_auto_submit) {
        Lock lock(mutex_);
        *empty_slots = capacity_ - size_;
        const UINT pending = size_ - submitted_;
        *until_auto_submit = auto_submit_ > pending ? auto_submit_ - pending : 0;
    }

private:
    ~WorkQueue() override {
        Close();
        if (work_) CloseThreadpoolWork(work_);
        if (error_event_) CloseHandle(error_event_);
        delete[] entries_;
    }

    // Called with mutex_ held. A blocking queue waits here for the drain to free
    // a slot; Close wakes the waiters.
    HRESULT reserve_locked() {
        for (;;) {
            if (closed_) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
            if (size_ < capacity_) return S_OK;
            if (!blocking_) return HRESULT_FROM_WIN32(ERROR_BUSY);
            if (submitted_ < size_) submit();
            SleepConditionVariableSRW(&space_, &mutex_, INFINITE, 0);
        }
    }

    void auto_submit_locked() {
        if (auto_submit_ && size_ - submitted_ >= auto_submit_) submit();
    }

    void append(const Entry& entry) {
        entries_[(head_ + size_) % capacity_] = entry;
        ++size_;
    }

    HRESULT enqueue(const Entry& entry) {
        Lock lock(mutex_);
        const HRESULT result = reserve_locked();
        if (FAILED(result)) return result;
        append(entry);
        auto_submit_locked();
        return S_OK;
    }

    void submit() {
        submitted_ = size_;
        if (submitted_ && !active_) {
            active_ = true;
            SubmitThreadpoolWork(work_);
        }
    }

    static void CALLBACK run(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) {
        static_cast<WorkQueue*>(context)->drain();
    }

    void drain() {
        for (;;) {
            Entry entry{};
            {
                Lock lock(mutex_);
                if (!submitted_) {
                    active_ = false;
                    return;
                }
                entry = entries_[head_];
            }
            HRESULT result = S_OK;
            switch (entry.kind) {
                case Kind::request: {
                    const CopyResult outcome = fulfill(entry.handle, entry.request);
                    result = outcome.hr;
                    if (entry.handle) CloseHandle(entry.handle);
                    // keep_destination holds the AddRef taken at enqueue. Releasing
                    // it while the staged copy is still on the GPU frees the dest.
                    if (entry.request.resource && !outcome.keep_destination) entry.request.resource->Release();
                    break;
                }
                case Kind::status:
                    entry.status->complete(entry.index, status_error_);
                    entry.status->Release();
                    status_error_ = S_OK;
                    break;
                case Kind::signal:
                    result = entry.fence->Signal(entry.value);
                    entry.fence->Release();
                    break;
                case Kind::event:
                    if (!SetEvent(entry.handle)) result = win32_error();
                    CloseHandle(entry.handle);
                    break;
                case Kind::failure:
                    result = entry.failure;
                    break;
            }
            const Kind command = entry.kind == Kind::failure ? entry.failed_kind : entry.kind;
            Lock lock(mutex_);
            if (FAILED(result)) {
                // A status, signal or event rejected at enqueue time is reported
                // through the error record alone; everything else also feeds the
                // next status entry, as before.
                const bool feeds_status = entry.kind != Kind::failure || command == Kind::request;
                if (feeds_status && SUCCEEDED(status_error_)) status_error_ = result;
                if (!errors_.failure_count) {
                    errors_.first_error = result;
                    first_failed_kind_ = command;
                }
                ++errors_.failure_count;
                if (error_event_) SetEvent(error_event_);
            }
            head_ = (head_ + 1) % capacity_;
            --size_;
            --submitted_;
            WakeAllConditionVariable(&space_);
        }
    }

    SRWLOCK mutex_ = SRWLOCK_INIT;
    CONDITION_VARIABLE space_ = CONDITION_VARIABLE_INIT;
    Entry* entries_;
    UINT capacity_;
    bool blocking_;
    UINT auto_submit_;
    UINT head_ = 0;
    UINT size_ = 0;
    UINT submitted_ = 0;
    bool active_ = false;
    bool closed_ = false;
    PTP_WORK work_ = nullptr;
    HRESULT status_error_ = S_OK;
    ErrorRecord errors_{};
    Kind first_failed_kind_ = Kind::request;
    HANDLE error_event_ = nullptr;
};

class StorageFactory final : public Object<Factory, factory_iid> {
public:
    HRESULT STDMETHODCALLTYPE OpenFile(PCWSTR path, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!path || !*path) return E_INVALIDARG;
        HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return win32_error();
        auto* file = new (std::nothrow) StorageFile(handle);
        if (!file) {
            CloseHandle(handle);
            return E_OUTOFMEMORY;
        }
        return publish(file, iid, out);
    }

    HRESULT STDMETHODCALLTYPE CreateQueue(UINT capacity, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!capacity || capacity > 65535) return E_INVALIDARG;
        auto* entries = new (std::nothrow) Entry[capacity];
        if (!entries) return E_OUTOFMEMORY;
        auto* queue = new (std::nothrow) WorkQueue(entries, capacity);
        if (!queue) {
            delete[] entries;
            return E_OUTOFMEMORY;
        }
        if (!queue->create_work()) {
            const HRESULT error = win32_error();
            queue->Release();
            return error;
        }
        return publish(queue, iid, out);
    }

    HRESULT STDMETHODCALLTYPE CreateStatusArray(UINT count, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!count || count > 65535) return E_INVALIDARG;
        auto* slots = new (std::nothrow) StatusArray::Slot[count];
        if (!slots) return E_OUTOFMEMORY;
        auto* array = new (std::nothrow) StatusArray(slots, count);
        if (!array) {
            delete[] slots;
            return E_OUTOFMEMORY;
        }
        return publish(array, iid, out);
    }

    HRESULT STDMETHODCALLTYPE SetStagingBufferSize(UINT size) override {
        if (!size) return S_OK;
        BX_STUB_LOG("XCRDAPI.dll", "SetStagingBufferSize");
        return E_NOTIMPL;
    }
};

// ---------------------------------------------------------------------------
// Public PC DirectStorage interfaces (dstorage.h, SDK 1.4.0) over the queue
// engine above. Behaviour follows the header's documentation; nothing here was
// compared against Microsoft's runtime. Xbox IDStorage*X interfaces are out of
// scope (#483): their IIDs have no licence-compatible public source.
// ---------------------------------------------------------------------------

// Private to this DLL: recovers the BetterXen object behind a public interface
// pointer, so a foreign IDStorageFile or IDStorageStatusArray is never cast.
constexpr IID kPublicImplIid{0x3c0f6a52, 0x8d1e, 0x4b7a, {0x9e, 0x21, 0x5f, 0x44, 0xc3, 0x0b, 0x7d, 0x18}};

void format_iid(REFIID iid, char (&out)[40]) {
    std::snprintf(out, sizeof(out), "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                  static_cast<unsigned long>(iid.Data1), iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1],
                  iid.Data4[2], iid.Data4[3], iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
}

// Reference counting and QueryInterface for one public interface chain.
template<class Interface>
class PublicObject : public Interface {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!IsEqualIID(riid, IID_IUnknown) && !IsEqualIID(riid, kPublicImplIid) && !supports(riid)) {
            return E_NOINTERFACE;
        }
        *result = static_cast<Interface*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = InterlockedDecrement(&references_);
        if (!references) delete this;
        return references;
    }

protected:
    virtual ~PublicObject() = default;
    virtual bool supports(REFIID riid) const = 0;

private:
    LONG references_ = 1;
};

// Returns the BetterXen implementation behind `object`, or nullptr. No reference is kept.
template<class Impl, class Interface>
Impl* public_impl(Interface* object) {
    if (!object) return nullptr;
    void* raw = nullptr;
    if (object->QueryInterface(kPublicImplIid, &raw) != S_OK || !raw) return nullptr;
    object->Release();
    return static_cast<Impl*>(static_cast<Interface*>(raw));
}

class PublicFile final : public PublicObject<IDStorageFile> {
public:
    explicit PublicFile(StorageFile* file) : file_(file) {}

    void STDMETHODCALLTYPE Close() override {
        const HRESULT hr = file_->Close();
        if (FAILED(hr)) {
            char detail[48];
            std::snprintf(detail, sizeof(detail), "hr=0x%08lx", static_cast<unsigned long>(hr));
            bx_log_emit(BX_LOG_WARN, "xcrdapi", "dstorage_file_close_failed", detail);
        }
    }

    HRESULT STDMETHODCALLTYPE GetFileInformation(BY_HANDLE_FILE_INFORMATION* info) override {
        return file_->GetFileInformation(info);
    }

    File* inner() const { return file_; }

private:
    ~PublicFile() override { file_->Release(); }
    bool supports(REFIID riid) const override { return IsEqualIID(riid, __uuidof(IDStorageFile)); }

    StorageFile* file_;
};

class PublicStatusArray final : public PublicObject<IDStorageStatusArray> {
public:
    explicit PublicStatusArray(StatusArray* array) : array_(array) {}

    // The header defines IsComplete as GetHResult(index) != E_PENDING.
    bool STDMETHODCALLTYPE IsComplete(UINT32 index) override { return array_->GetResult(index) != E_PENDING; }
    HRESULT STDMETHODCALLTYPE GetHResult(UINT32 index) override { return array_->GetResult(index); }

    StatusArray* inner() const { return array_; }

private:
    ~PublicStatusArray() override { array_->Release(); }
    bool supports(REFIID riid) const override { return IsEqualIID(riid, __uuidof(IDStorageStatusArray)); }

    StatusArray* array_;
};

// IDStorageQueue4 vtable slots: IUnknown 0..2, then the methods in header order.
enum PublicQueueSlot : UINT {
    kSlotEnqueueRequest = 3,
    kSlotCancelRequestsWithTag = 7,
    kSlotEnqueueRequests = 14,
};

const char* destination_name(UINT type) {
    switch (type) {
        case DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION: return "destination_texture_region";
        case DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES: return "destination_multiple_subresources";
        case DSTORAGE_REQUEST_DESTINATION_TILES: return "destination_tiles";
        case DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE: return "destination_multiple_subresources_range";
        default: return "destination_unknown";
    }
}

bool reserved_fields_clear(const DSTORAGE_REQUEST_OPTIONS& options) {
    for (const UINT8 byte : options.Reserved1) {
        if (byte) return false;
    }
    return options.Reserved == 0;
}

// The public queue. DSTORAGE_QUEUE_DESC capacity N becomes N - 1 usable slots
// (the header: an empty queue reports capacity - 1 empty slots), and requests
// are submitted automatically once N / 2 are waiting (the header: "until the
// queue is half full").
// Maps the engine's command kind onto the header's DSTORAGE_COMMAND_TYPE for
// RetrieveErrorRecord on the public PC queue.
DSTORAGE_COMMAND_TYPE command_type(Kind kind) {
    switch (kind) {
        case Kind::status: return DSTORAGE_COMMAND_TYPE_STATUS;
        case Kind::signal: return DSTORAGE_COMMAND_TYPE_SIGNAL;
        case Kind::event: return DSTORAGE_COMMAND_TYPE_EVENT;
        default: return DSTORAGE_COMMAND_TYPE_REQUEST;
    }
}

class PublicQueue final : public PublicObject<IDStorageQueue4> {
public:
    PublicQueue(WorkQueue* queue, const DSTORAGE_QUEUE_DESC1& desc) : queue_(queue), desc_(desc) {
        if (desc.Name) {
            std::snprintf(name_, sizeof(name_), "%s", desc.Name);
            desc_.Name = name_;
        }
        if (desc_.Device) desc_.Device->AddRef();
    }

    void STDMETHODCALLTYPE EnqueueRequest(const DSTORAGE_REQUEST* request) override {
        Request translated{};
        const HRESULT hr = translate(request, &translated);
        if (FAILED(hr)) {
            reject(hr, Kind::request, "EnqueueRequest");
            return;
        }
        accept(queue_->EnqueueRequest(&translated), Kind::request, "EnqueueRequest");
    }

    void STDMETHODCALLTYPE EnqueueStatus(IDStorageStatusArray* status_array, UINT32 index) override {
        auto* array = public_impl<PublicStatusArray>(status_array);
        if (!array) {
            reject(E_DSTORAGE_INVALID_STATUS_ARRAY, Kind::status, "EnqueueStatus");
            return;
        }
        HRESULT hr = queue_->EnqueueStatus(array->inner(), index);
        if (hr == E_INVALIDARG) hr = E_DSTORAGE_INDEX_BOUND;
        accept(hr, Kind::status, "EnqueueStatus");
    }

    void STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* fence, UINT64 value) override {
        if (!fence) {
            reject(E_DSTORAGE_INVALID_FENCE, Kind::signal, "EnqueueSignal");
            return;
        }
        accept(queue_->EnqueueSignal(fence, value), Kind::signal, "EnqueueSignal");
    }

    void STDMETHODCALLTYPE Submit() override {
        if (FAILED(queue_->Submit())) {
            bx_log_emit(BX_LOG_WARN, "xcrdapi", "dstorage_queue_closed", "Submit after Close; nothing submitted");
        }
    }

    // Allowed by the header ("might or might not complete"), but not implemented:
    // every request still runs. Logged so the trace shows it.
    void STDMETHODCALLTYPE CancelRequestsWithTag(UINT64, UINT64) override {
        BX_VTABLE_STUB_LOG("IDStorageQueue", kSlotCancelRequestsWithTag, "CancelRequestsWithTag");
    }

    void STDMETHODCALLTYPE Close() override { (void)queue_->Close(); }

    HANDLE STDMETHODCALLTYPE GetErrorEvent() override { return queue_->error_event(); }

    // FailureCount, FirstFailure.HResult and FirstFailure.CommandType are filled.
    // The per-command parameters in FirstFailure are left zeroed.
    void STDMETHODCALLTYPE RetrieveErrorRecord(DSTORAGE_ERROR_RECORD* record) override {
        if (!record) return;
        std::memset(record, 0, sizeof(*record));
        ErrorRecord errors{};
        Kind first = Kind::request;
        queue_->take_errors(&errors, &first);
        record->FailureCount = errors.failure_count > 0xffffffffull ? 0xffffffffu
                                                                    : static_cast<UINT32>(errors.failure_count);
        record->FirstFailure.HResult = errors.failure_count ? errors.first_error : S_OK;
        record->FirstFailure.CommandType = errors.failure_count ? command_type(first) : DSTORAGE_COMMAND_TYPE_NONE;
    }

    void STDMETHODCALLTYPE Query(DSTORAGE_QUEUE_INFO* info) override {
        if (!info) return;
        DSTORAGE_QUEUE_INFO1 full{};
        Query1(&full);
        info->Desc.SourceType = full.Desc.SourceType;
        info->Desc.Capacity = full.Desc.Capacity;
        info->Desc.Priority = full.Desc.Priority;
        info->Desc.Name = full.Desc.Name;
        info->Desc.Device = full.Desc.Device;
        info->EmptySlotCount = full.EmptySlotCount;
        info->RequestCountUntilAutoSubmit = full.RequestCountUntilAutoSubmit;
    }

    void STDMETHODCALLTYPE EnqueueSetEvent(HANDLE handle) override {
        if (!handle || handle == INVALID_HANDLE_VALUE) {
            reject(E_INVALIDARG, Kind::event, "EnqueueSetEvent");
            return;
        }
        accept(queue_->EnqueueSetEvent(handle), Kind::event, "EnqueueSetEvent");
    }

    // No compression format is decompressed by this queue.
    DSTORAGE_COMPRESSION_SUPPORT STDMETHODCALLTYPE GetCompressionSupport(DSTORAGE_COMPRESSION_FORMAT) override {
        return DSTORAGE_COMPRESSION_SUPPORT_NONE;
    }

    void STDMETHODCALLTYPE EnqueueRequests(const DSTORAGE_REQUEST* requests, UINT count, ID3D12Fence* fence,
                                           UINT64, DSTORAGE_ENQUEUE_REQUEST_FLAGS) override {
        if (count && !requests) {
            reject(E_INVALIDARG, Kind::request, "EnqueueRequests");
            return;
        }
        if (fence) {
            // Fence synchronization is not implemented, so none of these requests run.
            BX_VTABLE_STUB_LOG("IDStorageQueue3", kSlotEnqueueRequests, "EnqueueRequests_fence");
            for (UINT i = 0; i < count; ++i) reject(E_NOTIMPL, Kind::request, "EnqueueRequests");
            return;
        }
        for (UINT i = 0; i < count; ++i) EnqueueRequest(&requests[i]);
    }

    void STDMETHODCALLTYPE Query1(DSTORAGE_QUEUE_INFO1* info) override {
        if (!info) return;
        UINT empty = 0;
        UINT until_auto_submit = 0;
        queue_->counts(&empty, &until_auto_submit);
        info->Desc = desc_;
        info->EmptySlotCount = static_cast<UINT16>(empty);
        info->RequestCountUntilAutoSubmit = static_cast<UINT16>(until_auto_submit);
    }

private:
    ~PublicQueue() override {
        queue_->Release();
        if (desc_.Device) desc_.Device->Release();
    }

    bool supports(REFIID riid) const override {
        return IsEqualIID(riid, __uuidof(IDStorageQueue)) || IsEqualIID(riid, __uuidof(IDStorageQueue1))
            || IsEqualIID(riid, __uuidof(IDStorageQueue2)) || IsEqualIID(riid, __uuidof(IDStorageQueue3))
            || IsEqualIID(riid, __uuidof(IDStorageQueue4));
    }

    // Maps a DSTORAGE_REQUEST onto the engine's Request. Only uncompressed
    // file/memory sources into memory or buffer destinations are supported.
    HRESULT translate(const DSTORAGE_REQUEST* request, Request* out) const {
        if (!request) return E_INVALIDARG;
        const DSTORAGE_REQUEST_OPTIONS& options = request->Options;
        if (!reserved_fields_clear(options)) return E_DSTORAGE_RESERVED_FIELDS;
        if (static_cast<UINT64>(options.SourceType) != static_cast<UINT64>(desc_.SourceType)) {
            return E_DSTORAGE_INVALID_SOURCE_TYPE;
        }

        const UINT compression = static_cast<UINT>(options.CompressionFormat);
        if (compression == DSTORAGE_COMPRESSION_FORMAT_GDEFLATE) {
            out->compression = compression_gdeflate; // the engine logs it and fails with E_NOTIMPL
        } else if (compression != DSTORAGE_COMPRESSION_FORMAT_NONE) {
            BX_VTABLE_STUB_LOG("IDStorageQueue", kSlotEnqueueRequest,
                               compression == DSTORAGE_COMPRESSION_FORMAT_ZSTD ? "compression_zstd"
                                                                               : "compression_other");
            return E_NOTIMPL;
        }
        if (options.GaclTransformType != DSTORAGE_GACL_SHUFFLE_TRANSFORM_NONE) {
            BX_VTABLE_STUB_LOG("IDStorageQueue", kSlotEnqueueRequest, "gacl_shuffle_transform");
            return E_NOTIMPL;
        }

        UINT32 source_size = 0;
        if (options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE) {
            auto* file = public_impl<PublicFile>(request->Source.File.Source);
            if (!file) return E_DSTORAGE_FILE_NOT_OPEN;
            out->file = file->inner();
            out->file_offset = request->Source.File.Offset;
            source_size = request->Source.File.Size;
        } else {
            out->source = request->Source.Memory.Source;
            source_size = request->Source.Memory.Size;
        }

        const UINT destination = static_cast<UINT>(options.DestinationType);
        UINT32 destination_size = 0;
        if (destination == DSTORAGE_REQUEST_DESTINATION_MEMORY) {
            out->destination = request->Destination.Memory.Buffer;
            destination_size = request->Destination.Memory.Size;
        } else if (destination == DSTORAGE_REQUEST_DESTINATION_BUFFER) {
            // The header: a queue without a device accepts memory destinations only.
            if (!desc_.Device) return E_DSTORAGE_INVALID_DESTINATION_TYPE;
            out->resource = request->Destination.Buffer.Resource;
            out->resource_offset = request->Destination.Buffer.Offset;
            destination_size = request->Destination.Buffer.Size;
        } else {
            BX_VTABLE_STUB_LOG("IDStorageQueue", kSlotEnqueueRequest, destination_name(destination));
            return E_NOTIMPL;
        }

        if (!out->compression) {
            if (destination_size != source_size) return E_DSTORAGE_INVALID_DESTINATION_SIZE;
            if (request->UncompressedSize && request->UncompressedSize != source_size) {
                return E_DSTORAGE_INVALID_DESTINATION_SIZE;
            }
        }
        out->size = source_size;
        return S_OK;
    }

    void reject(HRESULT hr, Kind kind, const char* method) {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "%s hr=0x%08lx", method, static_cast<unsigned long>(hr));
        bx_log_emit(BX_LOG_WARN, "xcrdapi", "dstorage_enqueue_rejected", detail);
        if (FAILED(queue_->enqueue_failure(hr, kind))) {
            bx_log_emit(BX_LOG_WARN, "xcrdapi", "dstorage_queue_closed", "enqueue after Close; command dropped");
        }
    }

    // Engine-level rejections of an accepted command are reported the same way.
    void accept(HRESULT hr, Kind kind, const char* method) {
        if (SUCCEEDED(hr)) return;
        if (queue_->closed()) {
            bx_log_emit(BX_LOG_WARN, "xcrdapi", "dstorage_queue_closed", "enqueue after Close; command dropped");
            return;
        }
        reject(hr, kind, method);
    }

    WorkQueue* queue_;
    DSTORAGE_QUEUE_DESC1 desc_;
    char name_[DSTORAGE_REQUEST_MAX_NAME] = {};
};

// IDStorageFactory1 vtable slots: IUnknown 0..2, then the methods in header order.
enum PublicFactorySlot : UINT {
    kSlotSetDebugFlags = 6,
    kSlotCreateQueue1 = 8,
};

class PublicFactory final : public PublicObject<IDStorageFactory1> {
public:
    HRESULT STDMETHODCALLTYPE CreateQueue(const DSTORAGE_QUEUE_DESC* desc, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!desc) return E_INVALIDARG;
        DSTORAGE_QUEUE_DESC1 full{};
        full.SourceType = desc->SourceType;
        full.Capacity = desc->Capacity;
        full.Priority = desc->Priority;
        full.Name = desc->Name;
        full.Device = desc->Device;
        full.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        return create_queue(full, riid, ppv);
    }

    HRESULT STDMETHODCALLTYPE OpenFile(const WCHAR* path, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!path || !*path) return E_INVALIDARG;
        HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return win32_error();
        auto* file = new (std::nothrow) StorageFile(handle);
        if (!file) {
            CloseHandle(handle);
            return E_OUTOFMEMORY;
        }
        auto* wrapper = new (std::nothrow) PublicFile(file);
        if (!wrapper) {
            file->Release();
            return E_OUTOFMEMORY;
        }
        return publish(wrapper, riid, ppv);
    }

    HRESULT STDMETHODCALLTYPE CreateStatusArray(UINT32 capacity, PCSTR, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!capacity || capacity > 65535) return E_INVALIDARG;
        auto* slots = new (std::nothrow) StatusArray::Slot[capacity];
        if (!slots) return E_OUTOFMEMORY;
        auto* array = new (std::nothrow) StatusArray(slots, capacity);
        if (!array) {
            delete[] slots;
            return E_OUTOFMEMORY;
        }
        auto* wrapper = new (std::nothrow) PublicStatusArray(array);
        if (!wrapper) {
            array->Release();
            return E_OUTOFMEMORY;
        }
        return publish(wrapper, riid, ppv);
    }

    // The debug layer is not implemented.
    void STDMETHODCALLTYPE SetDebugFlags(UINT32 flags) override {
        if (flags) BX_VTABLE_STUB_LOG("IDStorageFactory", kSlotSetDebugFlags, "SetDebugFlags");
    }

    // Staged copies allocate an upload buffer per request, so there is no
    // shared staging buffer to resize. Accepted and logged.
    HRESULT STDMETHODCALLTYPE SetStagingBufferSize(UINT32 size) override {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "size=%lu has no effect", static_cast<unsigned long>(size));
        bx_log_emit(BX_LOG_INFO, "xcrdapi", "dstorage_staging_buffer_size", detail);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateQueue1(const DSTORAGE_QUEUE_DESC1* desc, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!desc) return E_INVALIDARG;
        return create_queue(*desc, riid, ppv);
    }

private:
    bool supports(REFIID riid) const override {
        return IsEqualIID(riid, __uuidof(IDStorageFactory)) || IsEqualIID(riid, __uuidof(IDStorageFactory1));
    }

    static HRESULT create_queue(const DSTORAGE_QUEUE_DESC1& desc, REFIID riid, void** ppv) {
        if (desc.Capacity < DSTORAGE_MIN_QUEUE_CAPACITY || desc.Capacity > DSTORAGE_MAX_QUEUE_CAPACITY) {
            return E_DSTORAGE_INVALID_QUEUE_CAPACITY;
        }
        const int priority = static_cast<int>(desc.Priority);
        if (priority < DSTORAGE_PRIORITY_FIRST || priority > DSTORAGE_PRIORITY_LAST) {
            return E_DSTORAGE_INVALID_QUEUE_PRIORITY;
        }
        const UINT64 source = static_cast<UINT64>(desc.SourceType);
        if (source != DSTORAGE_REQUEST_SOURCE_FILE && source != DSTORAGE_REQUEST_SOURCE_MEMORY) {
            return E_DSTORAGE_INVALID_SOURCE_TYPE;
        }
        if (desc.UsesGaclShuffleTransform) {
            BX_VTABLE_STUB_LOG("IDStorageFactory1", kSlotCreateQueue1, "UsesGaclShuffleTransform");
            return E_NOTIMPL;
        }
        const UINT slots = desc.Capacity - 1u;
        auto* entries = new (std::nothrow) Entry[slots];
        if (!entries) return E_OUTOFMEMORY;
        auto* queue = new (std::nothrow) WorkQueue(entries, slots, true, desc.Capacity / 2u);
        if (!queue) {
            delete[] entries;
            return E_OUTOFMEMORY;
        }
        if (!queue->create_work()) {
            const HRESULT error = win32_error();
            queue->Release();
            return error;
        }
        auto* wrapper = new (std::nothrow) PublicQueue(queue, desc);
        if (!wrapper) {
            queue->Release();
            return E_OUTOFMEMORY;
        }
        return publish(wrapper, riid, ppv);
    }
};

// ---------------------------------------------------------------------------
// Xbox Series DirectStorage COM interfaces (IDStorage*X), inferred from call-site
// observation of a project-authorized target build (provenance pending maintainer
// review; not a public contract), layered over the same WorkQueue / StorageFile /
// StatusArray engine. Unimplemented vtable slots log through BX_VTABLE_STUB_LOG
// so any newly reached slot surfaces immediately in bx-cli trace.
// ---------------------------------------------------------------------------

constexpr IID kXboxFactoryIid{0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr IID kXboxFileIid{0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};
constexpr IID kXboxStatusArrayIid{0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
constexpr IID kXboxQueueIid{0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};

struct XboxQueueDesc {
    UINT64 source_type;
    UINT16 capacity;
    INT8 priority;
    UINT8 reserved_pad[5];
    PCSTR name;
    UINT64 flags;
};
static_assert(sizeof(XboxQueueDesc) == 32, "XboxQueueDesc must be 32 bytes");
static_assert(offsetof(XboxQueueDesc, source_type) == 0x00, "source_type offset");
static_assert(offsetof(XboxQueueDesc, capacity) == 0x08, "capacity offset");
static_assert(offsetof(XboxQueueDesc, priority) == 0x0a, "priority offset");
static_assert(offsetof(XboxQueueDesc, name) == 0x10, "name offset");
static_assert(offsetof(XboxQueueDesc, flags) == 0x18, "flags offset");

struct XboxRequest {
    UINT64 options;          // +0x00: bit 22 (0x400000) = memory source, bit 0 = compressed
    void* destination;       // +0x08
    UINT64 destination_pad;  // +0x10
    UINT32 destination_size; // +0x18
    UINT32 reserved1c;       // +0x1c
    void* source_ptr;        // +0x20: IXboxDStorageFile* (file) or const void* (memory)
    UINT64 file_offset;      // +0x28
    UINT32 source_size;      // +0x30
    UINT32 reserved34;       // +0x34
};
static_assert(sizeof(XboxRequest) == 56, "XboxRequest must be 56 bytes");
static_assert(offsetof(XboxRequest, options) == 0x00, "options offset");
static_assert(offsetof(XboxRequest, destination) == 0x08, "destination offset");
static_assert(offsetof(XboxRequest, destination_size) == 0x18, "destination_size offset");
static_assert(offsetof(XboxRequest, source_ptr) == 0x20, "source_ptr offset");
static_assert(offsetof(XboxRequest, file_offset) == 0x28, "file_offset offset");
static_assert(offsetof(XboxRequest, source_size) == 0x30, "source_size offset");

constexpr UINT64 kXboxRequestSourceMemoryBit = 0x400000ull;
constexpr UINT64 kXboxRequestCompressedBit = 0x1ull;

struct IXboxDStorageFile : IUnknown {
    virtual void STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFileInformation(BY_HANDLE_FILE_INFORMATION* info) = 0;
    virtual HRESULT STDMETHODCALLTYPE StubSlot5() = 0;
    virtual HRESULT STDMETHODCALLTYPE StubSlot6() = 0;
};

struct IXboxDStorageStatusArray : IUnknown {
    virtual bool STDMETHODCALLTYPE IsComplete(UINT32 index) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetHResult(UINT32 index) = 0;
    virtual HRESULT STDMETHODCALLTYPE StubSlot5() = 0;
    virtual HRESULT STDMETHODCALLTYPE StubSlot6() = 0;
};

struct IXboxDStorageQueue : IUnknown {
    virtual void STDMETHODCALLTYPE EnqueueRequest(const XboxRequest* request) = 0;
    virtual void STDMETHODCALLTYPE EnqueueStatus(IXboxDStorageStatusArray* status_array, UINT32 index) = 0;
    virtual void STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* fence, UINT64 value) = 0;
    virtual void STDMETHODCALLTYPE Submit() = 0;
    virtual void STDMETHODCALLTYPE CancelRequestsWithTag(UINT64 mask, UINT64 value) = 0;
    virtual void STDMETHODCALLTYPE Close() = 0;
    virtual HANDLE STDMETHODCALLTYPE GetErrorEvent() = 0;
    virtual void STDMETHODCALLTYPE RetrieveErrorRecord(void* record) = 0;
    virtual void STDMETHODCALLTYPE Query(void* info) = 0;
    virtual HRESULT STDMETHODCALLTYPE StubSlot12() = 0;
    virtual HRESULT STDMETHODCALLTYPE StubSlot13() = 0;
    virtual HRESULT STDMETHODCALLTYPE StubSlot14() = 0;
    virtual HRESULT STDMETHODCALLTYPE StubSlot15() = 0;
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
    virtual HRESULT STDMETHODCALLTYPE StubSlot11() = 0;
    virtual HRESULT STDMETHODCALLTYPE StubSlot12() = 0;
};

class XboxFile final : public PublicObject<IXboxDStorageFile> {
public:
    explicit XboxFile(StorageFile* file) : file_(file) {}

    void STDMETHODCALLTYPE Close() override { (void)file_->Close(); }
    HRESULT STDMETHODCALLTYPE GetFileInformation(BY_HANDLE_FILE_INFORMATION* info) override {
        return file_->GetFileInformation(info);
    }
    HRESULT STDMETHODCALLTYPE StubSlot5() override {
        BX_VTABLE_STUB_LOG("IDStorageFileX", 5u, "Slot5");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE StubSlot6() override {
        BX_VTABLE_STUB_LOG("IDStorageFileX", 6u, "Slot6");
        return E_NOTIMPL;
    }

    File* inner() const { return file_; }

private:
    ~XboxFile() override { file_->Release(); }
    bool supports(REFIID riid) const override { return IsEqualIID(riid, kXboxFileIid); }

    StorageFile* file_;
};

class XboxStatusArray final : public PublicObject<IXboxDStorageStatusArray> {
public:
    explicit XboxStatusArray(StatusArray* array) : array_(array) {}

    bool STDMETHODCALLTYPE IsComplete(UINT32 index) override { return array_->GetResult(index) != E_PENDING; }
    HRESULT STDMETHODCALLTYPE GetHResult(UINT32 index) override { return array_->GetResult(index); }
    HRESULT STDMETHODCALLTYPE StubSlot5() override {
        BX_VTABLE_STUB_LOG("IDStorageStatusArrayX", 5u, "Slot5");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE StubSlot6() override {
        BX_VTABLE_STUB_LOG("IDStorageStatusArrayX", 6u, "Slot6");
        return E_NOTIMPL;
    }

    StatusArray* inner() const { return array_; }

private:
    ~XboxStatusArray() override { array_->Release(); }
    bool supports(REFIID riid) const override { return IsEqualIID(riid, kXboxStatusArrayIid); }

    StatusArray* array_;
};

class XboxQueue final : public PublicObject<IXboxDStorageQueue> {
public:
    XboxQueue(WorkQueue* queue, UINT64 source_type) : queue_(queue), source_type_(source_type) {}

    void STDMETHODCALLTYPE EnqueueRequest(const XboxRequest* request) override {
        if (!request) {
            reject(E_INVALIDARG, Kind::request, "EnqueueRequest");
            return;
        }
        const bool is_memory = (request->options & kXboxRequestSourceMemoryBit) != 0;
        if ((is_memory && source_type_ == 0) || (!is_memory && source_type_ != 0)) {
            reject(E_DSTORAGE_INVALID_SOURCE_TYPE, Kind::request, "EnqueueRequest");
            return;
        }
        if ((request->options & kXboxRequestCompressedBit) != 0) {
            BX_VTABLE_STUB_LOG("IDStorageQueueX", 3u, "EnqueueRequest_compressed");
            reject(E_NOTIMPL, Kind::request, "EnqueueRequest");
            return;
        }
        if (!request->destination || request->source_size != request->destination_size) {
            reject(E_DSTORAGE_INVALID_DESTINATION_SIZE, Kind::request, "EnqueueRequest");
            return;
        }

        Request translated{};
        translated.destination = request->destination;
        translated.size = request->source_size;
        if (is_memory) {
            if (!request->source_ptr) {
                reject(E_INVALIDARG, Kind::request, "EnqueueRequest");
                return;
            }
            translated.source = request->source_ptr;
        } else {
            auto* file = public_impl<XboxFile>(static_cast<IXboxDStorageFile*>(request->source_ptr));
            if (!file) {
                reject(E_DSTORAGE_FILE_NOT_OPEN, Kind::request, "EnqueueRequest");
                return;
            }
            translated.file = file->inner();
            translated.file_offset = request->file_offset;
        }
        accept(queue_->EnqueueRequest(&translated), Kind::request, "EnqueueRequest");
    }

    void STDMETHODCALLTYPE EnqueueStatus(IXboxDStorageStatusArray* status_array, UINT32 index) override {
        auto* array = public_impl<XboxStatusArray>(status_array);
        if (!array) {
            reject(E_DSTORAGE_INVALID_STATUS_ARRAY, Kind::status, "EnqueueStatus");
            return;
        }
        HRESULT hr = queue_->EnqueueStatus(array->inner(), index);
        if (hr == E_INVALIDARG) hr = E_DSTORAGE_INDEX_BOUND;
        accept(hr, Kind::status, "EnqueueStatus");
    }

    void STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* fence, UINT64 value) override {
        if (!fence) {
            reject(E_DSTORAGE_INVALID_FENCE, Kind::signal, "EnqueueSignal");
            return;
        }
        accept(queue_->EnqueueSignal(fence, value), Kind::signal, "EnqueueSignal");
    }

    void STDMETHODCALLTYPE Submit() override {
        if (FAILED(queue_->Submit())) {
            bx_log_emit(BX_LOG_WARN, "xcrdapi", "dstorage_queue_closed", "Submit after Close; nothing submitted");
        }
    }

    void STDMETHODCALLTYPE CancelRequestsWithTag(UINT64, UINT64) override {
        BX_VTABLE_STUB_LOG("IDStorageQueueX", 7u, "CancelRequestsWithTag");
    }

    void STDMETHODCALLTYPE Close() override { (void)queue_->Close(); }

    HANDLE STDMETHODCALLTYPE GetErrorEvent() override { return queue_->error_event(); }

    void STDMETHODCALLTYPE RetrieveErrorRecord(void*) override {
        // The size and layout of the Xbox caller's output are unverified.
        // Do not write the public PC record into it.
        BX_VTABLE_STUB_LOG("IDStorageQueueX", 10u, "RetrieveErrorRecord");
    }

    void STDMETHODCALLTYPE Query(void*) override { BX_VTABLE_STUB_LOG("IDStorageQueueX", 11u, "Query"); }
    HRESULT STDMETHODCALLTYPE StubSlot12() override {
        BX_VTABLE_STUB_LOG("IDStorageQueueX", 12u, "Slot12");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE StubSlot13() override {
        BX_VTABLE_STUB_LOG("IDStorageQueueX", 13u, "Slot13");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE StubSlot14() override {
        BX_VTABLE_STUB_LOG("IDStorageQueueX", 14u, "Slot14");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE StubSlot15() override {
        BX_VTABLE_STUB_LOG("IDStorageQueueX", 15u, "Slot15");
        return E_NOTIMPL;
    }

private:
    ~XboxQueue() override { queue_->Release(); }
    bool supports(REFIID riid) const override { return IsEqualIID(riid, kXboxQueueIid); }

    void reject(HRESULT hr, Kind kind, const char* method) {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "xbox %s hr=0x%08lx", method, static_cast<unsigned long>(hr));
        bx_log_emit(BX_LOG_WARN, "xcrdapi", "dstorage_enqueue_rejected", detail);
        if (FAILED(queue_->enqueue_failure(hr, kind))) {
            bx_log_emit(BX_LOG_WARN, "xcrdapi", "dstorage_queue_closed", "enqueue after Close; command dropped");
        }
    }

    void accept(HRESULT hr, Kind kind, const char* method) {
        if (SUCCEEDED(hr)) return;
        if (queue_->closed()) {
            bx_log_emit(BX_LOG_WARN, "xcrdapi", "dstorage_queue_closed", "enqueue after Close; command dropped");
            return;
        }
        reject(hr, kind, method);
    }

    WorkQueue* queue_;
    UINT64 source_type_;
};

class XboxFactory final : public PublicObject<IXboxDStorageFactory> {
public:
    HRESULT STDMETHODCALLTYPE CreateQueue(const void*, REFIID, void** ppv) override {
        if (ppv) *ppv = nullptr;
        BX_VTABLE_STUB_LOG("IDStorageFactoryX", 3u, "CreateQueue");
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE OpenFile(const WCHAR* path, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!path || !*path) return E_INVALIDARG;
        HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return win32_error();
        auto* file = new (std::nothrow) StorageFile(handle);
        if (!file) {
            CloseHandle(handle);
            return E_OUTOFMEMORY;
        }
        auto* wrapper = new (std::nothrow) XboxFile(file);
        if (!wrapper) {
            file->Release();
            return E_OUTOFMEMORY;
        }
        return publish(wrapper, riid, ppv);
    }

    HRESULT STDMETHODCALLTYPE CreateStatusArray(UINT32 capacity, PCSTR, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!capacity || capacity > 65535) return E_INVALIDARG;
        auto* slots = new (std::nothrow) StatusArray::Slot[capacity];
        if (!slots) return E_OUTOFMEMORY;
        auto* array = new (std::nothrow) StatusArray(slots, capacity);
        if (!array) {
            delete[] slots;
            return E_OUTOFMEMORY;
        }
        auto* wrapper = new (std::nothrow) XboxStatusArray(array);
        if (!wrapper) {
            array->Release();
            return E_OUTOFMEMORY;
        }
        return publish(wrapper, riid, ppv);
    }

    void STDMETHODCALLTYPE SetDebugFlags(UINT32 flags) override {
        if (flags) BX_VTABLE_STUB_LOG("IDStorageFactoryX", 6u, "SetDebugFlags");
    }

    HRESULT STDMETHODCALLTYPE StubSlot7() override {
        BX_VTABLE_STUB_LOG("IDStorageFactoryX", 7u, "Slot7");
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetStagingBufferSize(UINT32 size) override {
        char detail[80];
        /* Like the public factory: the engine has no staging buffer to size. */
        std::snprintf(detail, sizeof(detail), "xbox size=%lu has no effect", static_cast<unsigned long>(size));
        bx_log_emit(BX_LOG_INFO, "xcrdapi", "dstorage_staging_buffer_size", detail);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StubSlot9() override {
        BX_VTABLE_STUB_LOG("IDStorageFactoryX", 9u, "Slot9");
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE CreateQueueX(const XboxQueueDesc* desc, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!desc) return E_INVALIDARG;
        if (desc->capacity < 2 || desc->capacity > DSTORAGE_MAX_QUEUE_CAPACITY) {
            return E_DSTORAGE_INVALID_QUEUE_CAPACITY;
        }
        if (desc->source_type > 1) {
            return E_DSTORAGE_INVALID_SOURCE_TYPE;
        }
        /* desc->priority is ignored on purpose: the Xbox priority range is not part of
         * the inferred contract, so no value is rejected (the public queue's
         * E_DSTORAGE_INVALID_QUEUE_PRIORITY range check is not assumed to apply). */
        const UINT slots = desc->capacity - 1u;
        auto* entries = new (std::nothrow) Entry[slots];
        if (!entries) return E_OUTOFMEMORY;
        auto* queue = new (std::nothrow) WorkQueue(entries, slots, true, desc->capacity / 2u);
        if (!queue) {
            delete[] entries;
            return E_OUTOFMEMORY;
        }
        if (!queue->create_work()) {
            const HRESULT error = win32_error();
            queue->Release();
            return error;
        }
        auto* wrapper = new (std::nothrow) XboxQueue(queue, desc->source_type);
        if (!wrapper) {
            queue->Release();
            return E_OUTOFMEMORY;
        }
        return publish(wrapper, riid, ppv);
    }

    HRESULT STDMETHODCALLTYPE StubSlot11() override {
        BX_VTABLE_STUB_LOG("IDStorageFactoryX", 11u, "Slot11");
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE StubSlot12() override {
        BX_VTABLE_STUB_LOG("IDStorageFactoryX", 12u, "Slot12");
        return E_NOTIMPL;
    }

private:
    bool supports(REFIID riid) const override { return IsEqualIID(riid, kXboxFactoryIid); }
};
} // namespace

// Accepts BetterXen's synthetic factory IID and IUnknown (the synthetic factory),
// the public PC IDStorageFactory / IDStorageFactory1, and the inferred Xbox
// IDStorageFactoryX IID. Any other IID is logged with its value so `bx-cli trace`
// names the interface a guest asked for.
extern "C" HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (IsEqualIID(riid, __uuidof(IDStorageFactory)) || IsEqualIID(riid, __uuidof(IDStorageFactory1))) {
        auto* factory = new (std::nothrow) PublicFactory;
        return factory ? publish(factory, riid, ppv) : E_OUTOFMEMORY;
    }
    if (IsEqualIID(riid, kXboxFactoryIid)) {
        auto* factory = new (std::nothrow) XboxFactory;
        if (!factory) return E_OUTOFMEMORY;
        bx_log_emit(BX_LOG_INFO, "xcrdapi", "dstorage_get_factory", "dispatch xbox_com=IDStorageFactoryX");
        return publish(factory, riid, ppv);
    }
    if (!IsEqualIID(riid, factory_iid) && !IsEqualIID(riid, IID_IUnknown)) {
        char iid[40];
        format_iid(riid, iid);
        bx_log_emit(BX_LOG_WARN, "xcrdapi", "dstorage_get_factory_unknown_iid", iid);
        BX_STUB_LOG("XCRDAPI.dll", "DStorageGetFactory");
        return E_NOINTERFACE;
    }
    auto* factory = new (std::nothrow) StorageFactory;
    return factory ? publish(factory, riid, ppv) : E_OUTOFMEMORY;
}
