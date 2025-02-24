// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_RENDERER_HOST_RENDER_WIDGET_HOST_VIEW_EVENT_HANDLER_H_
#define CONTENT_BROWSER_RENDERER_HOST_RENDER_WIDGET_HOST_VIEW_EVENT_HANDLER_H_

#include <memory>

#include "base/macros.h"
#include "content/browser/renderer_host/input/mouse_wheel_phase_handler.h"
#include "content/common/content_export.h"
#include "content/public/browser/native_web_keyboard_event.h"
#include "ui/aura/window_tracker.h"
#include "ui/events/event_handler.h"
#include "ui/events/gestures/motion_event_aura.h"
#include "ui/latency/latency_info.h"

namespace aura {
class Window;
}  // namespace aura

namespace blink {
class WebMouseEvent;
class WebMouseWheelEvent;
class WebTouchEvent;
}  // namespace blink

namespace ui {
class TextInputClient;
class TouchSelectionController;
}

namespace content {

struct ContextMenuParams;
class OverscrollController;
class RenderWidgetHostImpl;
class RenderWidgetHostViewBase;
class TouchSelectionControllerClientAura;

// Provides an implementation of ui::EventHandler for use with
// RenderWidgetHostViewBase. A delegate is required in order to provide platform
// specific functionality.
//
// After processing events they will be forwarded to the provided
// RenderWidgetHostImpl.
//
// This does not implement ui::TextInputClient, which some
// RenderWidgetHostViewBase classes do.
// RenderWidgetHostViewEventHandler::Delegate implementations may have
// overlapping functionality with the ui::TextInputClient.
class CONTENT_EXPORT RenderWidgetHostViewEventHandler
    : public ui::EventHandler {
 public:
  // An interface to provide platform specific logic needed for event handling.
  class Delegate {
   public:
    Delegate();

    // Converts |rect| from window coordinate to screen coordinate.
    virtual gfx::Rect ConvertRectToScreen(const gfx::Rect& rect) const = 0;
    // Call keybindings handler against the event and send matched edit commands
    // to the renderer instead. |update_event| (if non-null) is set to indicate
    // whether ui::KeyEvent::SetHandled() should be called on the underlying
    // ui::KeyEvent.
    virtual void ForwardKeyboardEventWithLatencyInfo(
        const NativeWebKeyboardEvent& event,
        const ui::LatencyInfo& latency,
        bool* update_event) = 0;
    // Returns whether the widget needs to grab mouse capture to work properly.
    virtual bool NeedsMouseCapture() = 0;
    virtual void SetTooltipsEnabled(bool enable) = 0;
    virtual void ShowContextMenu(const ContextMenuParams& params) = 0;
    // Sends shutdown request.
    virtual void Shutdown() = 0;

    ui::TouchSelectionController* selection_controller() const {
      return selection_controller_.get();
    }

    TouchSelectionControllerClientAura* selection_controller_client() const {
      return selection_controller_client_.get();
    }

    OverscrollController* overscroll_controller() const {
      return overscroll_controller_.get();
    }

   protected:
    virtual ~Delegate();

    std::unique_ptr<TouchSelectionControllerClientAura>
        selection_controller_client_;
    std::unique_ptr<ui::TouchSelectionController> selection_controller_;
    std::unique_ptr<OverscrollController> overscroll_controller_;

   private:
    DISALLOW_COPY_AND_ASSIGN(Delegate);
  };

  RenderWidgetHostViewEventHandler(RenderWidgetHostImpl* host,
                                   RenderWidgetHostViewBase* host_view,
                                   Delegate* delegate);
  ~RenderWidgetHostViewEventHandler() override;

  // Set child popup's host view, and event handler, in order to redirect input.
  void SetPopupChild(RenderWidgetHostViewBase* popup_child_host_view,
                     ui::EventHandler* popup_child_event_handler);
  // Begin tracking a host window, such as when RenderWidgetHostViewBase is
  // fullscreen.
  void TrackHost(aura::Window* reference_window);

  MouseWheelPhaseHandler& mouse_wheel_phase_handler() {
    return mouse_wheel_phase_handler_;
  }

#if defined(OS_WIN)
  // Sets the ContextMenuParams when a context menu is triggered. Required for
  // subsequent event processing.
  void SetContextMenuParams(const ContextMenuParams& params);

  // Updates the cursor clip region. Used for mouse locking.
  void UpdateMouseLockRegion();
#endif  // defined(OS_WIN)

  bool accept_return_character() { return accept_return_character_; }
  void disable_input_event_router_for_testing() {
    disable_input_event_router_for_testing_ = true;
  }
  bool mouse_locked() { return mouse_locked_; }
  const ui::MotionEventAura& pointer_state() const { return pointer_state_; }
  void set_focus_on_mouse_down_or_key_event(
      bool focus_on_mouse_down_or_key_event) {
    set_focus_on_mouse_down_or_key_event_ = focus_on_mouse_down_or_key_event;
  }
  void set_window(aura::Window* window) { window_ = window; }

  // Lock/Unlock processing of future mouse events.
  bool LockMouse();
  void UnlockMouse();

  // ui::EventHandler:
  void OnKeyEvent(ui::KeyEvent* event) override;
  void OnMouseEvent(ui::MouseEvent* event) override;
  void OnScrollEvent(ui::ScrollEvent* event) override;
  void OnTouchEvent(ui::TouchEvent* event) override;
  void OnGestureEvent(ui::GestureEvent* event) override;

 private:
  FRIEND_TEST_ALL_PREFIXES(InputMethodResultAuraTest,
                           FinishImeCompositionSession);
  // Returns true if the |event| passed in can be forwarded to the renderer.
  bool CanRendererHandleEvent(const ui::MouseEvent* event,
                              bool mouse_locked,
                              bool selection_popup) const;

  // Confirm existing composition text in the webpage and ask the input method
  // to cancel its ongoing composition session.
  void FinishImeCompositionSession();

  // Forwards a mouse event to this view's parent window delegate.
  void ForwardMouseEventToParent(ui::MouseEvent* event);

  // Performs gesture handling needed for touch text selection. Sets event as
  // handled if it should not be further processed.
  void HandleGestureForTouchSelection(ui::GestureEvent* event);

  // Handles mouse event handling while the mouse is locked via LockMouse.
  void HandleMouseEventWhileLocked(ui::MouseEvent* event);

  // This method computes movementX/Y and keeps track of mouse location for
  // mouse lock on all mouse move events.
  // |ui_mouse_event| contains the mouse event received.
  // |event| contains the WebMouseEvent being modified.
  void ModifyEventMovementAndCoords(const ui::MouseEvent& ui_mouse_event,
                                    blink::WebMouseEvent* event);

  // Helper function to set keyboard focus to the main window.
  void SetKeyboardFocus();

  // Helper method to determine if, in mouse locked mode, the cursor should be
  // moved to center.
  bool ShouldMoveToCenter();

  // Returns true when we can do SurfaceHitTesting for the event type.
  bool ShouldRouteEvent(const ui::Event* event) const;

  // Directs events to the |host_|.
  void ProcessMouseEvent(const blink::WebMouseEvent& event,
                         const ui::LatencyInfo& latency);
  void ProcessMouseWheelEvent(const blink::WebMouseWheelEvent& event,
                              const ui::LatencyInfo& latency);
  void ProcessTouchEvent(const blink::WebTouchEvent& event,
                         const ui::LatencyInfo& latency);

  // Whether return characters should be passed on to the RenderWidgetHostImpl.
  bool accept_return_character_;

  // Allows tests to send gesture events for testing without first sending a
  // corresponding touch sequence, as would be required by
  // RenderWidgetHostInputEventRouter.
  bool disable_input_event_router_for_testing_;

  // While the mouse is locked, the cursor is hidden from the user. Mouse events
  // are still generated. However, the position they report is the last known
  // mouse position just as mouse lock was entered; the movement they report
  // indicates what the change in position of the mouse would be had it not been
  // locked.
  bool mouse_locked_;

  // Whether pinch-to-zoom should be enabled and pinch events forwarded to the
  // renderer.
  bool pinch_zoom_enabled_;

  // This flag when set ensures that we send over a notification to blink that
  // the current view has focus. Defaults to false.
  bool set_focus_on_mouse_down_or_key_event_;

  // Used to track the state of the window we're created from. Only used when
  // created fullscreen.
  std::unique_ptr<aura::WindowTracker> host_tracker_;

  // Used to record the last position of the mouse.
  // While the mouse is locked, they store the last known position just as mouse
  // lock was entered.
  // Relative to the upper-left corner of the view.
  gfx::PointF unlocked_mouse_position_;
  // Relative to the upper-left corner of the screen.
  gfx::PointF unlocked_global_mouse_position_;
  // Last cursor position relative to screen. Used to compute movementX/Y.
  gfx::PointF global_mouse_position_;
  // In mouse locked mode, we synthetically move the mouse cursor to the center
  // of the window when it reaches the window borders to avoid it going outside.
  // This flag is used to differentiate between these synthetic mouse move
  // events vs. normal mouse move events.
  bool synthetic_move_sent_;
  // Stores the current state of the active pointers targeting this
  // object.
  ui::MotionEventAura pointer_state_;

#if defined(OS_WIN)
  // Contains a copy of the last context menu request parameters. Only set when
  // we receive a request to show the context menu on a long press.
  std::unique_ptr<ContextMenuParams> last_context_menu_params_;
#endif  // defined(OS_WIN)

  // The following are not owned. They should outlive |this|
  RenderWidgetHostImpl* const host_;
  // Should create |this| and own it.
  RenderWidgetHostViewBase* const host_view_;
  // Optional, used to redirect events to a popup and associated handler.
  RenderWidgetHostViewBase* popup_child_host_view_;
  ui::EventHandler* popup_child_event_handler_;
  Delegate* const delegate_;
  aura::Window* window_;
  MouseWheelPhaseHandler mouse_wheel_phase_handler_;

  DISALLOW_COPY_AND_ASSIGN(RenderWidgetHostViewEventHandler);
};

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_RENDER_WIDGET_HOST_VIEW_EVENT_HANDLER_H_
