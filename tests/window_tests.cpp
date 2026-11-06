#include "../native/window.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
LRESULT CALLBACK fixedWindow(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_GETMINMAXINFO) {
        auto info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = info->ptMaxTrackSize = {1280, 720};
        return 0;
    }
    if (message == WM_SIZING) { GetWindowRect(window, reinterpret_cast<RECT*>(lParam)); return TRUE; }
    return DefWindowProcW(window, message, wParam, lParam);
}
void setClient(HWND window, LONG width, LONG height) {
    RECT rect{0, 0, width, height};
    require(AdjustWindowRectExForDpi(&rect, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE, 0, GetDpiForWindow(window)), "frame calculation failed");
    SetWindowPos(window, nullptr, 100, 100, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
}
int main() {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSW klass{}; klass.lpfnWndProc = fixedWindow; klass.lpszClassName = L"QAQRWindowResizeTest"; klass.hInstance = GetModuleHandleW(nullptr);
        require(RegisterClassW(&klass) != 0, "class registration failed");
        auto window = CreateWindowExW(0, klass.lpszClassName, L"QAQR isolated resize fixture", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 100, 100, 800, 500, nullptr, nullptr, klass.hInstance, nullptr);
        require(window != nullptr, "window creation failed");
        setClient(window, 960, 540);
        RECT before{}; GetClientRect(window, &before);
        require(!(GetWindowLongPtrW(window, GWL_STYLE) & WS_THICKFRAME), "fixture was already resizable");
        require(ark::attachWindowResize(window), "could not attach resize handler");
        RECT client{}; GetClientRect(window, &client);
        require(client.right == before.right && client.bottom == before.bottom, "adding border changed the render area");
        require(GetWindowLongPtrW(window, GWL_STYLE) & WS_THICKFRAME, "resize border missing");
        for (const SIZE size : {SIZE{960, 540}, SIZE{1000, 625}, SIZE{900, 600}}) {
            setClient(window, size.cx, size.cy);
            GetClientRect(window, &client);
            const double ratio = static_cast<double>(client.right) / client.bottom;
            RECT original{}; GetWindowRect(window, &original);
            const LONG frameX = original.right - original.left - client.right, frameY = original.bottom - original.top - client.bottom;
            SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
            MINMAXINFO limits{}; SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
            require(limits.ptMinTrackSize.x < 960 && limits.ptMaxTrackSize.x > 1280, "fixed resolution limits survived");
            for (WPARAM edge = WMSZ_LEFT; edge <= WMSZ_BOTTOMRIGHT; ++edge) for (bool shrink : {false, true}) {
                RECT rect = original;
                const LONG dx = shrink ? -150 : 203, dy = shrink ? -100 : 117;
                if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) rect.left -= dx;
                else if (edge != WMSZ_TOP && edge != WMSZ_BOTTOM) rect.right += dx;
                if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) rect.top -= dy;
                else if (edge != WMSZ_LEFT && edge != WMSZ_RIGHT) rect.bottom += dy;
                require(SendMessageW(window, WM_SIZING, edge, reinterpret_cast<LPARAM>(&rect)) == TRUE, "resize message not handled");
                require(std::abs((rect.right - rect.left - frameX) - (rect.bottom - rect.top - frameY) * ratio) <= 1.1, "client aspect ratio changed");
                if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) require(rect.right == original.right, "opposite right edge moved");
                else require(rect.left == original.left, "opposite left edge moved");
                if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) require(rect.bottom == original.bottom, "opposite bottom edge moved");
                else require(rect.top == original.top, "opposite top edge moved");
                require(rect.right - rect.left != original.right - original.left, "game's fixed sizing handler won");
            }
            SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
        }
        SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP);
        require(!(GetWindowLongPtrW(window, GWL_STYLE) & WS_THICKFRAME), "fullscreen acquired a resize border");
        MINMAXINFO fullscreen{}; SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&fullscreen));
        require(fullscreen.ptMinTrackSize.x == 1280 && fullscreen.ptMaxTrackSize.x == 1280, "fullscreen limits were changed");
        SetWindowLongPtrW(window, GWL_STYLE, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU);
        require(GetWindowLongPtrW(window, GWL_STYLE) & WS_THICKFRAME, "windowed mode did not restore border");
        DestroyWindow(window);
        // Exercise the actual worker -> window-thread hook handoff as well.
        // Fully transparent tool windows keep this fixture off the desktop.
        klass.lpszClassName = L"UnityWndClass";
        require(RegisterClassW(&klass) != 0, "Unity fixture registration failed");
        require(ark::startWindowResize(klass.hInstance), "window watcher did not start");
        for (int iteration = 0; iteration < 2; ++iteration) {
            window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, klass.lpszClassName, L"QAQR transparent fixture", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, -20000, -20000, 960, 540, nullptr, nullptr, klass.hInstance, nullptr);
            require(window != nullptr, "watcher fixture creation failed");
            SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA);
            ShowWindow(window, SW_SHOWNOACTIVATE);
            const auto deadline = GetTickCount64() + 5000;
            while (!(GetWindowLongPtrW(window, GWL_STYLE) & WS_THICKFRAME) && GetTickCount64() < deadline) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                Sleep(10);
            }
            require(GetWindowLongPtrW(window, GWL_STYLE) & WS_THICKFRAME, "watcher failed to attach on the owning thread or recreate the hook");
            DestroyWindow(window);
        }
        ark::stopWindowResize();
        std::cout << "Window resize: native HWND, eight edges, growth/shrink, 16:9/16:10/3:2, client area, anchors, fullscreen, thread handoff and window recreation passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
