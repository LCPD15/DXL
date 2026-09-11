// Runs against production ReUiBackend.cpp in both D3D11 and D3D12 fixtures.
// No system input or raw-device registration is synthesized or replaced.
static void TestCursorFallback() {
    CursorFallback normal;
    for (int i = 0; i < 200; ++i) {
        const POINT real{100 + i, 100 + i};
        const POINT actual = normal.Update(real, 1600, 900, {1, 1, false}, 1000 + i * 8);
        Check(!normal.active && actual.x == real.x && actual.y == real.y,
              "ordinary slow cursor incorrectly switched to raw mode");
    }
    CursorFallback pinned;
    POINT actual{};
    for (unsigned i = 0; i < 5; ++i)
        actual = pinned.Update({800, 450}, 1600, 900, {12, -3, false}, 1000 + i * 16);
    Check(pinned.active && actual.x == 860 && actual.y == 435,
          "pinned cursor did not preserve accumulated relative movement");
    actual = pinned.Update({100, 100}, 1600, 900, {10, 4, false}, 1100);
    Check(pinned.active && actual.x == 870 && actual.y == 439,
          "native recenter caused virtual cursor jump");
    actual = pinned.Update({100, 100}, 1600, 900, {1000000, -1000000, false}, 1116);
    Check(actual.x == 1599 && actual.y == 0, "virtual cursor escaped window bounds");
    pinned.Update({100, 100}, 1600, 900, {0, 0, true}, 1132);
    Check(!pinned.active && pinned.absolute, "tablet/RDP absolute mouse integrated as delta");
    pinned.Reset();
    actual = pinned.Update({200, 220}, 1600, 900, {}, 1200);
    Check(!pinned.active && !pinned.absolute && actual.x == 200 && actual.y == 220,
          "panel reopen retained previous virtual cursor session");
    CursorFallback border;
    for (unsigned i = 0; i < 30; ++i)
        border.Update({1599, 450}, 1600, 900, {12, 0, false}, 1000 + i * 16);
    Check(!border.active, "normal window boundary incorrectly triggered pin fallback");
    CursorFallback intermittent;
    for (unsigned i = 0; i < 30; ++i)
        intermittent.Update({800, 450}, 1600, 900, {12, 0, false}, 1000 + i * 300);
    Check(!intermittent.active, "isolated old deltas accumulated into false pin detection");
    ConsumeRawMouse();
    RAWMOUSE relative{}; relative.lLastX = 7; relative.lLastY = -9;
    QueueRelativeMouse(relative); QueueRelativeMouse(relative);
    const RawMouseDelta raw = ConsumeRawMouse();
    Check(raw.x == 14 && raw.y == -18 && !raw.absolute, "raw mouse producer lost motion");
    const RawMouseDelta empty = ConsumeRawMouse();
    Check(empty.x == 0 && empty.y == 0, "raw mouse delta applied more than once");
    relative.usFlags = MOUSE_MOVE_ABSOLUTE; relative.lLastX = 65000;
    QueueRelativeMouse(relative);
    const RawMouseDelta absolute = ConsumeRawMouse();
    Check(absolute.absolute && absolute.x == 0 && absolute.y == 0, "raw absolute coordinate overflow");
    std::atomic<bool> open{true};
    ReUi::SetUiOpen(&open);
    g.virtualCursor = true;
    cursorFallback.active = true;
    open = false;
    ReUi::SetUiOpen(&open); // no FramePresent on closed/no-toast frames
    Check(!g.virtualCursor && !cursorFallback.session && !g.inputSessionOpen,
          "panel close without a render callback retained cursor ownership");
    ReUi::SetUiOpen(nullptr);
    printf("PASS: adaptive raw cursor normal/pinned/recenter/bounds/absolute/reopen/queue regression\n");
}
