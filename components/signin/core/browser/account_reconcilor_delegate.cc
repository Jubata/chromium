// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/signin/core/browser/account_reconcilor_delegate.h"

namespace signin {

bool AccountReconcilorDelegate::IsReconcileEnabled() const {
  return false;
}

bool AccountReconcilorDelegate::IsAccountConsistencyEnforced() const {
  return false;
}

bool AccountReconcilorDelegate::ShouldAbortReconcileIfPrimaryHasError() const {
  return false;
}

std::string AccountReconcilorDelegate::GetFirstGaiaAccountForReconcile(
    const std::vector<std::string>& chrome_accounts,
    const std::vector<gaia::ListedAccount>& gaia_accounts,
    const std::string& primary_account,
    bool first_execution) const {
  return std::string();
}

}  // namespace signin
