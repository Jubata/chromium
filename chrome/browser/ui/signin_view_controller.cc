// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/signin_view_controller.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/signin/dice_tab_helper.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_dialogs.h"
#include "chrome/browser/ui/signin_view_controller_delegate.h"
#include "chrome/browser/ui/singleton_tabs.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/signin/core/browser/profile_management_switches.h"
#include "google_apis/gaia/gaia_urls.h"

namespace {

// Returns the sign-in reason for |mode|.
signin_metrics::Reason GetSigninReasonFromMode(profiles::BubbleViewMode mode) {
  DCHECK(SigninViewController::ShouldShowSigninForMode(mode));
  switch (mode) {
    case profiles::BUBBLE_VIEW_MODE_GAIA_SIGNIN:
      return signin_metrics::Reason::REASON_SIGNIN_PRIMARY_ACCOUNT;
    case profiles::BUBBLE_VIEW_MODE_GAIA_ADD_ACCOUNT:
      return signin_metrics::Reason::REASON_ADD_SECONDARY_ACCOUNT;
    case profiles::BUBBLE_VIEW_MODE_GAIA_REAUTH:
      return signin_metrics::Reason::REASON_REAUTHENTICATION;
    default:
      NOTREACHED();
      return signin_metrics::Reason::REASON_UNKNOWN_REASON;
  }
}

}  // namespace

SigninViewController::SigninViewController() : delegate_(nullptr) {}

SigninViewController::~SigninViewController() {
  CloseModalSignin();
}

// static
bool SigninViewController::ShouldShowSigninForMode(
    profiles::BubbleViewMode mode) {
  return mode == profiles::BUBBLE_VIEW_MODE_GAIA_SIGNIN ||
         mode == profiles::BUBBLE_VIEW_MODE_GAIA_ADD_ACCOUNT ||
         mode == profiles::BUBBLE_VIEW_MODE_GAIA_REAUTH;
}

void SigninViewController::ShowSignin(
    profiles::BubbleViewMode mode,
    Browser* browser,
    signin_metrics::AccessPoint access_point) {
  DCHECK(ShouldShowSigninForMode(mode));
  if (signin::IsDicePrepareMigrationEnabled()) {
    ShowDiceSigninTab(mode, browser, access_point);
  } else {
    ShowModalSigninDialog(mode, browser, access_point);
  }
}

void SigninViewController::ShowModalSigninDialog(
    profiles::BubbleViewMode mode,
    Browser* browser,
    signin_metrics::AccessPoint access_point) {
  CloseModalSignin();
  // The delegate will delete itself on request of the UI code when the widget
  // is closed.
  delegate_ = SigninViewControllerDelegate::CreateModalSigninDelegate(
      this, mode, browser, access_point);

  // When the user has a proxy that requires HTTP auth, loading the sign-in
  // dialog can trigger the HTTP auth dialog.  This means the signin view
  // controller needs a dialog manager to handle any such dialog.
  delegate_->AttachDialogManager();
  chrome::RecordDialogCreation(chrome::DialogIdentifier::SIGN_IN);
}

void SigninViewController::ShowModalSyncConfirmationDialog(Browser* browser) {
  CloseModalSignin();
  // The delegate will delete itself on request of the UI code when the widget
  // is closed.
  delegate_ = SigninViewControllerDelegate::CreateSyncConfirmationDelegate(
      this, browser);
  chrome::RecordDialogCreation(
      chrome::DialogIdentifier::SIGN_IN_SYNC_CONFIRMATION);
}

void SigninViewController::ShowModalSigninErrorDialog(Browser* browser) {
  CloseModalSignin();
  // The delegate will delete itself on request of the UI code when the widget
  // is closed.
  delegate_ =
      SigninViewControllerDelegate::CreateSigninErrorDelegate(this, browser);
  chrome::RecordDialogCreation(chrome::DialogIdentifier::SIGN_IN_ERROR);
}

bool SigninViewController::ShowsModalDialog() {
  return delegate_ != nullptr;
}

void SigninViewController::CloseModalSignin() {
  if (delegate_)
    delegate_->CloseModalSignin();

  DCHECK(!delegate_);
}

void SigninViewController::SetModalSigninHeight(int height) {
  if (delegate_)
    delegate_->ResizeNativeView(height);
}

void SigninViewController::PerformNavigation() {
  if (delegate_)
    delegate_->PerformNavigation();
}

void SigninViewController::ResetModalSigninDelegate() {
  delegate_ = nullptr;
}

void SigninViewController::ShowDiceSigninTab(
    profiles::BubbleViewMode mode,
    Browser* browser,
    signin_metrics::AccessPoint access_point) {
  signin_metrics::Reason signin_reason = GetSigninReasonFromMode(mode);
  chrome::ShowSingletonTab(browser, GaiaUrls::GetInstance()->add_account_url());
  content::WebContents* active_contents =
      browser->tab_strip_model()->GetActiveWebContents();
  DCHECK_EQ(GaiaUrls::GetInstance()->add_account_url(),
            active_contents->GetVisibleURL());
  DiceTabHelper::CreateForWebContents(active_contents);
  DiceTabHelper* tab_helper = DiceTabHelper::FromWebContents(active_contents);
  tab_helper->SetSigninAccessPoint(access_point);
  tab_helper->SetSigninReason(signin_reason);

  if (signin_reason == signin_metrics::Reason::REASON_SIGNIN_PRIMARY_ACCOUNT) {
    signin_metrics::LogSigninAccessPointStarted(access_point);
    signin_metrics::RecordSigninUserActionForAccessPoint(access_point);
  }
}

content::WebContents*
SigninViewController::GetModalDialogWebContentsForTesting() {
  DCHECK(delegate_);
  return delegate_->web_contents();
}
