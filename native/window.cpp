#include "window.hpp"
#include <commctrl.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <new>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")

namespace ark {
namespace {
constexpr UINT_PTR subclassId = 0x51415257;
std::atomic_bool stopping = false;
std::atomic<HWND> activeWindow = nullptr, pendingWindow = nullptr;
struct WindowState {
    double ratio = 0;
    SIZE initial{};
    UINT_PTR timer = 0;
    bool sizing = false;
};
bool windowed(HWND window) {
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    return (style & WS_CAPTION) == WS_CAPTION && !(style & WS_CHILD) && !IsIconic(window);
}
void capture(HWND window, WindowState& state) {
    RECT client{};
    if (GetClientRect(window, &client) && client.right > 0 && client.bottom > 0) {
        state.ratio = static_cast<double>(client.right) / client.bottom;
        state.initial = {client.right, client.bottom};
    }
}
SIZE frameSize(HWND window) {
    RECT outer{}, client{};
    GetWindowRect(window, &outer); GetClientRect(window, &client);
    return {outer.right - outer.left - client.right, outer.bottom - outer.top - client.bottom};
}
void enableFrame(HWND window) {
    if (!windowed(window)) return;
    auto style = GetWindowLongPtrW(window, GWL_STYLE);
    if (style & WS_THICKFRAME) return;
    RECT client{};
    if (!GetClientRect(window, &client)) return;
    // Preserve the render area when adding the wider resize border.
    RECT outer = client;
    if (!AdjustWindowRectExForDpi(&outer, static_cast<DWORD>(style | WS_THICKFRAME), GetMenu(window) != nullptr,
        static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window))) return;
    SetWindowLongPtrW(window, GWL_STYLE, style | WS_THICKFRAME);
    SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
SIZE minimumClient(HWND window, double ratio) {
    const double scale = std::max(96u, GetDpiForWindow(window)) / 96.0;
    const auto height = static_cast<LONG>(std::ceil(std::max(180.0, 320.0 / ratio) * scale));
    return {static_cast<LONG>(std::lround(height * ratio)), height};
}
void constrain(HWND window, WindowState& state, WPARAM edge, RECT& rect) {
    if (state.ratio <= 0) capture(window, state);
    if (state.ratio <= 0 || edge < WMSZ_LEFT || edge > WMSZ_BOTTOMRIGHT) return;
    const auto frame = frameSize(window), minimum = minimumClient(window, state.ratio);
    LONG width = std::max(1L, rect.right - rect.left - frame.cx);
    LONG height = std::max(1L, rect.bottom - rect.top - frame.cy);
    const bool horizontal = edge == WMSZ_LEFT || edge == WMSZ_RIGHT;
    const bool vertical = edge == WMSZ_TOP || edge == WMSZ_BOTTOM;
    const bool useWidth = horizontal || (!vertical && std::abs(width - state.initial.cx) >= std::abs(height - state.initial.cy) * state.ratio);
    if (useWidth) {
        width = std::max(width, minimum.cx);
        height = static_cast<LONG>(std::lround(width / state.ratio));
    } else {
        height = std::max(height, minimum.cy);
        width = static_cast<LONG>(std::lround(height * state.ratio));
    }
    width += frame.cx; height += frame.cy;
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) rect.left = rect.right - width;
    else rect.right = rect.left + width;
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) rect.top = rect.bottom - height;
    else rect.bottom = rect.top + height;
}
LRESULT CALLBACK resizeProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
    auto& state = *reinterpret_cast<WindowState*>(data);
    if (message == WM_NCDESTROY) {
        if (state.timer) KillTimer(window, state.timer);
        RemoveWindowSubclass(window, resizeProc, id);
        HWND expected = window; activeWindow.compare_exchange_strong(expected, nullptr);
        delete &state;
        return DefSubclassProc(window, message, wParam, lParam);
    }
    if (stopping.load()) return DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_TIMER && wParam == state.timer) { enableFrame(window); return 0; }
    if (message == WM_STYLECHANGING && wParam == static_cast<WPARAM>(GWL_STYLE)) {
        auto result = DefSubclassProc(window, message, wParam, lParam);
        auto& style = *reinterpret_cast<STYLESTRUCT*>(lParam);
        if ((style.styleNew & WS_CAPTION) == WS_CAPTION && !(style.styleNew & WS_CHILD)) style.styleNew |= WS_THICKFRAME;
        return result;
    }
    if (!windowed(window)) return DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_ENTERSIZEMOVE) { capture(window, state); state.sizing = true; }
    if (message == WM_EXITSIZEMOVE) state.sizing = false;
    if (message == WM_NCHITTEST) {
        const auto hit = DefWindowProcW(window, message, wParam, lParam);
        if (hit >= HTLEFT && hit <= HTBOTTOMRIGHT) return hit;
    }
    if (message == WM_SETCURSOR && LOWORD(lParam) >= HTLEFT && LOWORD(lParam) <= HTBOTTOMRIGHT)
        return DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_SIZING) {
        auto proposed = *reinterpret_cast<RECT*>(lParam);
        DefSubclassProc(window, message, wParam, lParam);
        constrain(window, state, wParam, proposed);
        *reinterpret_cast<RECT*>(lParam) = proposed;
        return TRUE;
    }
    if (message == WM_GETMINMAXINFO) {
        auto result = DefSubclassProc(window, message, wParam, lParam);
        if (!state.sizing) capture(window, state);
        if (state.ratio > 0) {
            const auto frame = frameSize(window), minimum = minimumClient(window, state.ratio);
            auto& limits = *reinterpret_cast<MINMAXINFO*>(lParam);
            limits.ptMinTrackSize = {minimum.cx + frame.cx, minimum.cy + frame.cy};
            // Unity's fixed-window build can report the original resolution as
            // its maximum. Replace that constraint for interactive sizing.
            limits.ptMaxTrackSize = {GetSystemMetrics(SM_CXVIRTUALSCREEN) + frame.cx, GetSystemMetrics(SM_CYVIRTUALSCREEN) + frame.cy};
        }
        return result;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}
LRESULT CALLBACK installHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && !stopping.load()) {
        const auto& message = *reinterpret_cast<CWPSTRUCT*>(lParam);
        if (message.hwnd == pendingWindow.load()) attachWindowResize(message.hwnd);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
BOOL CALLBACK findWindow(HWND window, LPARAM parameter) {
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId() || GetWindow(window, GW_OWNER) || !IsWindowVisible(window)) return TRUE;
    wchar_t name[64]{}; GetClassNameW(window, name, 64);
    if (wcscmp(name, L"UnityWndClass")) return TRUE;
    *reinterpret_cast<HWND*>(parameter) = window;
    return FALSE;
}
DWORD WINAPI watchWindows(void* module) {
    while (!stopping.load()) {
        if (!activeWindow.load()) {
            HWND window = nullptr;
            EnumWindows(findWindow, reinterpret_cast<LPARAM>(&window));
            if (window) {
                pendingWindow.store(window);
                // The callback executes on Unity's window thread. Comctl32
                // subclass helpers must never be called from this worker.
                auto hook = SetWindowsHookExW(WH_CALLWNDPROC, installHook, static_cast<HMODULE>(module), GetWindowThreadProcessId(window, nullptr));
                if (hook) {
                    DWORD_PTR result = 0;
                    SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result);
                    UnhookWindowsHookEx(hook);
                }
                pendingWindow.store(nullptr);
            }
        }
        Sleep(1000);
    }
    return 0;
}
}
bool attachWindowResize(HWND window) {
    if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) return false;
    DWORD_PTR existing = 0;
    if (GetWindowSubclass(window, resizeProc, subclassId, &existing)) return true;
    auto state = new(std::nothrow) WindowState;
    if (!state) return false;
    capture(window, *state);
    if (!SetWindowSubclass(window, resizeProc, subclassId, reinterpret_cast<DWORD_PTR>(state))) { delete state; return false; }
    activeWindow.store(window);
    enableFrame(window);
    state->timer = SetTimer(window, reinterpret_cast<UINT_PTR>(state), 1000, nullptr);
    return true;
}
bool startWindowResize(HMODULE module) {
    auto thread = CreateThread(nullptr, 0, watchWindows, module, 0, nullptr);
    if (!thread) return false;
    CloseHandle(thread);
    return true;
}
void stopWindowResize() { stopping.store(true); }
}
