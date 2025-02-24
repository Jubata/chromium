// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/tabs/tab_experimental.h"

#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/tab_contents/core_tab_helper.h"
#include "chrome/browser/ui/tabs/tab_strip_model_experimental.h"
#include "chrome/browser/ui/view_ids.h"
#include "components/grit/components_scaled_resources.h"
#include "ui/views/border.h"
#include "ui/views/controls/label.h"
#include "ui/views/masked_targeter_delegate.h"
#include "ui/views/widget/widget.h"

namespace {

// Returns the width of the tab endcap in DIP.  More precisely, this is the
// width of the curve making up either the outer or inner edge of the stroke;
// since these two curves are horizontally offset by 1 px (regardless of scale),
// the total width of the endcap from tab outer edge to the inside end of the
// stroke inner edge is (GetUnscaledEndcapWidth() * scale) + 1.
//
// The value returned here must be at least Tab::kMinimumEndcapWidth.
float GetTabEndcapWidth() {
  return GetLayoutInsets(TAB).left() - 0.5f;
}

}  // namespace

TabExperimental::TabExperimental(TabStripModelExperimental* model,
                                 const TabDataExperimental* data)
    : views::View(),
      model_(model),
      data_(data),
      type_(data->type()),
      title_(new views::Label),
      hover_controller_(this),
      paint_(this) {
  title_->SetHorizontalAlignment(gfx::ALIGN_TO_HEAD);
  title_->SetElideBehavior(gfx::FADE_TAIL);
  title_->SetHandlesTooltips(false);
  title_->SetAutoColorReadabilityEnabled(false);
  title_->SetText(CoreTabHelper::GetDefaultTitle());
  AddChildView(title_);

  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));

  // So we get don't get enter/exit on children and don't prematurely stop the
  // hover.
  set_notify_enter_exit_on_child(true);

  set_id(VIEW_ID_TAB);

  // This will cause calls to GetContentsBounds to return only the rectangle
  // inside the tab shape, rather than to its extents.
  SetBorder(views::CreateEmptyBorder(GetLayoutInsets(TAB)));
}

TabExperimental::~TabExperimental() {}

void TabExperimental::SetClosing() {
  closing_ = true;
  data_ = nullptr;
}

void TabExperimental::SetActive(bool active) {
  if (active != active_) {
    active_ = active;
    SchedulePaint();
  }
}

void TabExperimental::SetSelected(bool selected) {
  if (selected != selected_) {
    selected_ = selected;
    SchedulePaint();
  }
}

void TabExperimental::DataUpdated() {
  type_ = data_->type();
  title_->SetText(data_->GetTitle());
}

void TabExperimental::SetGroupLayoutParams(int first_child_begin_x) {
  first_child_begin_x_ = first_child_begin_x;
}

int TabExperimental::GetOverlap() {
  // We want to overlap the endcap portions entirely.
  return gfx::ToCeiledInt(GetTabEndcapWidth());
}

bool TabExperimental::GetHitTestMask(gfx::Path* mask) const {
  // When the window is maximized we don't want to shave off the edges or top
  // shadow of the tab, such that the user can click anywhere along the top
  // edge of the screen to select a tab. Ditto for immersive fullscreen.
  const views::Widget* widget = GetWidget();
  *mask = paint_.GetBorderPath(
      GetWidget()->GetCompositor()->device_scale_factor(), true,
      widget && (widget->IsMaximized() || widget->IsFullscreen()),
      GetTabEndcapWidth(), size());
  return true;
}

void TabExperimental::OnPaint(gfx::Canvas* canvas) {
  if (type_ == TabDataExperimental::Type::kSingle)
    paint_.PaintTabBackground(canvas, active_, 0, 0, nullptr);
  else
    paint_.PaintGroupBackground(canvas, active_);
}

void TabExperimental::Layout() {
  // Space between the favicon and title.
  constexpr int kTitleSpacing = 6;
  const gfx::Rect bounds = GetContentsBounds();

  int title_left = bounds.x() + kTitleSpacing;
  int title_right;
  if (first_child_begin_x_ >= 0)
    title_right = first_child_begin_x_;
  else
    title_right = bounds.width() - kTitleSpacing;

  title_->SetBoundsRect(gfx::Rect(title_left, bounds.y(),
                                  title_right - title_left, bounds.height()));
}

bool TabExperimental::OnMousePressed(const ui::MouseEvent& event) {
  // TODO(brettw) the non-experimental one has some stuff about touch and
  // multi-selection here.
  if (event.IsOnlyLeftMouseButton())
    model_->ActivateTabAt(model_->GetViewIndexForData(data_), true);
  return true;
}

void TabExperimental::OnMouseReleased(const ui::MouseEvent& event) {
  // Close tab on middle click, but only if the button is released over the tab
  // (normal windows behavior is to discard presses of a UI element where the
  // releases happen off the element).
  if (event.IsMiddleMouseButton()) {
    if (HitTestPoint(event.location())) {
      // TODO(brettw) old one did PrepareForCloseAt which does some animation
      // stuff.
      model_->CloseWebContentsAt(
          model_->GetViewIndexForData(data_),
          TabStripModel::CLOSE_USER_GESTURE |
              TabStripModel::CLOSE_CREATE_HISTORICAL_TAB);
    } else if (closing_) {
      // We're animating closed and a middle mouse button was pushed on us but
      // we don't contain the mouse anymore. We assume the user is clicking
      // quicker than the animation and we should close the tab that falls under
      // the mouse.
      /* TODO(brettw) fast closing.
      Tab* closest_tab = controller_->GetTabAt(this, event.location());
      if (closest_tab)
        controller_->CloseTab(closest_tab, CLOSE_TAB_FROM_MOUSE);
      */
    }
  }
}
