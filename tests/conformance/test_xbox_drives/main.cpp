/*
 * Check BetterXen's inferred Xbox drive layout through synthetic file operations.
 *
 * The project-authorized target title we traced opens its package on G: and writes crash logs
 * to T:, and stops at startup when either is missing; that these roles hold for Xbox titles in
 * general is an inference. The fixture checks the resulting layout. The expected contents
 * of G: come from this program's own path (GetModuleFileNameW).
 */
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "FAIL: test_xbox_drives:%d: %s (Win32 error %lu)\n", \
                         __LINE__, #condition, static_cast<unsigned long>(GetLastError())); \
            return 1;                                                             \
        }                                                                         \
    } while (false)

static bool read_all(const wchar_t* path, std::vector<char>& out) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 28);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) &&
             got == out.size();
    }
    CloseHandle(file);
    return ok;
}

int main() {
    wchar_t self[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, self, MAX_PATH);
    CHECK(len > 0 && len < MAX_PATH);
    const wchar_t* name = std::wcsrchr(self, L'\\');
    CHECK(name != nullptr);
    ++name;

    // G: resolves to this executable's directory: the same file, byte for byte.
    wchar_t on_g[MAX_PATH]{};
    std::swprintf(on_g, MAX_PATH, L"G:\\%ls", name);
    std::vector<char> expected, actual;
    CHECK(read_all(self, expected));
    CHECK(read_all(on_g, actual));
    CHECK(actual == expected);

    // T: is a directory a title can create, read back and delete a file in.
    DWORD attrs = GetFileAttributesW(L"T:\\");
    CHECK(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
    wchar_t probe[MAX_PATH]{};
    std::swprintf(probe, MAX_PATH, L"T:\\bx_drive_probe_%lu.tmp",
                  static_cast<unsigned long>(GetCurrentProcessId()));
    static const char payload[] = "bx-drive-probe";
    HANDLE file = CreateFileW(probe, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    DWORD wrote = 0;
    BOOL written = WriteFile(file, payload, sizeof(payload), &wrote, nullptr);
    CloseHandle(file);
    CHECK(written && wrote == sizeof(payload));
    std::vector<char> back;
    CHECK(read_all(probe, back));
    CHECK(back.size() == sizeof(payload) && std::memcmp(back.data(), payload, sizeof(payload)) == 0);
    CHECK(DeleteFileW(probe));

    std::printf("PASS: test_xbox_drives\n");
    return 0;
}
