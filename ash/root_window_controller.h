// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_ROOT_WINDOW_CONTROLLER_H_
#define ASH_ROOT_WINDOW_CONTROLLER_H_

#include <map>
#include <memory>
#include <vector>

#include "ash/ash_export.h"
#include "ash/public/cpp/shelf_types.h"
#include "ash/wm/workspace/workspace_types.h"
#include "base/macros.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"

namespace aura {
class Window;
}

namespace gfx {
class Point;
}

namespace keyboard {
class KeyboardController;
}

namespace ui {
class EventHandler;
class MenuModel;
class WindowTreeHost;
}

namespace views {
class MenuModelAdapter;
class MenuRunner;
class Widget;
}

namespace wm {
class ScopedCaptureClient;
}

namespace ash {
class AlwaysOnTopController;
class AnimatingWallpaperWidgetController;
class AshTouchExplorationManager;
class AshWindowTreeHost;
class LockScreenActionBackgroundController;
enum class LoginStatus;
class PanelLayoutManager;
class Shelf;
class ShelfLayoutManager;
class StackingController;
class StatusAreaWidget;
class SystemModalContainerLayoutManager;
class SystemTray;
class SystemWallpaperController;
class TouchHudDebug;
class TouchHudProjection;
class WallpaperWidgetController;
class WindowManager;
class WorkspaceController;

namespace wm {
class RootWindowLayoutManager;
}

// This class maintains the per root window state for ash. This class
// owns the root window and other dependent objects that should be
// deleted upon the deletion of the root window. This object is
// indirectly owned and deleted by |WindowTreeHostManager|.
// The RootWindowController for particular root window is stored in
// its property (RootWindowSettings) and can be obtained using
// |RootWindowController::ForWindow(aura::Window*)| function.
class ASH_EXPORT RootWindowController {
 public:
  // Enumerates the type of display. If there is only a single display then
  // it is primary. In a multi-display environment one monitor is deemed the
  // PRIMARY and all others SECONDARY.
  enum class RootWindowType { PRIMARY, SECONDARY };

  ~RootWindowController();

  // Creates and Initialize the RootWindowController for primary display.
  static void CreateForPrimaryDisplay(AshWindowTreeHost* host);

  // Creates and Initialize the RootWindowController for secondary displays.
  static void CreateForSecondaryDisplay(AshWindowTreeHost* host);

  // Returns a RootWindowController of the window's root window.
  static RootWindowController* ForWindow(const aura::Window* window);

  // Returns the RootWindowController of the target root window.
  static RootWindowController* ForTargetRootWindow();

  static std::vector<RootWindowController*> root_window_controllers() {
    return root_window_controllers_ ? *root_window_controllers_
                                    : std::vector<RootWindowController*>();
  }

  // TODO(sky): move these to a separate class or use AshWindowTreeHost in
  // mash. http://crbug.com/671246.
  AshWindowTreeHost* ash_host() { return ash_host_.get(); }
  const AshWindowTreeHost* ash_host() const { return ash_host_.get(); }

  aura::WindowTreeHost* GetHost();
  const aura::WindowTreeHost* GetHost() const;
  aura::Window* GetRootWindow();
  const aura::Window* GetRootWindow() const;

  WorkspaceController* workspace_controller() {
    return workspace_controller_.get();
  }

  wm::WorkspaceWindowState GetWorkspaceWindowState();

  Shelf* shelf() const { return shelf_.get(); }

  // Initializes the shelf for this root window and notifies observers.
  void InitializeShelf();

  // Enables projection touch HUD.
  void SetTouchHudProjectionEnabled(bool enable);

  // Get touch HUDs associated with this root window controller.
  TouchHudDebug* touch_hud_debug() const { return touch_hud_debug_; }
  TouchHudProjection* touch_hud_projection() const {
    return touch_hud_projection_;
  }

  // Set touch HUDs for this root window controller. The root window controller
  // will not own the HUDs; their lifetimes are managed by themselves. Whenever
  // the widget showing a HUD is being destroyed (e.g. because of detaching a
  // display), the HUD deletes itself.
  void set_touch_hud_debug(TouchHudDebug* hud) { touch_hud_debug_ = hud; }
  void set_touch_hud_projection(TouchHudProjection* hud) {
    touch_hud_projection_ = hud;
  }

  PanelLayoutManager* panel_layout_manager() { return panel_layout_manager_; }

  wm::RootWindowLayoutManager* root_window_layout_manager() {
    return root_window_layout_manager_;
  }

  // Access the shelf layout manager associated with this root
  // window controller, NULL if no such shelf exists.
  ShelfLayoutManager* GetShelfLayoutManager();

  // Returns the layout manager for the appropriate modal-container. If the
  // window is inside the lockscreen modal container, then the layout manager
  // for that is returned. Otherwise the layout manager for the default modal
  // container is returned.
  // If no window is specified (i.e. |window| is null), then the lockscreen
  // modal container is used if the screen is currently locked. Otherwise, the
  // default modal container is used.
  SystemModalContainerLayoutManager* GetSystemModalLayoutManager(
      aura::Window* window);

  AlwaysOnTopController* always_on_top_controller() {
    return always_on_top_controller_.get();
  }

  // May return null, for example for a secondary monitor at the login screen.
  StatusAreaWidget* GetStatusAreaWidget();

  // Returns the system tray on this root window. Note that
  // calling this on the root window that doesn't have a shelf will
  // lead to a crash.
  SystemTray* GetSystemTray();

  // True if the window can receive events on this root window.
  bool CanWindowReceiveEvents(aura::Window* window);

  // Returns the window events will be targeted at for the specified location
  // (in screen coordinates).
  //
  // NOTE: the returned window may not contain the location as resize handles
  // may extend outside the bounds of the window.
  aura::Window* FindEventTarget(const gfx::Point& location_in_screen);

  // Gets the last location seen in a mouse event in this root window's
  // coordinates. This may return a point outside the root window's bounds.
  gfx::Point GetLastMouseLocationInRoot();

  aura::Window* GetContainer(int container_id);
  const aura::Window* GetContainer(int container_id) const;

  WallpaperWidgetController* wallpaper_widget_controller() {
    return wallpaper_widget_controller_.get();
  }
  void SetWallpaperWidgetController(WallpaperWidgetController* controller);

  AnimatingWallpaperWidgetController* animating_wallpaper_widget_controller() {
    return animating_wallpaper_widget_controller_.get();
  }
  void SetAnimatingWallpaperWidgetController(
      AnimatingWallpaperWidgetController* controller);

  LockScreenActionBackgroundController*
  lock_screen_action_background_controller() {
    return lock_screen_action_background_controller_.get();
  }

  // Called when the wallpaper animation is finished. Updates
  // |system_wallpaper_| to be black and drops |boot_splash_screen_| and moves
  // the wallpaper controller into the root window controller. |widget| holds
  // the wallpaper image, or NULL if the wallpaper is a solid color.
  void OnWallpaperAnimationFinished(views::Widget* widget);

  // Deletes associated objects and clears the state, but doesn't delete
  // the root window yet. This is used to delete a secondary displays'
  // root window safely when the display disconnect signal is received,
  // which may come while we're in the nested run loop.
  void Shutdown();

  // Deletes all child windows and performs necessary cleanup.
  void CloseChildWindows();

  // Moves child windows to |dest|.
  void MoveWindowsTo(aura::Window* dest);

  // Force the shelf to query for it's current visibility state.
  void UpdateShelfVisibility();

  // Initialize touch HUDs if necessary.
  void InitTouchHuds();

  // Returns the topmost window or one of its transient parents, if any of them
  // are in fullscreen mode.
  aura::Window* GetWindowForFullscreenMode();

  // Activate virtual keyboard on current root window controller.
  void ActivateKeyboard(keyboard::KeyboardController* keyboard_controller);

  // Deactivate virtual keyboard on current root window controller.
  void DeactivateKeyboard(keyboard::KeyboardController* keyboard_controller);

  // Tests if a window is associated with the virtual keyboard.
  bool IsVirtualKeyboardWindow(aura::Window* window);

  // If touch exploration is enabled, update the touch exploration
  // controller so that synthesized touch events are anchored at this point.
  void SetTouchAccessibilityAnchorPoint(const gfx::Point& anchor_point);

  // Shows a context menu at the |location_in_screen|.
  void ShowContextMenu(const gfx::Point& location_in_screen,
                       ui::MenuSourceType source_type);

  // Called when the login status changes after login (such as lock/unlock).
  void UpdateAfterLoginStatusChange(LoginStatus status);

 private:
  // TODO(sky): remove this. Temporary during ash-mus unification.
  // http://crbug.com/671246.
  friend class WindowManager;

  // Creates a new RootWindowController with the specified host. Only one of
  // |ash_host| or |window_tree_host| should be specified. This takes ownership
  // of the supplied arguments.
  // TODO(sky): mash should create AshWindowTreeHost, http://crbug.com/671246.
  RootWindowController(AshWindowTreeHost* ash_host,
                       aura::WindowTreeHost* window_tree_host);

  // Initializes the RootWindowController based on |root_window_type|.
  void Init(RootWindowType root_window_type);

  void InitLayoutManagers();

  // Creates the containers (aura::Windows) used by the shell.
  void CreateContainers();

  // Initializes |system_wallpaper_| and possibly also |boot_splash_screen_|.
  // The initial color is determined by the |root_window_type| and whether or
  // not this is the first boot.
  void CreateSystemWallpaper(RootWindowType root_window_type);

  // Resets Shell::GetRootWindowForNewWindows() if appropriate. This is called
  // during shutdown to make sure GetRootWindowForNewWindows() isn't referencing
  // this.
  void ResetRootForNewWindowsIfNecessary();

  // Callback for MenuModelAdapter.
  void OnMenuClosed();

  std::unique_ptr<AshWindowTreeHost> ash_host_;
  std::unique_ptr<aura::WindowTreeHost> mus_window_tree_host_;
  // This comes from |ash_host_| or |mus_window_tree_host_|.
  aura::WindowTreeHost* window_tree_host_;

  // LayoutManagers are owned by the window they are installed on.
  PanelLayoutManager* panel_layout_manager_ = nullptr;
  wm::RootWindowLayoutManager* root_window_layout_manager_ = nullptr;

  std::unique_ptr<WallpaperWidgetController> wallpaper_widget_controller_;
  std::unique_ptr<AnimatingWallpaperWidgetController>
      animating_wallpaper_widget_controller_;
  std::unique_ptr<WorkspaceController> workspace_controller_;

  std::unique_ptr<AlwaysOnTopController> always_on_top_controller_;

  // Manages the context menu.
  std::unique_ptr<ui::MenuModel> menu_model_;
  std::unique_ptr<views::MenuModelAdapter> menu_model_adapter_;
  std::unique_ptr<views::MenuRunner> menu_runner_;

  std::unique_ptr<StackingController> stacking_controller_;

  // The shelf controller for this root window. Exists for the entire lifetime
  // of the RootWindowController so that it is safe for observers to be added
  // to it during construction of the shelf widget and status tray.
  std::unique_ptr<Shelf> shelf_;

  // TODO(jamescook): Eliminate this. It is left over from legacy shelf code and
  // doesn't mean anything in particular.
  bool shelf_initialized_ = false;

  std::unique_ptr<SystemWallpaperController> system_wallpaper_;

  // Responsible for initializing TouchExplorationController when spoken
  // feedback is on.
  std::unique_ptr<AshTouchExplorationManager> touch_exploration_manager_;

  // Heads-up displays for touch events. These HUDs are not owned by the root
  // window controller and manage their own lifetimes.
  TouchHudDebug* touch_hud_debug_ = nullptr;
  TouchHudProjection* touch_hud_projection_ = nullptr;

  // Handles double clicks on the panel window header.
  std::unique_ptr<ui::EventHandler> panel_container_handler_;

  std::unique_ptr<::wm::ScopedCaptureClient> capture_client_;

  std::unique_ptr<LockScreenActionBackgroundController>
      lock_screen_action_background_controller_;

  static std::vector<RootWindowController*>* root_window_controllers_;

  DISALLOW_COPY_AND_ASSIGN(RootWindowController);
};

}  // namespace ash

#endif  // ASH_ROOT_WINDOW_CONTROLLER_H_
