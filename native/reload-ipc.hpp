#pragma once
#include "manifest.hpp"
#include <stdexcept>

namespace ark {
struct WinHandle {
    HANDLE value;
    explicit WinHandle(HANDLE handle) : value(handle) {}
    ~WinHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
};
inline std::wstring pipeName(DWORD pid) { return L"\\\\.\\pipe\\QAQR-Arknights-Reload-" + std::to_wstring(pid); }
inline DWORD finishIo(HANDLE pipe, OVERLAPPED& operation, BOOL started, DWORD timeout = 10000) {
    if (!started && GetLastError() != ERROR_IO_PENDING) throw std::runtime_error("Runtime communication failed");
    if (!started && WaitForSingleObject(operation.hEvent, timeout) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation); WaitForSingleObject(operation.hEvent, INFINITE);
        throw std::runtime_error("Runtime did not respond; restart the game with the updated QAQR loader");
    }
    DWORD count = 0;
    if (!GetOverlappedResult(pipe, &operation, &count, FALSE)) throw std::runtime_error("Runtime communication failed");
    return count;
}
inline DWORD pipeRead(HANDLE pipe, void* bytes, DWORD size) {
    WinHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED operation{}; operation.hEvent = event.value;
    return finishIo(pipe, operation, ReadFile(pipe, bytes, size, nullptr, &operation));
}
inline void pipeWrite(HANDLE pipe, const void* bytes, DWORD size) {
    WinHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED operation{}; operation.hEvent = event.value;
    if (finishIo(pipe, operation, WriteFile(pipe, bytes, size, nullptr, &operation)) != size) throw std::runtime_error("Incomplete runtime message");
}
inline std::string runtimeRequest(DWORD pid, const std::wstring& request) {
    if (request.size() > 32000) throw std::runtime_error("Runtime request path is too long");
    const auto name = pipeName(pid);
    if (!WaitNamedPipeW(name.c_str(), 3000)) throw std::runtime_error("No live-reload runtime found; restart the game using the updated QAQR loader");
    WinHandle pipe(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    ULONG server = 0;
    if (pipe.value == INVALID_HANDLE_VALUE || !GetNamedPipeServerProcessId(pipe.value, &server) || server != pid) throw std::runtime_error("Runtime process identity mismatch");
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr)) throw std::runtime_error("Cannot configure runtime communication");
    pipeWrite(pipe.value, request.c_str(), static_cast<DWORD>((request.size() + 1) * sizeof(wchar_t)));
    std::vector<char> bytes(65536);
    auto size = pipeRead(pipe.value, bytes.data(), static_cast<DWORD>(bytes.size()));
    const char received = 1;
    pipeWrite(pipe.value, &received, 1);
    return std::string(bytes.data(), size);
}
}
