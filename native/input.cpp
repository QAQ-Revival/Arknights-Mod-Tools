#include "input.hpp"
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <vector>

#pragma comment(lib, "user32.lib")

// Unity 2021.3 registers the mouse for raw input on its own window and pumps
// one message per loop iteration until the queue is empty, only then running a
// frame. With raw input in the same queue Windows no longer folds mouse moves,
// so an 8000 Hz mouse puts a WM_INPUT and a WM_MOUSEMOVE in the queue for every
// report. Each costs Unity hundreds of microseconds, and the pump rarely
// empties. Measured in game: 80-97 % of the main thread spent pumping.
//
// The fix keeps that traffic away from Unity's thread:
// - the mouse registration is pointed at a relay window on its own thread,
//   which folds motion and posts Unity a single WM_INPUT per pump cycle;
// - Unity's pump takes at most one WM_MOUSEMOVE per cycle (MovePacer);
// - buffered reads still fold consecutive motion records.
namespace ark {
namespace {
using RawInputBuffer = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
using RawInputData = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using RegisterDevices = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
using PeekMessageFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
std::atomic<RawInputBuffer> originalBuffer = nullptr;
std::atomic<RawInputData> originalData = nullptr;
std::atomic<RegisterDevices> originalRegister = nullptr;
std::atomic<PeekMessageFn> originalPeek = nullptr;
std::atomic<unsigned long long> recordsRead = 0, recordsDelivered = 0, recordsRelayed = 0, movesDeferred = 0;
constexpr DWORD mouseSize = sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE);
static_assert(sizeof(RAWINPUT) == mouseSize, "relay records are stored as RAWINPUT");

RAWINPUT* next(RAWINPUT* record) {
    // Same address-based alignment as NEXTRAWINPUTBLOCK.
    return reinterpret_cast<RAWINPUT*>((reinterpret_cast<ULONG_PTR>(record) + record->header.dwSize + sizeof(ULONG_PTR) - 1) & ~(sizeof(ULONG_PTR) - 1));
}
bool motion(const RAWINPUT& record) {
    return record.header.dwType == RIM_TYPEMOUSE && record.header.dwSize == mouseSize &&
        !(record.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && record.data.mouse.usButtonFlags == 0;
}
bool joins(const RAWINPUT& run, const RAWINPUT& record) {
    if (!motion(run) || !motion(record) || run.header.hDevice != record.header.hDevice ||
        run.header.wParam != record.header.wParam || run.data.mouse.usFlags != record.data.mouse.usFlags) return false;
    const auto x = static_cast<long long>(run.data.mouse.lLastX) + record.data.mouse.lLastX;
    const auto y = static_cast<long long>(run.data.mouse.lLastY) + record.data.mouse.lLastY;
    return x >= LONG_MIN && x <= LONG_MAX && y >= LONG_MIN && y <= LONG_MAX;
}
void fold(RAWINPUT& run, const RAWINPUT& record) {
    // Keep the newest per-report state; only motion is accumulated.
    run.data.mouse.lLastX += record.data.mouse.lLastX;
    run.data.mouse.lLastY += record.data.mouse.lLastY;
    run.data.mouse.ulRawButtons = record.data.mouse.ulRawButtons;
    run.data.mouse.ulExtraInformation = record.data.mouse.ulExtraInformation;
}

struct Relay {
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<RAWINPUT> queue;
    HWND unity = nullptr;
    HANDLE lastDevice = nullptr;
    bool posted = false, sink = false;
} relay;
struct Locked {
    Locked() { AcquireSRWLockExclusive(&relay.lock); }
    ~Locked() { ReleaseSRWLockExclusive(&relay.lock); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
};
std::atomic<HWND> relayWindow = nullptr;
std::atomic_bool relayStarted = false;
HANDLE relayReady = nullptr;
// Posted WM_INPUT messages carry this address instead of a system handle.
char relayToken = 0;
HRAWINPUT relayHandle() { return reinterpret_cast<HRAWINPUT>(&relayToken); }
constexpr size_t relayLimit = 4096;

bool foregroundIsOurs() {
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}
LRESULT CALLBACK relayProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INPUT) {
        RAWINPUT record{};
        UINT size = sizeof(record);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &record, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) relayRawMouse(record);
    }
    // DefWindowProc releases the system's raw input data.
    return DefWindowProcW(window, message, wParam, lParam);
}
DWORD WINAPI relayThread(void* module) {
    WNDCLASSW klass{};
    klass.lpfnWndProc = relayProc;
    klass.hInstance = static_cast<HINSTANCE>(module);
    klass.lpszClassName = L"QAQRRawMouseRelay";
    RegisterClassW(&klass);
    const HWND window = CreateWindowExW(0, klass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, klass.hInstance, nullptr);
    relayWindow.store(window);
    SetEvent(relayReady);
    if (!window) return 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    return 0;
}
bool startRelay() {
    if (relayStarted.exchange(true)) return relayWindow.load() != nullptr;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&relayProc), &self);
    relayReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!relayReady) return false;
    if (auto thread = CreateThread(nullptr, 0, relayThread, self, 0, nullptr)) {
        WaitForSingleObject(relayReady, 5000);
        CloseHandle(thread);
    }
    return relayWindow.load() != nullptr;
}

bool redirectable(const RAWINPUTDEVICE& device) {
    // Only Unity's plain registration; anything with notifications or legacy
    // suppression stays exactly as requested.
    return device.hwndTarget && (device.dwFlags & ~RIDEV_INPUTSINK) == 0;
}
BOOL WINAPI relayRegister(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
    const auto call = originalRegister.load();
    const HWND window = relayWindow.load();
    if (!devices || !count || size != sizeof(RAWINPUTDEVICE) || !window) return call(devices, count, size);
    std::vector<RAWINPUTDEVICE> changed(devices, devices + count);
    HWND unity = nullptr;
    bool mouse = false, sink = false;
    for (auto& device : changed) {
        if (device.usUsagePage != 1 || device.usUsage != 2) continue;
        mouse = true;
        if (!redirectable(device)) continue;
        unity = device.hwndTarget;
        sink = (device.dwFlags & RIDEV_INPUTSINK) != 0;
        device.hwndTarget = window;
        device.dwFlags |= RIDEV_INPUTSINK;
    }
    if (!unity) {
        const BOOL result = call(devices, count, size);
        if (result && mouse) { Locked scope; relay.unity = nullptr; relay.queue.clear(); relay.posted = false; }
        return result;
    }
    // Publish the post target before the first record can reach the relay.
    { Locked scope; relay.unity = unity; relay.sink = sink; }
    if (call(changed.data(), count, size)) return TRUE;
    // Never leave Unity without mouse input: fall back to its own request.
    { Locked scope; relay.unity = nullptr; }
    return call(devices, count, size);
}
void adoptRegistration() {
    // The runtime may be injected after Unity registered the mouse.
    UINT count = 0;
    GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
    if (!count) return;
    std::vector<RAWINPUTDEVICE> devices(count);
    const UINT read = GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE));
    if (read == static_cast<UINT>(-1)) return;
    for (UINT index = 0; index < read; ++index) {
        const auto& device = devices[index];
        DWORD process = 0;
        if (device.usUsagePage != 1 || device.usUsage != 2 || device.hwndTarget == relayWindow.load() || !redirectable(device)) continue;
        GetWindowThreadProcessId(device.hwndTarget, &process);
        if (process == GetCurrentProcessId()) relayRegister(&device, 1, sizeof(RAWINPUTDEVICE));
    }
}
UINT WINAPI relayData(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT headerSize) {
    if (input != relayHandle()) return originalData.load()(input, command, data, size, headerSize);
    if (!size || headerSize != sizeof(RAWINPUTHEADER) || (command != RID_INPUT && command != RID_HEADER)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return static_cast<UINT>(-1);
    }
    const UINT needed = command == RID_INPUT ? mouseSize : sizeof(RAWINPUTHEADER);
    if (!data) { *size = needed; return 0; }
    if (*size < needed) { *size = needed; SetLastError(ERROR_INSUFFICIENT_BUFFER); return static_cast<UINT>(-1); }
    RAWINPUT record{};
    bool delivered = false;
    {
        Locked scope;
        if (!relay.queue.empty()) {
            record = relay.queue.front();
            if (command == RID_INPUT) { relay.queue.erase(relay.queue.begin()); delivered = true; }
        } else {
            // Already drained by a buffered read: a motionless record is a no-op.
            record.header = {RIM_TYPEMOUSE, mouseSize, relay.lastDevice, RIM_INPUTSINK};
        }
        if (command == RID_INPUT) relay.posted = false;
    }
    if (delivered) recordsDelivered.fetch_add(1, std::memory_order_relaxed);
    std::memcpy(data, &record, needed);
    return needed;
}
UINT WINAPI coalescedBuffer(PRAWINPUT data, PUINT size, UINT headerSize) {
    if (data && size && headerSize == sizeof(RAWINPUTHEADER)) {
        size_t taken = 0;
        {
            Locked scope;
            taken = std::min<size_t>(*size / sizeof(RAWINPUT), relay.queue.size());
            std::copy_n(relay.queue.begin(), taken, data);
            relay.queue.erase(relay.queue.begin(), relay.queue.begin() + static_cast<std::ptrdiff_t>(taken));
        }
        if (taken) {
            recordsDelivered.fetch_add(taken, std::memory_order_relaxed);
            return static_cast<UINT>(taken);
        }
    }
    const auto call = originalBuffer.load();
    const UINT count = call(data, size, headerSize);
    if (!data || !count || count == static_cast<UINT>(-1) || headerSize != sizeof(RAWINPUTHEADER)) return count;
    const UINT kept = coalesceRawInput(data, count);
    recordsRead.fetch_add(count, std::memory_order_relaxed);
    recordsDelivered.fetch_add(kept, std::memory_order_relaxed);
    return kept;
}

MovePacer pacer;
std::atomic<DWORD> pumpThread = 0;
BOOL peekPump(MSG* message, bool* paceable, void*) {
    const BOOL result = originalPeek.load()(message, nullptr, 0, 0, PM_REMOVE);
    // Unity itself skips moves synthesized from pen or touch; leave them alone.
    *paceable = result && message->message == WM_MOUSEMOVE &&
        (static_cast<ULONG_PTR>(GetMessageExtraInfo()) & 0xFF515700) != 0xFF515700;
    return result;
}
BOOL WINAPI pacedPeek(LPMSG message, HWND window, UINT first, UINT last, UINT remove) {
    const auto call = originalPeek.load();
    // Only the main loop's unfiltered removal; wait loops pass filters.
    if (!message || window || first || last || remove != PM_REMOVE) return call(message, window, first, last, remove);
    const DWORD thread = GetCurrentThreadId();
    DWORD expected = 0;
    if (!pumpThread.compare_exchange_strong(expected, thread) && expected != thread) return call(message, window, first, last, remove);
    const BOOL result = pacer.next(message, peekPump, nullptr);
    movesDeferred.store(pacer.deferred(), std::memory_order_relaxed);
    return result;
}

IMAGE_THUNK_DATA* importSlot(HMODULE module, const char* library, const char* function) {
    const auto base = reinterpret_cast<BYTE*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return nullptr;
    for (auto entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); entry->Name; ++entry) {
        if (_stricmp(reinterpret_cast<const char*>(base + entry->Name), library) || !entry->OriginalFirstThunk) continue;
        auto name = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + entry->OriginalFirstThunk);
        auto slot = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->FirstThunk);
        for (; name->u1.AddressOfData; ++name, ++slot) {
            if (IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal)) continue;
            if (!strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name->u1.AddressOfData)->Name, function)) return slot;
        }
    }
    return nullptr;
}
template<class T> bool hookImport(HMODULE module, const char* function, T replacement, std::atomic<T>& original) {
    auto slot = importSlot(module, "USER32.dll", function);
    if (!slot) return false;
    const auto current = reinterpret_cast<T>(slot->u1.Function);
    if (current == replacement) return true;
    if (!current) return false;
    DWORD protection = 0;
    if (!VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), PAGE_READWRITE, &protection)) return false;
    // Publish the forward target before any caller can reach the replacement.
    original.store(current);
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&slot->u1.Function), reinterpret_cast<void*>(replacement));
    VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), protection, &protection);
    return true;
}
DWORD WINAPI waitForUnity(void*) {
    for (int attempt = 0; attempt < 1200; ++attempt) {
        if (auto unity = GetModuleHandleW(L"UnityPlayer.dll")) { coalesceModuleRawInput(unity); return 0; }
        Sleep(50);
    }
    return 0;
}
}

UINT coalesceRawInput(RAWINPUT* records, UINT count) {
    if (!records || !count) return count;
    RAWINPUT* read = records;
    RAWINPUT* write = records;
    RAWINPUT* run = nullptr;
    UINT kept = 0;
    for (UINT index = 0; index < count; ++index) {
        RAWINPUT* following = next(read);
        if (run && joins(*run, *read)) fold(*run, *read);
        else {
            // Compaction only ever moves records towards the start.
            if (write != read) memmove(write, read, read->header.dwSize);
            run = write;
            write = next(write);
            ++kept;
        }
        read = following;
    }
    return kept;
}

void relayRawMouse(const RAWINPUT& record) {
    if (record.header.dwType != RIM_TYPEMOUSE || record.header.dwSize != mouseSize) return;
    const bool foreground = foregroundIsOurs();
    HWND target = nullptr;
    {
        Locked scope;
        // Unity registered without background input: keep that behaviour.
        if (!relay.unity || (!relay.sink && !foreground)) return;
        if (!relay.queue.empty() && joins(relay.queue.back(), record)) fold(relay.queue.back(), record);
        else if (relay.queue.size() < relayLimit) relay.queue.push_back(record);
        relay.lastDevice = record.header.hDevice;
        if (!relay.posted) { relay.posted = true; target = relay.unity; }
    }
    recordsRead.fetch_add(1, std::memory_order_relaxed);
    recordsRelayed.fetch_add(1, std::memory_order_relaxed);
    if (target && !PostMessageW(target, WM_INPUT, RIM_INPUTSINK, reinterpret_cast<LPARAM>(relayHandle()))) {
        Locked scope;
        relay.posted = false;
    }
}

BOOL MovePacer::next(MSG* message, Peek peek, void* context) {
    if (hasQueued) { *message = queued; hasQueued = false; return TRUE; }
    bool paceable = false;
    if (!peek(message, &paceable, context)) {
        // Queue empty: the held position goes out before the frame runs.
        if (holding) { *message = held; holding = false; moves = 1; return TRUE; }
        moves = 0;
        return FALSE;
    }
    if (paceable) {
        holding = false;  // a newer position replaces a held one
        if (moves) {
            held = *message;
            holding = true;
            moves = 0;
            ++deferredMoves;
            return FALSE;  // end this cycle so the frame can run
        }
        moves = 1;
        return TRUE;
    }
    if (holding) {
        // Deliver the held move before whatever followed it, e.g. a click.
        queued = *message;
        hasQueued = true;
        *message = held;
        holding = false;
        moves = 1;
    }
    return TRUE;
}

bool coalesceModuleRawInput(HMODULE module) {
    const bool buffered = hookImport(module, "GetRawInputBuffer", &coalescedBuffer, originalBuffer);
    if (startRelay() && hookImport(module, "GetRawInputData", &relayData, originalData) &&
        hookImport(module, "RegisterRawInputDevices", &relayRegister, originalRegister)) adoptRegistration();
    hookImport(module, "PeekMessageA", &pacedPeek, originalPeek);
    return buffered;
}

bool startMouseCoalescing() {
    HMODULE self = nullptr;
    // The patched imports point into this DLL for the rest of the process.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&coalescedBuffer), &self)) return false;
    auto thread = CreateThread(nullptr, 0, waitForUnity, nullptr, 0, nullptr);
    if (!thread) return false;
    CloseHandle(thread);
    return true;
}

MouseCoalescingStats mouseCoalescingStats() {
    bool redirected = false;
    { Locked scope; redirected = relay.unity != nullptr; }
    return {originalBuffer.load() != nullptr, redirected, recordsRead.load(std::memory_order_relaxed), recordsDelivered.load(std::memory_order_relaxed),
        recordsRelayed.load(std::memory_order_relaxed), movesDeferred.load(std::memory_order_relaxed)};
}
}
