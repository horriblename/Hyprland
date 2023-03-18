#include "InputManager.hpp"
#include "../../Compositor.hpp"

void CInputManager::onTouchDown(wlr_touch_down_event* e) {
    auto       PMONITOR = g_pCompositor->getMonitorFromName(e->touch->output_name ? e->touch->output_name : "");

    const auto PDEVIT = std::find_if(m_lTouchDevices.begin(), m_lTouchDevices.end(), [&](const STouchDevice& other) { return other.pWlrDevice == &e->touch->base; });

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

    wf::touch::gesture_event_t gesture_event = {.type = wf::touch::EVENT_TYPE_TOUCH_DOWN, .time = e->time_msec, .finger = e->touch_id, .pos = {e->x, e->y}};

    m_sFingerState.update(gesture_event);
    updateGestures(gesture_event);

    m_sTouchData.touchFocusWindow  = m_pFoundWindowToFocus;
    m_sTouchData.touchFocusSurface = m_pFoundSurfaceToFocus;
    m_sTouchData.touchFocusLS      = m_pFoundLSToFocus;

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
    const auto finger = m_sFingerState.fingers.find(e->touch_id);
    if (finger == m_sFingerState.fingers.end()) {
        // idk what horrible thing has to happen for this to occur, but checking regardless
        Debug::log(WARN, "could not find finger of id %d in m_sFingerState", e->touch_id);
        return;
    }

    const wf::touch::gesture_event_t gesture_event = {
        .type   = wf::touch::EVENT_TYPE_TOUCH_UP,
        .time   = e->time_msec,
        .finger = e->touch_id,
        .pos    = finger->second.current,
    };

    updateGestures(gesture_event);
    m_sFingerState.update(gesture_event);

    if (m_sTouchData.touchFocusSurface) {
        wlr_seat_touch_notify_up(g_pCompositor->m_sSeat.seat, e->time_msec, e->touch_id);
    }
}

void CInputManager::onTouchMove(wlr_touch_motion_event* e) {
    const wf::touch::gesture_event_t gesture_event = {.type = wf::touch::EVENT_TYPE_MOTION, .time = e->time_msec, .finger = e->touch_id, .pos = {e->x, e->y}};
    updateGestures(gesture_event);
    m_sFingerState.update(gesture_event);

    if (m_sTouchData.touchFocusWindow && g_pCompositor->windowValidMapped(m_sTouchData.touchFocusWindow)) {
        const auto PMONITOR = g_pCompositor->getMonitorFromID(m_sTouchData.touchFocusWindow->m_iMonitorID);

        wlr_cursor_warp(g_pCompositor->m_sWLRCursor, g_pCompositor->m_sSeat.mouse->mouse, PMONITOR->vecPosition.x + e->x * PMONITOR->vecSize.x,
                        PMONITOR->vecPosition.y + e->y * PMONITOR->vecSize.y);

        const auto local = g_pInputManager->getMouseCoordsInternal() - m_sTouchData.touchSurfaceOrigin;

        wlr_seat_touch_notify_motion(g_pCompositor->m_sSeat.seat, e->time_msec, e->touch_id, local.x, local.y);
        // wlr_seat_pointer_notify_motion(g_pCompositor->m_sSeat.seat, e->time_msec, local.x, local.y);
    } else if (m_sTouchData.touchFocusLS) {
        const auto PMONITOR = g_pCompositor->getMonitorFromID(m_sTouchData.touchFocusLS->monitorID);

        wlr_cursor_warp(g_pCompositor->m_sWLRCursor, g_pCompositor->m_sSeat.mouse->mouse, PMONITOR->vecPosition.x + e->x * PMONITOR->vecSize.x,
                        PMONITOR->vecPosition.y + e->y * PMONITOR->vecSize.y);

        const auto local = g_pInputManager->getMouseCoordsInternal() - m_sTouchData.touchSurfaceOrigin;

        wlr_seat_touch_notify_motion(g_pCompositor->m_sSeat.seat, e->time_msec, e->touch_id, local.x, local.y);
        // wlr_seat_pointer_notify_motion(g_pCompositor->m_sSeat.seat, e->time_msec, local.x, local.y);
    }
}

void CInputManager::onPointerHoldBegin(wlr_pointer_hold_begin_event* e) {
    wlr_pointer_gestures_v1_send_hold_begin(g_pCompositor->m_sWLRPointerGestures, g_pCompositor->m_sSeat.seat, e->time_msec, e->fingers);
}

void CInputManager::onPointerHoldEnd(wlr_pointer_hold_end_event* e) {
    wlr_pointer_gestures_v1_send_hold_end(g_pCompositor->m_sWLRPointerGestures, g_pCompositor->m_sSeat.seat, e->time_msec, e->cancelled);
}

void CInputManager::updateGestures(const wf::touch::gesture_event_t& e) {
    Debug::log(LOG, "updateGestures called");
    // FIXME obv not good
    // none of the onSwipeXxx functions use wlr_pointer_swip_xxx_event.pointer so it prolly won't crash
    switch (e.type) {
        case wf::touch::EVENT_TYPE_TOUCH_DOWN: {
            wlr_pointer_swipe_begin_event emulated_swipe = wlr_pointer_swipe_begin_event{.pointer = nullptr, .time_msec = e.time, .fingers = m_sFingerState.fingers.size()};
            m_vTouchGestureLastCenter                    = Vector2D(m_sFingerState.get_center().origin.x, m_sFingerState.get_center().origin.y);
            onSwipeBegin(&emulated_swipe);
            break;
        }
        case wf::touch::EVENT_TYPE_MOTION: {
            auto                           currentCenter = Vector2D(m_sFingerState.get_center().current.x, m_sFingerState.get_center().current.y);

            wlr_pointer_swipe_update_event emulated_swipe = wlr_pointer_swipe_update_event{.pointer   = nullptr,
                                                                                           .time_msec = e.time,
                                                                                           .fingers   = m_sFingerState.fingers.size(),
                                                                                           .dx        = currentCenter.x - m_vTouchGestureLastCenter.x,
                                                                                           .dy        = currentCenter.y - m_vTouchGestureLastCenter.y};
            onSwipeUpdate(&emulated_swipe);
            m_vTouchGestureLastCenter = currentCenter;
            break;
        }
        case wf::touch::EVENT_TYPE_TOUCH_UP: {
            // TODO maybe reset m_vTouchGestureLastCenter and handle error if motion event received with invalid last center
            wlr_pointer_swipe_end_event emulated_swipe = wlr_pointer_swipe_end_event{.pointer = nullptr, .time_msec = e.time, .cancelled = false};
            onSwipeEnd(&emulated_swipe);
            break;
        };
    }

    // FIXME I have no clue what this does
    for (auto& gesture : m_cGestures) {
        if ((m_sFingerState.fingers.size() == 1) && (e.type == wf::touch::EVENT_TYPE_TOUCH_DOWN)) {
            gesture.reset(e.time);
        }

        gesture.update_state(e);
    }
};
