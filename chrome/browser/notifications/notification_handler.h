// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_NOTIFICATIONS_NOTIFICATION_HANDLER_H_
#define CHROME_BROWSER_NOTIFICATIONS_NOTIFICATION_HANDLER_H_

#include <memory>
#include <string>

#include "base/callback_forward.h"
#include "base/optional.h"
#include "base/strings/string16.h"

class GURL;
class Profile;

// Interface that enables the different kind of notifications to process
// operations coming from the user or decisions made by the underlying
// notification type.
class NotificationHandler {
 public:
  virtual ~NotificationHandler();

  // Called after a notification has been displayed.
  virtual void OnShow(Profile* profile, const std::string& notification_id);

  // Process notification close events. The |completed_closure| must be invoked
  // on the UI thread once processing of the close event has been finished.
  virtual void OnClose(Profile* profile,
                       const GURL& origin,
                       const std::string& notification_id,
                       bool by_user,
                       base::OnceClosure completed_closure);

  // Process clicks on a notification or its buttons, depending on
  // |action_index|. The |completed_closure| must be invoked on the UI thread
  // once processing of the click event has been finished.
  virtual void OnClick(Profile* profile,
                       const GURL& origin,
                       const std::string& notification_id,
                       const base::Optional<int>& action_index,
                       const base::Optional<base::string16>& reply,
                       base::OnceClosure completed_closure);

  // Open notification settings.
  virtual void OpenSettings(Profile* profile);
};

#endif  // CHROME_BROWSER_NOTIFICATIONS_NOTIFICATION_HANDLER_H_
