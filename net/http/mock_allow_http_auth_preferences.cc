// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/mock_allow_http_auth_preferences.h"
#include "build/build_config.h"

namespace net {

MockAllowHttpAuthPreferences::MockAllowHttpAuthPreferences() {}

MockAllowHttpAuthPreferences::~MockAllowHttpAuthPreferences() {}

bool MockAllowHttpAuthPreferences::CanUseDefaultCredentials(
    const GURL& auth_origin) const {
  return true;
}

bool MockAllowHttpAuthPreferences::CanDelegate(const GURL& auth_origin) const {
  return true;
}

}  // namespace net
