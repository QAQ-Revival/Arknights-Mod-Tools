#pragma once
#include <windows.h>

namespace ark {
// Folds runs of relative mouse motion inside a GetRawInputBuffer result into
// one record per run. Totals, buttons, wheel data and record order survive.
// Returns the record count left at the start of the buffer.
UINT coalesceRawInput(RAWINPUT* records, UINT count);
// Installs the mouse fixes on one module's USER32 imports: buffered record
// folding, the raw mouse relay and WM_MOUSEMOVE pacing.
bool coalesceModuleRawInput(HMODULE module);
// Waits for UnityPlayer.dll on a worker thread, then patches its imports only.
bool startMouseCoalescing();
// The relay thread hands every raw mouse record here; tests feed it directly.
void relayRawMouse(const RAWINPUT& record);

// Lets a message pump take at most one WM_MOUSEMOVE per cycle. A further move
// ends the cycle and is delivered first in the next one unless a newer move
// replaces it, so the final pointer position is never lost and clicks keep
// their order.
class MovePacer {
public:
    // Same contract as PeekMessage(PM_REMOVE); *paceable marks a mouse move.
    using Peek = BOOL (*)(MSG* message, bool* paceable, void* context);
    BOOL next(MSG* message, Peek peek, void* context);
    unsigned long long deferred() const { return deferredMoves; }
private:
    MSG held{}, queued{};
    bool holding = false, hasQueued = false;
    unsigned moves = 0;
    unsigned long long deferredMoves = 0;
};

struct MouseCoalescingStats {
    bool active, redirected;
    unsigned long long records, delivered, relayed, deferred;
};
MouseCoalescingStats mouseCoalescingStats();
}
