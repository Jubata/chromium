// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/arc/arc_auth_notification.h"

#include <memory>
#include <string>
#include <utility>

#include "base/macros.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/chromeos/arc/arc_optin_uma.h"
#include "chrome/browser/chromeos/arc/arc_util.h"
#include "chrome/browser/notifications/notification_display_service.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ash/multi_user/multi_user_util.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/grit/theme_resources.h"
#include "components/session_manager/core/session_manager.h"
#include "components/signin/core/account_id/account_id.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/chromeos/devicetype_utils.h"
#include "ui/message_center/notification.h"
#include "ui/message_center/notification_delegate.h"
#include "ui/message_center/public/cpp/message_center_switches.h"
#include "url/gurl.h"

namespace {

bool g_disabled = false;

// Ids of the notification shown on first run.
const char kNotifierId[] = "arc_auth";
const char kFirstRunNotificationId[] = "arc_auth/first_run";

class ArcAuthNotificationDelegate
    : public message_center::NotificationDelegate {
 public:
  explicit ArcAuthNotificationDelegate(Profile* profile) : profile_(profile) {}

  // message_center::NotificationDelegate
  // TODO(tetsui): Remove this method when IsNewStyleNotificationEnabled() flag
  // is removed.
  void ButtonClick(int button_index) override {
    if (button_index == 0) {
      LogUma(arc::OptInActionType::NOTIFICATION_ACCEPTED);
      arc::SetArcPlayStoreEnabledForProfile(profile_, true);
    } else {
      LogUma(arc::OptInActionType::NOTIFICATION_DECLINED);
      arc::SetArcPlayStoreEnabledForProfile(profile_, false);
    }
  }

  void Click() override {
    LogUma(arc::OptInActionType::NOTIFICATION_ACCEPTED);
    arc::SetArcPlayStoreEnabledForProfile(profile_, true);
  }

  void Close(bool by_user) override {
    LogUma(arc::OptInActionType::NOTIFICATION_TIMED_OUT);
  }

 private:
  ~ArcAuthNotificationDelegate() override {}

  void LogUma(arc::OptInActionType action) {
    if (!uma_logged_)
      UpdateOptInActionUMA(action);
    uma_logged_ = true;
  }

  Profile* const profile_;

  // Whether a UMA action has been logged.
  bool uma_logged_ = false;

  DISALLOW_COPY_AND_ASSIGN(ArcAuthNotificationDelegate);
};

}  // namespace

namespace arc {

ArcAuthNotification::ArcAuthNotification(Profile* profile)
    : profile_(profile), weak_ptr_factory_(this) {
  if (g_disabled)
    return;
  session_manager::SessionManager* session_manager =
      session_manager::SessionManager::Get();
  if (!session_manager) {
    // In case we have message center to show notification.
    Show();
    return;
  }

  if (session_manager->session_state() ==
      session_manager::SessionState::ACTIVE) {
    Show();
  } else {
    session_manager->AddObserver(this);
  }
}

ArcAuthNotification::~ArcAuthNotification() {
  Hide();
}

// static
void ArcAuthNotification::DisableForTesting() {
  g_disabled = true;
}

void ArcAuthNotification::Show() {
  DCHECK(!g_disabled);

  message_center::NotifierId notifier_id(
      message_center::NotifierId::SYSTEM_COMPONENT, kNotifierId);
  notifier_id.profile_id =
      multi_user_util::GetAccountIdFromProfile(profile_).GetUserEmail();

  std::unique_ptr<message_center::Notification> notification;
  if (message_center::IsNewStyleNotificationEnabled()) {
    notification = message_center::Notification::CreateSystemNotification(
        message_center::NOTIFICATION_TYPE_SIMPLE, kFirstRunNotificationId,
        l10n_util::GetStringFUTF16(IDS_ARC_NOTIFICATION_TITLE,
                                   ui::GetChromeOSDeviceName()),
        l10n_util::GetStringUTF16(IDS_ARC_NOTIFICATION_MESSAGE), gfx::Image(),
        l10n_util::GetStringUTF16(IDS_ARC_NOTIFICATION_DISPLAY_SOURCE), GURL(),
        notifier_id, message_center::RichNotificationData(),
        new ArcAuthNotificationDelegate(profile_), kNotificationPlayPrismIcon,
        message_center::SystemNotificationWarningLevel::NORMAL);
  } else {
    message_center::RichNotificationData data;
    data.buttons.push_back(message_center::ButtonInfo(l10n_util::GetStringUTF16(
        IDS_ARC_OPEN_PLAY_STORE_NOTIFICATION_BUTTON)));
    data.buttons.push_back(message_center::ButtonInfo(
        l10n_util::GetStringUTF16(IDS_ARC_CANCEL_NOTIFICATION_BUTTON)));

    ui::ResourceBundle& resource_bundle =
        ui::ResourceBundle::GetSharedInstance();
    notification = std::make_unique<message_center::Notification>(
        message_center::NOTIFICATION_TYPE_SIMPLE, kFirstRunNotificationId,
        l10n_util::GetStringFUTF16(IDS_ARC_NOTIFICATION_TITLE,
                                   ui::GetChromeOSDeviceName()),
        l10n_util::GetStringUTF16(IDS_ARC_NOTIFICATION_MESSAGE),
        resource_bundle.GetImageNamed(IDR_ARC_PLAY_STORE_NOTIFICATION),
        l10n_util::GetStringUTF16(IDS_ARC_NOTIFICATION_DISPLAY_SOURCE), GURL(),
        notifier_id, data, new ArcAuthNotificationDelegate(profile_));
  }
  NotificationDisplayService::GetForProfile(profile_)->Display(
      NotificationCommon::TRANSIENT, *notification);
}

void ArcAuthNotification::Hide() {
  NotificationDisplayService::GetForProfile(profile_)->Close(
      NotificationCommon::TRANSIENT, kFirstRunNotificationId);

  session_manager::SessionManager* session_manager =
      session_manager::SessionManager::Get();
  if (session_manager)
    session_manager->RemoveObserver(this);
}

void ArcAuthNotification::OnSessionStateChanged() {
  session_manager::SessionManager* session_manager =
      session_manager::SessionManager::Get();
  if (session_manager->session_state() !=
      session_manager::SessionState::ACTIVE) {
    return;
  }

  session_manager->RemoveObserver(this);
  // Don't call Show directly due race condition in observers.
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::Bind(&ArcAuthNotification::Show, weak_ptr_factory_.GetWeakPtr()));
}

}  // namespace arc
