#include "InputManager.hpp"
#include "../../Compositor.hpp"

// workaround so that emulated swipes aren't too small to be detected
constexpr int touchSwipeFactor = 500;

void          CInputManager::onTouchDown(wlr_touch_down_event* e) {
    static auto* const PSWIPE        = &g_pConfigManager->getConfigValuePtr("gestures:workspace_swipe")->intValue;
    static auto* const PSWIPEFINGERS = &g_pConfigManager->getConfigValuePtr("gestures:workspace_swipe_fingers")->intValue;

    auto               PMONITOR = g_pCompositor->getMonitorFromName(e->touch->output_name ? e->touch->output_name : "");

    const auto         PDEVIT = std::find_if(m_lTouchDevices.begin(), m_lTouchDevices.end(), [&](const STouchDevice& other) { return other.pWlrDevice == &e->touch->base; });

    if (PDEVIT != m_lTouchDevices.end() && !PDEVIT->boundOutput.empty())
        PMONITOR = g_pCompositor->getMonitorFromName(PDEVIT->boundOutput);

    PMONITOR = PMONITOR ? PMONITOR : g_pCompositor->m_pLastMonitor;

    wlr_cursor_warp(g_pCompositor->m_sWLRCursor, nullptr, PMONITOR->vecPosition.x + e->x * PMONITOR->vecSize.x, PMONITOR->vecPosition.y + e->y * PMONITOR->vecSize.y);

    refocus();

    if (m_ecbClickBehavior == CLICKMODE_KILL) {
        wlr_pointer_button_event e;
        e.state = WLR_BUTTON_PRESSED;
        g_pInputManager->processMouseDownKill(&e);
        return;
    }

    m_bLastInputTouch = true;

    m_sTouchData.touchFocusWindow  = m_pFoundWindowToFocus;
    m_sTouchData.touchFocusSurface = m_pFoundSurfaceToFocus;
    m_sTouchData.touchFocusLS      = m_pFoundLSToFocus;

    SFingerData finger;

    m_vTouchGestureLastCenter = Vector2D(e->x, e->y);
    finger                    = SFingerData{
                                    .type = 0,
                                    .id   = e->touch_id,
                                    .time = e->time_msec,
                                    .pos  = m_vTouchGestureLastCenter,
    };
    m_lFingers.push_back(finger);

    if (*PSWIPE && (long)m_lFingers.size() == *PSWIPEFINGERS) {
        if (m_sActiveSwipe.pMonitor) { // emulated swipe will also interfere with real swipe events
            emulateSwipeEnd(e->time_msec, true);
        }
        emulateSwipeBegin(e->time_msec, m_lFingers.size());
        return;
    }

    Vector2D local;

    if (m_sTouchData.touchFocusWindow) {
        if (m_sTouchData.touchFocusWindow->m_bIsX11) {
            local = g_pInputManager->getMouseCoordsInternal() - m_sTouchData.touchFocusWindow->m_vRealPosition.goalv();
        } else {
            g_pCompositor->vectorWindowToSurface(g_pInputManager->getMouseCoordsInternal(), m_sTouchData.touchFocusWindow, local);
        }

        m_sTouchData.touchSurfaceOrigin = g_pInputManager->getMouseCoordsInternal() - local;
    } else if (m_sTouchData.touchFocusLS) {
        local = g_pInputManager->getMouseCoordsInternal() - Vector2D(m_sTouchData.touchFocusLS->geometry.x, m_sTouchData.touchFocusLS->geometry.y) -
            g_pCompositor->m_pLastMonitor->vecPosition;

        m_sTouchData.touchSurfaceOrigin = g_pInputManager->getMouseCoordsInternal() - local;
    } else {
        return; // oops, nothing found.
    }

    wlr_seat_touch_notify_down(g_pCompositor->m_sSeat.seat, m_sTouchData.touchFocusSurface, e->time_msec, e->touch_id, local.x, local.y);

    wlr_idle_notify_activity(g_pCompositor->m_sWLRIdle, g_pCompositor->m_sSeat.seat);
}

void CInputManager::onTouchUp(wlr_touch_up_event* e) {
    m_lFingers.remove_if([&e](SFingerData finger) { return finger.id == e->touch_id; });

    if (m_bTouchGestureActive) {
        emulateSwipeEnd(e->time_msec, false);
        return;
    }

    if (m_sTouchData.touchFocusSurface) {
        wlr_seat_touch_notify_up(g_pCompositor->m_sSeat.seat, e->time_msec, e->touch_id);
    }
}

void CInputManager::onTouchMove(wlr_touch_motion_event* e) {
    auto finger = std::find_if(m_lFingers.begin(), m_lFingers.end(), [&](SFingerData finger) { return finger.id == e->touch_id; });
    if (finger == m_lFingers.end()) {
        Debug::log(ERR, "Touch ID not found!");
        return;
    }
    finger->type = 1;
    finger->time = e->time_msec;
    finger->pos  = Vector2D(e->x, e->y);

    if (m_bTouchGestureActive) {
        emulateSwipeUpdate(e->time_msec, m_lFingers.size());
        return;
    }

    if (m_sTouchData.touchFocusWindow && g_pCompositor->windowValidMapped(m_sTouchData.touchFocusWindow)) {
        const auto PMONITOR = g_pCompositor->getMonitorFromID(m_sTouchData.touchFocusWindow->m_iMonitorID);

        wlr_cursor_warp(g_pCompositor->m_sWLRCursor, nullptr, PMONITOR->vecPosition.x + e->x * PMONITOR->vecSize.x,
                        PMONITOR->vecPosition.y + e->y * PMONITOR->vecSize.y);

        const auto local = g_pInputManager->getMouseCoordsInternal() - m_sTouchData.touchSurfaceOrigin;

        wlr_seat_touch_notify_motion(g_pCompositor->m_sSeat.seat, e->time_msec, e->touch_id, local.x, local.y);
        wlr_seat_pointer_notify_motion(g_pCompositor->m_sSeat.seat, e->time_msec, local.x, local.y);
    } else if (m_sTouchData.touchFocusLS) {
        const auto PMONITOR = g_pCompositor->getMonitorFromID(m_sTouchData.touchFocusLS->monitorID);

        wlr_cursor_warp(g_pCompositor->m_sWLRCursor, nullptr, PMONITOR->vecPosition.x + e->x * PMONITOR->vecSize.x,
                        PMONITOR->vecPosition.y + e->y * PMONITOR->vecSize.y);

        const auto local = g_pInputManager->getMouseCoordsInternal() - m_sTouchData.touchSurfaceOrigin;

        wlr_seat_touch_notify_motion(g_pCompositor->m_sSeat.seat, e->time_msec, e->touch_id, local.x, local.y);
        wlr_seat_pointer_notify_motion(g_pCompositor->m_sSeat.seat, e->time_msec, local.x, local.y);
    }
}

void CInputManager::emulateSwipeBegin(uint32_t time, uint32_t fingers) {
    auto emulated_swipe = wlr_pointer_swipe_begin_event{.pointer = nullptr, .time_msec = time, .fingers = fingers};
    onSwipeBegin(&emulated_swipe);
    m_bTouchGestureActive = true;
    return;
}

void CInputManager::emulateSwipeEnd(uint32_t time, bool cancelled) {
    auto emulated_swipe = wlr_pointer_swipe_end_event{.pointer = nullptr, .time_msec = time, .cancelled = cancelled};
    onSwipeEnd(&emulated_swipe);
    m_bTouchGestureActive = false;
    return;
}

void CInputManager::emulateSwipeUpdate(uint32_t time, uint32_t fingers /*TODO remove?*/) {
    // static auto* const PSWIPEDIST = &g_pConfigManager->getConfigValuePtr("gestures:workspace_swipe_distance")->intValue;
    const bool         VERTANIMS  = m_sActiveSwipe.pWorkspaceBegin->m_vRenderOffset.getConfig()->pValues->internalStyle == "slidevert";

    if (!m_sActiveSwipe.pMonitor) {
        Debug::log(ERR, "ignoring touch gesture motion event due to missing monitor!");
        return;
    }

    // touch coords are within 0 to 1, we need to scale it with screen width/height (should consider scaling) and
    // divide by PSWIPEDIST to get one to one gestures
    const double swipeFactor = (VERTANIMS ? m_sActiveSwipe.pMonitor->vecTransformedSize.y : m_sActiveSwipe.pMonitor->vecTransformedSize.x);

    Vector2D     currentCenter;
    for (auto finger : m_lFingers) {
        currentCenter = currentCenter + finger.pos;
    }
    currentCenter = currentCenter / m_lFingers.size();

    auto emulated_swipe = wlr_pointer_swipe_update_event{.pointer   = nullptr,
                                                         .time_msec = time,
                                                         .fingers   = m_lFingers.size(),
                                                         .dx        = (currentCenter.x - m_vTouchGestureLastCenter.x) * swipeFactor,
                                                         .dy        = (currentCenter.y - m_vTouchGestureLastCenter.y) * swipeFactor};

    onSwipeUpdate(&emulated_swipe);
    m_vTouchGestureLastCenter = currentCenter;
}

void CInputManager::onPointerHoldBegin(wlr_pointer_hold_begin_event* e) {
    wlr_pointer_gestures_v1_send_hold_begin(g_pCompositor->m_sWLRPointerGestures, g_pCompositor->m_sSeat.seat, e->time_msec, e->fingers);
}

void CInputManager::onPointerHoldEnd(wlr_pointer_hold_end_event* e) {
    wlr_pointer_gestures_v1_send_hold_end(g_pCompositor->m_sWLRPointerGestures, g_pCompositor->m_sSeat.seat, e->time_msec, e->cancelled);
}
