#include "../native/input.hpp"
#include <algorithm>
#include <climits>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

// NEXTRAWINPUTBLOCK names QWORD, which the SDK headers leave undefined.
using QWORD = unsigned long long;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Record { DWORD type; HANDLE device; WPARAM wParam; USHORT flags, buttons; LONG x, y; ULONG extra; DWORD size; };
HANDLE mouseA = reinterpret_cast<HANDLE>(0x10), mouseB = reinterpret_cast<HANDLE>(0x20);
Record move(HANDLE device, LONG x, LONG y, ULONG extra = 0) { return {RIM_TYPEMOUSE, device, RIM_INPUT, MOUSE_MOVE_RELATIVE, 0, x, y, extra, sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)}; }

// Lays records out exactly as GetRawInputBuffer does, walked by NEXTRAWINPUTBLOCK.
struct Buffer {
    std::vector<unsigned long long> storage = std::vector<unsigned long long>(4096);
    UINT count = 0;
    RAWINPUT* first() { return reinterpret_cast<RAWINPUT*>(storage.data()); }
    void fill(const std::vector<Record>& records) {
        std::fill(storage.begin(), storage.end(), 0xCDCDCDCDCDCDCDCDull);
        RAWINPUT* cursor = first();
        for (const auto& record : records) {
            std::memset(cursor, 0, record.size);
            cursor->header = {record.type, record.size, record.device, record.wParam};
            if (record.type == RIM_TYPEMOUSE) {
                cursor->data.mouse.usFlags = record.flags;
                cursor->data.mouse.usButtonFlags = record.buttons;
                cursor->data.mouse.lLastX = record.x;
                cursor->data.mouse.lLastY = record.y;
                cursor->data.mouse.ulExtraInformation = record.extra;
            } else if (record.type == RIM_TYPEHID) {
                cursor->data.hid.dwSizeHid = record.size - sizeof(RAWINPUTHEADER) - 2 * sizeof(DWORD);
                cursor->data.hid.dwCount = 1;
            }
            cursor = NEXTRAWINPUTBLOCK(cursor);
        }
        count = static_cast<UINT>(records.size());
    }
    std::vector<Record> read(UINT kept) {
        std::vector<Record> output;
        RAWINPUT* cursor = first();
        for (UINT index = 0; index < kept; ++index, cursor = NEXTRAWINPUTBLOCK(cursor)) {
            const auto& mouse = cursor->data.mouse;
            output.push_back({cursor->header.dwType, cursor->header.hDevice, cursor->header.wParam, mouse.usFlags, mouse.usButtonFlags, mouse.lLastX, mouse.lLastY, mouse.ulExtraInformation, cursor->header.dwSize});
        }
        return output;
    }
};
bool same(const Record& left, const Record& right) {
    if (left.type != right.type || left.device != right.device || left.wParam != right.wParam || left.size != right.size) return false;
    return left.type != RIM_TYPEMOUSE || (left.flags == right.flags && left.buttons == right.buttons && left.x == right.x && left.y == right.y && left.extra == right.extra);
}
void expect(const std::vector<Record>& input, const std::vector<Record>& expected, const char* message) {
    Buffer buffer; buffer.fill(input);
    const auto output = buffer.read(ark::coalesceRawInput(buffer.first(), buffer.count));
    require(output.size() == expected.size(), message);
    for (size_t index = 0; index < output.size(); ++index) require(same(output[index], expected[index]), message);
}

Buffer provided;
bool failProvider = false;
UINT WINAPI fakeProvider(PRAWINPUT data, PUINT size, UINT header) {
    require(header == sizeof(RAWINPUTHEADER), "header size was not forwarded");
    if (!data) { *size = sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE); return 0; }
    if (failProvider) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return static_cast<UINT>(-1); }
    std::memcpy(data, provided.storage.data(), provided.storage.size() * sizeof(unsigned long long));
    return provided.count;
}
struct Script { std::vector<MSG> items; size_t index = 0; };
MSG mouseMove(LPARAM position) { MSG message{}; message.message = WM_MOUSEMOVE; message.lParam = position; return message; }
MSG other(UINT type) { MSG message{}; message.message = type; return message; }
MSG empty() { return MSG{}; }
BOOL scripted(MSG* message, bool* paceable, void* context) {
    auto& script = *static_cast<Script*>(context);
    require(script.index < script.items.size(), "pacer read past the script");
    const MSG item = script.items[script.index++];
    if (!item.message) return FALSE;
    *message = item;
    *paceable = item.message == WM_MOUSEMOVE;
    return TRUE;
}
RAWINPUTDEVICE registered(USHORT usage) {
    UINT count = 0;
    GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
    std::vector<RAWINPUTDEVICE> devices(count + 1);
    count = static_cast<UINT>(devices.size());
    const UINT read = GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE));
    for (UINT index = 0; read != static_cast<UINT>(-1) && index < read; ++index) if (devices[index].usUsagePage == 1 && devices[index].usUsage == usage) return devices[index];
    return RAWINPUTDEVICE{};
}
std::vector<MSG> drainInput() {
    std::vector<MSG> inputs;
    MSG message{};
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) if (message.message == WM_INPUT) inputs.push_back(message);
    return inputs;
}
void* swapOwnImport(void* replacement) {
    auto base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; entry->Name; ++entry) {
        if (_stricmp(reinterpret_cast<char*>(base + entry->Name), "USER32.dll")) continue;
        auto name = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->OriginalFirstThunk);
        auto slot = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->FirstThunk);
        for (; name->u1.AddressOfData; ++name, ++slot) {
            if (IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal) || strcmp(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name->u1.AddressOfData)->Name, "GetRawInputBuffer")) continue;
            DWORD protection = 0;
            require(VirtualProtect(&slot->u1.Function, sizeof(void*), PAGE_READWRITE, &protection), "fixture import is not writable");
            auto previous = reinterpret_cast<void*>(slot->u1.Function);
            slot->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&slot->u1.Function, sizeof(void*), protection, &protection);
            return previous;
        }
    }
    throw std::runtime_error("fixture does not import GetRawInputBuffer");
}
}

int main() {
    try {
        // An 8000 Hz burst between frames: one record per 125 microseconds.
        std::vector<Record> burst;
        for (int index = 0; index < 133; ++index) burst.push_back(move(mouseA, index % 3 - 1 + 2, -(index % 2), index));
        LONG x = 0, y = 0; for (const auto& record : burst) { x += record.x; y += record.y; }
        expect(burst, {move(mouseA, x, y, 132)}, "a frame of motion did not fold into one record");

        Record press = move(mouseA, 3, 1); press.buttons = RI_MOUSE_LEFT_BUTTON_DOWN;
        Record wheel = move(mouseA, 0, 0); wheel.buttons = RI_MOUSE_WHEEL;
        Record absolute = move(mouseA, 30000, 20000); absolute.flags = MOUSE_MOVE_ABSOLUTE;
        Record sink = move(mouseA, 4, 4); sink.wParam = RIM_INPUTSINK;
        Record key{RIM_TYPEKEYBOARD, reinterpret_cast<HANDLE>(0x30), RIM_INPUT, 0, 0, 0, 0, 0, sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD)};
        Record hid{RIM_TYPEHID, reinterpret_cast<HANDLE>(0x40), RIM_INPUT, 0, 0, 0, 0, 0, sizeof(RAWINPUTHEADER) + 2 * sizeof(DWORD) + 5};
        expect({move(mouseA, 1, 1), move(mouseA, 2, 2), press, move(mouseA, 1, 0), move(mouseA, 1, 0), wheel, move(mouseA, -1, 0)},
            {move(mouseA, 3, 3), press, move(mouseA, 2, 0), wheel, move(mouseA, -1, 0)}, "buttons or wheel were merged or reordered");
        expect({move(mouseA, 1, 0), move(mouseB, 1, 0), move(mouseB, 1, 0), move(mouseA, 1, 0)},
            {move(mouseA, 1, 0), move(mouseB, 2, 0), move(mouseA, 1, 0)}, "different devices were merged");
        expect({move(mouseA, 1, 0), key, move(mouseA, 1, 0), hid, move(mouseA, 1, 0), move(mouseA, 1, 0)},
            {move(mouseA, 1, 0), key, move(mouseA, 1, 0), hid, move(mouseA, 2, 0)}, "keyboard or variable-size HID records lost their place");
        Record sinkRun = sink; sinkRun.x = sinkRun.y = 8;
        expect({absolute, absolute, move(mouseA, 1, 0), sink, sink}, {absolute, absolute, move(mouseA, 1, 0), sinkRun}, "absolute or background records were merged across kinds");
        expect({move(mouseA, LONG_MAX - 1, 0), move(mouseA, 5, 0), move(mouseA, 1, 0)}, {move(mouseA, LONG_MAX - 1, 0), move(mouseA, 6, 0)}, "overflow was not split into a new record");
        expect({move(mouseA, 7, 7)}, {move(mouseA, 7, 7)}, "a single record changed");
        require(ark::coalesceRawInput(nullptr, 0) == 0, "empty input changed");

        // The pacer on its own: one move per pump cycle, newest wins, order kept.
        {
            Script script{{mouseMove(1), other(WM_KEYDOWN), mouseMove(2), mouseMove(3), empty(), empty(), mouseMove(4), other(WM_LBUTTONDOWN), empty()}};
            ark::MovePacer pacer;
            const std::vector<std::pair<BOOL, LPARAM>> expected{{TRUE, 1}, {TRUE, -1}, {FALSE, 0}, {TRUE, 3}, {FALSE, 0}, {FALSE, 0}, {TRUE, 4}, {TRUE, -1}, {FALSE, 0}};
            for (const auto& [result, position] : expected) {
                MSG message{};
                require(pacer.next(&message, scripted, &script) == result, "pacer returned the wrong pump result");
                if (result && position >= 0) require(message.message == WM_MOUSEMOVE && message.lParam == position, "pacer delivered the wrong move");
                if (result && position < 0) require(message.message != WM_MOUSEMOVE, "pacer reordered a non-move message");
            }
            require(pacer.deferred() == 1, "deferred moves were not counted");
        }

        // A message-only window stands in for Unity's; it registered the mouse
        // before the runtime was installed.
        WNDCLASSW klass{}; klass.lpfnWndProc = DefWindowProcW; klass.hInstance = GetModuleHandleW(nullptr); klass.lpszClassName = L"QAQRInputFixture";
        require(RegisterClassW(&klass) != 0, "fixture class registration failed");
        const HWND unity = CreateWindowExW(0, klass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, klass.hInstance, nullptr);
        require(unity != nullptr, "fixture window creation failed");
        RAWINPUTDEVICE early{1, 2, 0, unity};
        require(RegisterRawInputDevices(&early, 1, sizeof(early)), "fixture mouse registration failed");

        // Route a real import through the replacement, as UnityPlayer.dll does.
        provided.fill(burst);
        auto real = swapOwnImport(reinterpret_cast<void*>(&fakeProvider));
        require(!ark::mouseCoalescingStats().active, "coalescing reported active before installation");
        require(ark::coalesceModuleRawInput(GetModuleHandleW(nullptr)), "own import could not be patched");
        require(ark::coalesceModuleRawInput(GetModuleHandleW(nullptr)), "second installation was not idempotent");
        require(!ark::coalesceModuleRawInput(GetModuleHandleW(L"kernel32.dll")), "a module without the import was patched");
        Buffer received; UINT size = static_cast<UINT>(received.storage.size() * sizeof(unsigned long long));
        const UINT kept = GetRawInputBuffer(received.first(), &size, sizeof(RAWINPUTHEADER));
        const auto output = received.read(kept);
        require(output.size() == 1 && same(output[0], move(mouseA, x, y, 132)), "patched import did not fold the batch");
        UINT query = 0;
        require(GetRawInputBuffer(nullptr, &query, sizeof(RAWINPUTHEADER)) == 0 && query == sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE), "size query changed");
        failProvider = true;
        require(GetRawInputBuffer(received.first(), &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "error result changed");
        failProvider = false;
        auto stats = ark::mouseCoalescingStats();
        // Real mouse records may already reach the relay, so only the floor is exact.
        require(stats.active && stats.records >= 133 && stats.delivered == 1, "statistics do not match the delivered batch");

        // The existing registration moved to the relay thread's window.
        const auto relayTarget = registered(2);
        wchar_t name[64]{};
        GetClassNameW(relayTarget.hwndTarget, name, 64);
        require(!wcscmp(name, L"QAQRRawMouseRelay") && (relayTarget.dwFlags & RIDEV_INPUTSINK), "existing mouse registration was not moved to the relay");
        require(ark::mouseCoalescingStats().redirected, "relay did not report the redirect");
        // Unity's own call: mouse moves to the relay, the keyboard stays put.
        RAWINPUTDEVICE both[2] = {{1, 2, RIDEV_INPUTSINK, unity}, {1, 6, 0, unity}};
        require(RegisterRawInputDevices(both, 2, sizeof(RAWINPUTDEVICE)), "Unity-style registration failed");
        require(registered(2).hwndTarget == relayTarget.hwndTarget && registered(6).hwndTarget == unity, "registration was not split between relay and Unity");

        // Real mouse movement reaches the relay from here on. Unregister at the
        // system level, behind the hook, so the checks below see test data only.
        provided.count = 0;  // the system buffer is empty from here on
        const auto system = reinterpret_cast<decltype(&RegisterRawInputDevices)>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "RegisterRawInputDevices"));
        RAWINPUTDEVICE quiet[2] = {{1, 2, RIDEV_REMOVE, nullptr}, {1, 6, RIDEV_REMOVE, nullptr}};
        require(system(quiet, 2, sizeof(RAWINPUTDEVICE)), "could not isolate the fixture from real input");
        Sleep(100);  // let the relay thread finish input it already received
        for (const auto& message : drainInput()) {
            RAWINPUT discarded{}; UINT discardedSize = sizeof(discarded);
            GetRawInputData(reinterpret_cast<HRAWINPUT>(message.lParam), RID_INPUT, &discarded, &discardedSize, sizeof(RAWINPUTHEADER));
        }
        for (Buffer scratch;;) {
            UINT scratchSize = static_cast<UINT>(scratch.storage.size() * sizeof(unsigned long long));
            if (!GetRawInputBuffer(scratch.first(), &scratchSize, sizeof(RAWINPUTHEADER))) break;
        }
        const auto relayedBefore = ark::mouseCoalescingStats().relayed;
        RAWINPUT raw{};
        raw.header = {RIM_TYPEMOUSE, sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE), mouseA, RIM_INPUTSINK};
        raw.data.mouse.lLastX = 2; raw.data.mouse.lLastY = -1;
        for (int index = 0; index < 100; ++index) ark::relayRawMouse(raw);
        RAWINPUT click = raw;
        click.data.mouse.usButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN; click.data.mouse.lLastX = click.data.mouse.lLastY = 0;
        ark::relayRawMouse(click);
        raw.data.mouse.lLastX = -3; raw.data.mouse.lLastY = 0;
        for (int index = 0; index < 10; ++index) ark::relayRawMouse(raw);
        auto posted = drainInput();
        require(posted.size() == 1 && posted[0].hwnd == unity, "relay did not post exactly one WM_INPUT for the batch");
        const auto token = reinterpret_cast<HRAWINPUT>(posted[0].lParam);
        RAWINPUT first{}; UINT got = sizeof(first);
        require(GetRawInputData(token, RID_INPUT, &first, &got, sizeof(RAWINPUTHEADER)) == sizeof(RAWINPUT), "posted record could not be read");
        require(first.data.mouse.lLastX == 200 && first.data.mouse.lLastY == -100 && first.data.mouse.usButtonFlags == 0, "relayed motion was not folded");
        Buffer rest; UINT restSize = static_cast<UINT>(rest.storage.size() * sizeof(unsigned long long));
        const auto tail = rest.read(GetRawInputBuffer(rest.first(), &restSize, sizeof(RAWINPUTHEADER)));
        require(tail.size() == 2 && tail[0].buttons == RI_MOUSE_LEFT_BUTTON_DOWN && tail[1].x == -30, "click or later motion lost its order");
        require(GetRawInputBuffer(rest.first(), &restSize, sizeof(RAWINPUTHEADER)) == 0, "relay records were delivered twice");
        got = sizeof(first);
        require(GetRawInputData(token, RID_INPUT, &first, &got, sizeof(RAWINPUTHEADER)) == sizeof(RAWINPUT) && first.data.mouse.lLastX == 0 && first.data.mouse.lLastY == 0 && first.data.mouse.usButtonFlags == 0, "a late read was not motionless");
        query = 0;
        require(GetRawInputData(token, RID_INPUT, nullptr, &query, sizeof(RAWINPUTHEADER)) == 0 && query == sizeof(RAWINPUT), "relay size query failed");
        query = 8;
        require(GetRawInputData(token, RID_INPUT, &first, &query, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "short buffer was accepted");
        // Unity's window procedure ends in DefWindowProc for this message.
        DefWindowProcW(unity, WM_INPUT, RIM_INPUTSINK, posted[0].lParam);
        DefWindowProcW(unity, WM_INPUT, RIM_INPUT, posted[0].lParam);
        ark::relayRawMouse(raw);
        require(drainInput().size() == 1, "posting did not resume after Unity read the batch");
        got = sizeof(first);
        require(GetRawInputData(token, RID_INPUT, &first, &got, sizeof(RAWINPUTHEADER)) == sizeof(RAWINPUT) && first.data.mouse.lLastX == -3, "second batch was wrong");

        // The patched pump: one move per cycle, newest wins, held move goes before a click.
        const auto post = [&](UINT message, int x) { PostMessageW(unity, message, 0, MAKELPARAM(x, x)); };
        const auto pump = [](BOOL result, UINT message, int x, const char* failure) {
            MSG received{};
            require(PeekMessageA(&received, nullptr, 0, 0, PM_REMOVE) == result, failure);
            if (result) require(received.message == message && received.lParam == MAKELPARAM(x, x), failure);
        };
        post(WM_MOUSEMOVE, 1); post(WM_MOUSEMOVE, 2); post(WM_MOUSEMOVE, 3);
        pump(TRUE, WM_MOUSEMOVE, 1, "first move was not delivered");
        pump(FALSE, 0, 0, "a second move did not end the pump cycle");
        pump(TRUE, WM_MOUSEMOVE, 3, "the newest move did not replace the held one");
        pump(FALSE, 0, 0, "cycle did not end after the newest move");
        post(WM_MOUSEMOVE, 4); post(WM_MOUSEMOVE, 5); post(WM_LBUTTONDOWN, 5);
        pump(TRUE, WM_MOUSEMOVE, 4, "move before click was not delivered");
        pump(FALSE, 0, 0, "move before click did not end the cycle");
        pump(TRUE, WM_MOUSEMOVE, 5, "held move was not delivered before the click");
        pump(TRUE, WM_LBUTTONDOWN, 5, "click was lost or reordered");
        pump(FALSE, 0, 0, "cycle did not end after the click");
        post(WM_MOUSEMOVE, 6); post(WM_MOUSEMOVE, 7);
        pump(TRUE, WM_MOUSEMOVE, 6, "move was not delivered");
        pump(FALSE, 0, 0, "move did not end the cycle");
        pump(TRUE, WM_MOUSEMOVE, 7, "the final position was lost when movement stopped");
        pump(FALSE, 0, 0, "queue did not end empty");
        MSG filtered{};
        post(WM_MOUSEMOVE, 8); post(WM_MOUSEMOVE, 9);
        require(PeekMessageA(&filtered, unity, 0, 0, PM_REMOVE) && PeekMessageA(&filtered, unity, 0, 0, PM_REMOVE) && filtered.lParam == MAKELPARAM(9, 9), "filtered pumps were paced");

        stats = ark::mouseCoalescingStats();
        require(stats.relayed - relayedBefore == 112 && stats.deferred == 3, "relay or pacing statistics are wrong");
        // Unity removing the mouse stops the relay.
        require(RegisterRawInputDevices(both, 2, sizeof(RAWINPUTDEVICE)), "re-registration failed");
        RAWINPUTDEVICE removal[2] = {{1, 2, RIDEV_REMOVE, nullptr}, {1, 6, RIDEV_REMOVE, nullptr}};
        require(RegisterRawInputDevices(removal, 2, sizeof(RAWINPUTDEVICE)), "removal failed");
        require(!ark::mouseCoalescingStats().redirected && registered(2).hwndTarget == nullptr, "removal did not stop the relay");
        Sleep(50);
        drainInput();  // real input may have been posted while registered
        ark::relayRawMouse(raw);
        require(drainInput().empty(), "relay posted after removal");
        DestroyWindow(unity);
        swapOwnImport(real);
        std::cout << "Mouse coalescing: 8000 Hz burst, buttons, wheel, devices, keyboard/HID order, absolute, background, overflow, import routing, size query and errors passed\n";
        std::cout << "Mouse relay and pacing: late registration takeover, keyboard untouched, one WM_INPUT per batch, folding, click order, motionless late read, cleanup, one move per pump cycle, final position, filtered pumps and removal passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
