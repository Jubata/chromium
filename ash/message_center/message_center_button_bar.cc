// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/message_center/message_center_button_bar.h"

#include "ash/message_center/message_center_style.h"
#include "ash/message_center/message_center_view.h"
#include "ash/resources/vector_icons/vector_icons.h"
#include "ash/strings/grit/ash_strings.h"
#include "base/macros.h"
#include "build/build_config.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/simple_menu_model.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/text_constants.h"
#include "ui/message_center/message_center.h"
#include "ui/message_center/notifier_id.h"
#include "ui/message_center/public/cpp/message_center_constants.h"
#include "ui/resources/grit/ui_resources.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/button/menu_button.h"
#include "ui/views/controls/button/menu_button_listener.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/separator.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/grid_layout.h"
#include "ui/views/painter.h"

using message_center::MessageCenter;

namespace ash {

namespace {

constexpr SkColor kTextColor = SkColorSetARGB(0xFF, 0x0, 0x0, 0x0);
constexpr SkColor kButtonSeparatorColor = SkColorSetARGB(0x1F, 0x0, 0x0, 0x0);
constexpr int kTextFontSize = 14;
constexpr int kSeparatorHeight = 24;
constexpr gfx::Insets kSeparatorPadding(12, 0, 12, 0);
constexpr gfx::Insets kButtonBarBorder(4, 18, 4, 0);

void SetDefaultButtonStyle(views::Button* button) {
  button->SetFocusForPlatform();
  button->SetFocusPainter(views::Painter::CreateSolidFocusPainter(
      message_center::kFocusBorderColor, gfx::Insets(1, 2, 2, 2)));
  button->SetBorder(
      views::CreateEmptyBorder(message_center_style::kActionIconPadding));

  // TODO(tetsui): Add ripple effect to the buttons.
}

views::Separator* CreateVerticalSeparator() {
  views::Separator* separator = new views::Separator;
  separator->SetPreferredHeight(kSeparatorHeight);
  separator->SetColor(kButtonSeparatorColor);
  separator->SetBorder(views::CreateEmptyBorder(kSeparatorPadding));
  return separator;
}

}  // namespace

// MessageCenterButtonBar /////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
MessageCenterButtonBar::MessageCenterButtonBar(
    MessageCenterView* message_center_view,
    MessageCenter* message_center,
    bool settings_initially_visible,
    const base::string16& title)
    : message_center_view_(message_center_view),
      message_center_(message_center),
      notification_label_(nullptr),
      button_container_(nullptr),
      close_all_button_(nullptr),
      settings_button_(nullptr),
      quiet_mode_button_(nullptr) {
  SetPaintToLayer();
  SetBackground(
      views::CreateSolidBackground(message_center_style::kBackgroundColor));
  views::BoxLayout* layout =
      new views::BoxLayout(views::BoxLayout::kHorizontal);
  SetLayoutManager(layout);
  SetBorder(views::CreateEmptyBorder(kButtonBarBorder));

  notification_label_ = new views::Label(title);
  notification_label_->SetAutoColorReadabilityEnabled(false);
  notification_label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  notification_label_->SetEnabledColor(kTextColor);
  // "Roboto-Medium, 14sp" is specified in the mock.
  notification_label_->SetFontList(
      message_center_style::GetFontListForSizeAndWeight(
          kTextFontSize, gfx::Font::Weight::MEDIUM));
  AddChildView(notification_label_);
  layout->SetFlexForView(notification_label_, 1);

  button_container_ = new views::View;
  button_container_->SetLayoutManager(
      new views::BoxLayout(views::BoxLayout::kHorizontal));
  close_all_button_ = new views::ImageButton(this);
  close_all_button_->SetImage(
      views::Button::STATE_NORMAL,
      gfx::CreateVectorIcon(kNotificationCenterClearAllIcon,
                            message_center_style::kActionIconSize,
                            message_center_style::kActiveButtonColor));
  close_all_button_->SetImage(
      views::Button::STATE_DISABLED,
      gfx::CreateVectorIcon(kNotificationCenterClearAllIcon,
                            message_center_style::kActionIconSize,
                            message_center_style::kInactiveButtonColor));
  close_all_button_->SetTooltipText(l10n_util::GetStringUTF16(
      IDS_ASH_MESSAGE_CENTER_CLEAR_ALL_BUTTON_TOOLTIP));
  SetDefaultButtonStyle(close_all_button_);
  button_container_->AddChildView(close_all_button_);
  button_container_->AddChildView(CreateVerticalSeparator());

  quiet_mode_button_ = new views::ToggleImageButton(this);
  quiet_mode_button_->SetImage(
      views::Button::STATE_NORMAL,
      gfx::CreateVectorIcon(kNotificationCenterDoNotDisturbOffIcon,
                            message_center_style::kActionIconSize,
                            message_center_style::kInactiveButtonColor));
  gfx::ImageSkia quiet_mode_toggle_icon =
      gfx::CreateVectorIcon(kNotificationCenterDoNotDisturbOnIcon,
                            message_center_style::kActionIconSize,
                            message_center_style::kActiveButtonColor);
  quiet_mode_button_->SetToggledImage(views::Button::STATE_NORMAL,
                                      &quiet_mode_toggle_icon);
  quiet_mode_button_->SetTooltipText(l10n_util::GetStringUTF16(
      IDS_ASH_MESSAGE_CENTER_QUIET_MODE_BUTTON_TOOLTIP));
  SetQuietModeState(message_center->IsQuietMode());
  SetDefaultButtonStyle(quiet_mode_button_);
  button_container_->AddChildView(quiet_mode_button_);
  button_container_->AddChildView(CreateVerticalSeparator());

  collapse_button_ = new views::ImageButton(this);
  collapse_button_->SetImage(
      views::Button::STATE_NORMAL,
      gfx::CreateVectorIcon(kNotificationCenterCollapseIcon,
                            message_center_style::kActionIconSize,
                            message_center_style::kActiveButtonColor));
  collapse_button_->SetTooltipText(l10n_util::GetStringUTF16(
      IDS_ASH_MESSAGE_CENTER_COLLAPSE_BUTTON_TOOLTIP));
  SetDefaultButtonStyle(collapse_button_);
  AddChildView(collapse_button_);

  settings_button_ = new views::ImageButton(this);
  settings_button_->SetImage(
      views::Button::STATE_NORMAL,
      gfx::CreateVectorIcon(kNotificationCenterSettingsIcon,
                            message_center_style::kActionIconSize,
                            message_center_style::kActiveButtonColor));
  settings_button_->SetTooltipText(l10n_util::GetStringUTF16(
      IDS_ASH_MESSAGE_CENTER_SETTINGS_BUTTON_TOOLTIP));
  SetDefaultButtonStyle(settings_button_);
  button_container_->AddChildView(settings_button_);

  AddChildView(button_container_);

  SetCloseAllButtonEnabled(!settings_initially_visible);
  SetBackArrowVisible(settings_initially_visible);
}

MessageCenterButtonBar::~MessageCenterButtonBar() {}

void MessageCenterButtonBar::SetSettingsAndQuietModeButtonsEnabled(
    bool enabled) {
  settings_button_->SetEnabled(enabled);
  quiet_mode_button_->SetEnabled(enabled);
}

void MessageCenterButtonBar::SetCloseAllButtonEnabled(bool enabled) {
  if (close_all_button_)
    close_all_button_->SetEnabled(enabled);
}

views::Button* MessageCenterButtonBar::GetCloseAllButtonForTest() const {
  return close_all_button_;
}

views::Button* MessageCenterButtonBar::GetQuietModeButtonForTest() const {
  return quiet_mode_button_;
}

views::Button* MessageCenterButtonBar::GetSettingsButtonForTest() const {
  return settings_button_;
}

void MessageCenterButtonBar::SetBackArrowVisible(bool visible) {
  collapse_button_->SetVisible(visible);
  button_container_->SetVisible(!visible);
  Layout();
}

void MessageCenterButtonBar::SetTitle(const base::string16& title) {
  notification_label_->SetText(title);
}

void MessageCenterButtonBar::SetButtonsVisible(bool visible) {
  settings_button_->SetVisible(visible);
  quiet_mode_button_->SetVisible(visible);

  if (close_all_button_)
    close_all_button_->SetVisible(visible);

  Layout();
}

void MessageCenterButtonBar::SetQuietModeState(bool is_quiet_mode) {
  quiet_mode_button_->SetToggled(is_quiet_mode);
}

void MessageCenterButtonBar::ChildVisibilityChanged(views::View* child) {
  InvalidateLayout();
}

void MessageCenterButtonBar::ButtonPressed(views::Button* sender,
                                           const ui::Event& event) {
  if (sender == close_all_button_) {
    message_center_view()->ClearAllClosableNotifications();
  } else if (sender == settings_button_) {
    message_center_view()->SetSettingsVisible(true);
  } else if (sender == collapse_button_) {
    message_center_view()->SetSettingsVisible(false);
  } else if (sender == quiet_mode_button_) {
    if (message_center()->IsQuietMode())
      message_center()->SetQuietMode(false);
    else
      message_center()->EnterQuietModeWithExpire(base::TimeDelta::FromDays(1));
  } else {
    NOTREACHED();
  }
}

}  // namespace ash
