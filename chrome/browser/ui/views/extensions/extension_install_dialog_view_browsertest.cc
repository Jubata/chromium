// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/extensions/extension_install_dialog_view.h"

#include <string>
#include <utility>

#include "base/macros.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/extensions/chrome_test_extension_loader.h"
#include "chrome/browser/extensions/extension_browsertest.h"
#include "chrome/browser/extensions/extension_icon_manager.h"
#include "chrome/browser/extensions/extension_install_prompt.h"
#include "chrome/browser/extensions/extension_install_prompt_test_helper.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/test/test_browser_dialog.h"
#include "chrome/browser/ui/webui/extensions/extension_settings_handler.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/extensions/extension_test_util.h"
#include "components/constrained_window/constrained_window_views.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/test/test_utils.h"
#include "extensions/common/extension.h"
#include "extensions/common/permissions/permission_message_provider.h"
#include "extensions/common/permissions/permissions_data.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_client_view.h"

using extensions::PermissionIDSet;
using extensions::PermissionMessage;
using extensions::PermissionMessages;

class ExtensionInstallDialogViewTestBase : public ExtensionBrowserTest {
 protected:
  explicit ExtensionInstallDialogViewTestBase(
      ExtensionInstallPrompt::PromptType prompt_type);
  ~ExtensionInstallDialogViewTestBase() override {}

  void SetUpOnMainThread() override;

  // Creates and returns an install prompt of |prompt_type_|.
  std::unique_ptr<ExtensionInstallPrompt::Prompt> CreatePrompt();

  content::WebContents* web_contents() { return web_contents_; }

 private:
  const extensions::Extension* extension_;
  ExtensionInstallPrompt::PromptType prompt_type_;
  content::WebContents* web_contents_;

  DISALLOW_COPY_AND_ASSIGN(ExtensionInstallDialogViewTestBase);
};

ExtensionInstallDialogViewTestBase::ExtensionInstallDialogViewTestBase(
    ExtensionInstallPrompt::PromptType prompt_type)
    : extension_(nullptr), prompt_type_(prompt_type), web_contents_(nullptr) {}

void ExtensionInstallDialogViewTestBase::SetUpOnMainThread() {
  ExtensionBrowserTest::SetUpOnMainThread();

  extension_ = ExtensionBrowserTest::LoadExtension(test_data_dir_.AppendASCII(
      "install_prompt/permissions_scrollbar_regression"));

  web_contents_ = browser()->tab_strip_model()->GetWebContentsAt(0);
}

std::unique_ptr<ExtensionInstallPrompt::Prompt>
ExtensionInstallDialogViewTestBase::CreatePrompt() {
  std::unique_ptr<ExtensionInstallPrompt::Prompt> prompt(
      new ExtensionInstallPrompt::Prompt(prompt_type_));
  prompt->set_extension(extension_);

  std::unique_ptr<ExtensionIconManager> icon_manager(
      new ExtensionIconManager());
  prompt->set_icon(icon_manager->GetIcon(extension_->id()));

  return prompt;
}

class ScrollbarTest : public ExtensionInstallDialogViewTestBase {
 protected:
  ScrollbarTest();
  ~ScrollbarTest() override {}

  bool IsScrollbarVisible(
      std::unique_ptr<ExtensionInstallPrompt::Prompt> prompt);

 private:
  DISALLOW_COPY_AND_ASSIGN(ScrollbarTest);
};

ScrollbarTest::ScrollbarTest()
    : ExtensionInstallDialogViewTestBase(
          ExtensionInstallPrompt::PERMISSIONS_PROMPT) {
}

bool ScrollbarTest::IsScrollbarVisible(
    std::unique_ptr<ExtensionInstallPrompt::Prompt> prompt) {
  ExtensionInstallDialogView* dialog = new ExtensionInstallDialogView(
      profile(), web_contents(), ExtensionInstallPrompt::DoneCallback(),
      std::move(prompt));

  // Create the modal view around the install dialog view.
  views::Widget* modal = constrained_window::CreateBrowserModalDialogViews(
      dialog, web_contents()->GetTopLevelNativeWindow());
  modal->Show();
  content::RunAllTasksUntilIdle();

  // Check if the vertical scrollbar is visible.
  return dialog->scroll_view()->vertical_scroll_bar()->visible();
}

// Tests that a scrollbar _is_ shown for an excessively long extension
// install prompt.
IN_PROC_BROWSER_TEST_F(ScrollbarTest, LongPromptScrollbar) {
  base::string16 permission_string(base::ASCIIToUTF16("Test"));
  PermissionMessages permissions;
  for (int i = 0; i < 20; i++) {
    permissions.push_back(PermissionMessage(permission_string,
                                            PermissionIDSet()));
  }
  std::unique_ptr<ExtensionInstallPrompt::Prompt> prompt = CreatePrompt();
  prompt->AddPermissions(permissions,
                         ExtensionInstallPrompt::REGULAR_PERMISSIONS);
  ASSERT_TRUE(IsScrollbarVisible(std::move(prompt)))
      << "Scrollbar is not visible";
}

// Tests that a scrollbar isn't shown for this regression case.
// See crbug.com/385570 for details.
IN_PROC_BROWSER_TEST_F(ScrollbarTest, ScrollbarRegression) {
  base::string16 permission_string(base::ASCIIToUTF16(
      "Read and modify your data on *.facebook.com"));
  PermissionMessages permissions;
  permissions.push_back(PermissionMessage(permission_string,
                                          PermissionIDSet()));
  std::unique_ptr<ExtensionInstallPrompt::Prompt> prompt = CreatePrompt();
  prompt->AddPermissions(permissions,
                         ExtensionInstallPrompt::REGULAR_PERMISSIONS);
  ASSERT_FALSE(IsScrollbarVisible(std::move(prompt))) << "Scrollbar is visible";
}

class ExtensionInstallDialogViewTest
    : public ExtensionInstallDialogViewTestBase {
 protected:
  ExtensionInstallDialogViewTest()
      : ExtensionInstallDialogViewTestBase(
            ExtensionInstallPrompt::INSTALL_PROMPT) {}
  ~ExtensionInstallDialogViewTest() override {}

  views::DialogDelegateView* CreateAndShowPrompt(
      ExtensionInstallPromptTestHelper* helper) {
    std::unique_ptr<ExtensionInstallDialogView> dialog(
        new ExtensionInstallDialogView(profile(), web_contents(),
                                       helper->GetCallback(), CreatePrompt()));
    views::DialogDelegateView* delegate_view = dialog.get();

    views::Widget* modal_dialog = views::DialogDelegate::CreateDialogWidget(
        dialog.release(), nullptr,
        platform_util::GetViewForWindow(
            browser()->window()->GetNativeWindow()));
    modal_dialog->Show();

    return delegate_view;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ExtensionInstallDialogViewTest);
};

// Verifies that the delegate is notified when the user selects to accept or
// cancel the install.
IN_PROC_BROWSER_TEST_F(ExtensionInstallDialogViewTest, NotifyDelegate) {
  {
    // User presses install.
    ExtensionInstallPromptTestHelper helper;
    views::DialogDelegateView* delegate_view = CreateAndShowPrompt(&helper);
    delegate_view->GetDialogClientView()->AcceptWindow();
    EXPECT_EQ(ExtensionInstallPrompt::Result::ACCEPTED, helper.result());
  }
  {
    // User presses cancel.
    ExtensionInstallPromptTestHelper helper;
    views::DialogDelegateView* delegate_view = CreateAndShowPrompt(&helper);
    delegate_view->GetDialogClientView()->CancelWindow();
    EXPECT_EQ(ExtensionInstallPrompt::Result::USER_CANCELED, helper.result());
  }
  {
    // Dialog is closed without the user explicitly choosing to proceed or
    // cancel.
    ExtensionInstallPromptTestHelper helper;
    views::DialogDelegateView* delegate_view = CreateAndShowPrompt(&helper);
    delegate_view->GetWidget()->Close();
    // TODO(devlin): Should this be ABORTED?
    EXPECT_EQ(ExtensionInstallPrompt::Result::USER_CANCELED, helper.result());
  }
}

// Verifies that the "Add extension" button is disabled initally, but re-enabled
// after a short time delay.
IN_PROC_BROWSER_TEST_F(ExtensionInstallDialogViewTest, InstallButtonDelay) {
  ExtensionInstallDialogView::SetInstallButtonDelayForTesting(0);
  ExtensionInstallPromptTestHelper helper;
  views::DialogDelegateView* delegate_view = CreateAndShowPrompt(&helper);

  // Check that dialog is visible.
  EXPECT_TRUE(delegate_view->visible());

  // Check initial button states.
  EXPECT_FALSE(delegate_view->IsDialogButtonEnabled(ui::DIALOG_BUTTON_OK));
  EXPECT_TRUE(delegate_view->IsDialogButtonEnabled(ui::DIALOG_BUTTON_CANCEL));
  EXPECT_TRUE(delegate_view->GetInitiallyFocusedView()->HasFocus());

  // Check OK button state after timeout to verify that it is re-enabled.
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(delegate_view->IsDialogButtonEnabled(ui::DIALOG_BUTTON_OK));

  // Ensure default button (cancel) has focus.
  EXPECT_TRUE(delegate_view->GetInitiallyFocusedView()->HasFocus());
  delegate_view->Close();
}

class ExtensionInstallDialogViewInteractiveBrowserTest
    : public DialogBrowserTest {
 public:
  ExtensionInstallDialogViewInteractiveBrowserTest() {}

  // DialogBrowserTest:
  void ShowDialog(const std::string& name) override {
    extensions::ChromeTestExtensionLoader loader(browser()->profile());
    base::FilePath test_data_dir;
    PathService::Get(chrome::DIR_TEST_DATA, &test_data_dir);
    scoped_refptr<const extensions::Extension> extension = loader.LoadExtension(
        test_data_dir.AppendASCII("extensions/uitest/long_name"));

    SkBitmap icon;
    // The dialog will downscale large images.
    icon.allocN32Pixels(800, 800);
    icon.eraseARGB(255, 128, 255, 128);

    auto prompt = std::make_unique<ExtensionInstallPrompt::Prompt>(
        external_install_ ? ExtensionInstallPrompt::EXTERNAL_INSTALL_PROMPT
                          : ExtensionInstallPrompt::INLINE_INSTALL_PROMPT);
    prompt->AddPermissions(permissions_,
                           ExtensionInstallPrompt::REGULAR_PERMISSIONS);

    if (from_webstore_)
      prompt->SetWebstoreData("69,420", true, 2.5, 37);

    auto* web_contents = browser()->tab_strip_model()->GetActiveWebContents();
    auto install_prompt =
        std::make_unique<ExtensionInstallPrompt>(web_contents);
    install_prompt->ShowDialog(
        base::Bind([](ExtensionInstallPrompt::Result r) {}), extension.get(),
        &icon, std::move(prompt), ExtensionInstallPrompt::ShowDialogCallback());
  }

  void set_external_install() { external_install_ = true; }
  void set_from_webstore() { from_webstore_ = true; }

  void AddPermission(std::string permission) {
    permissions_.push_back(
        PermissionMessage(base::ASCIIToUTF16(permission), PermissionIDSet()));
  }

  void AddPermissionWithDetails(
      std::string main_permission,
      std::vector<base::string16> detailed_permissions) {
    permissions_.push_back(
        PermissionMessage(base::ASCIIToUTF16(main_permission),
                          PermissionIDSet(), std::move(detailed_permissions)));
  }

 private:
  bool external_install_ = false;
  bool from_webstore_ = false;
  PermissionMessages permissions_;

  DISALLOW_COPY_AND_ASSIGN(ExtensionInstallDialogViewInteractiveBrowserTest);
};

IN_PROC_BROWSER_TEST_F(ExtensionInstallDialogViewInteractiveBrowserTest,
                       InvokeDialog_Simple) {
  RunDialog();
}

IN_PROC_BROWSER_TEST_F(ExtensionInstallDialogViewInteractiveBrowserTest,
                       InvokeDialog_External) {
  set_external_install();
  RunDialog();
}

IN_PROC_BROWSER_TEST_F(ExtensionInstallDialogViewInteractiveBrowserTest,
                       InvokeDialog_ExternalWithPermission) {
  set_external_install();
  AddPermission("Example permission");
  RunDialog();
}

IN_PROC_BROWSER_TEST_F(ExtensionInstallDialogViewInteractiveBrowserTest,
                       InvokeDialog_FromWebstore) {
  set_from_webstore();
  RunDialog();
}

IN_PROC_BROWSER_TEST_F(ExtensionInstallDialogViewInteractiveBrowserTest,
                       InvokeDialog_FromWebstoreWithPermission) {
  set_from_webstore();
  AddPermission("Example permission");
  RunDialog();
}

IN_PROC_BROWSER_TEST_F(ExtensionInstallDialogViewInteractiveBrowserTest,
                       InvokeDialog_MultilinePermission) {
  AddPermission(
      "In the shade of the house, in the sunshine of the riverbank "
      "near the boats, in the shade of the Sal-wood forest, in the "
      "shade of the fig tree is where Siddhartha grew up");
  RunDialog();
}

IN_PROC_BROWSER_TEST_F(ExtensionInstallDialogViewInteractiveBrowserTest,
                       InvokeDialog_ManyPermissions) {
  for (int i = 0; i < 20; i++)
    AddPermission("Example permission");
  RunDialog();
}

IN_PROC_BROWSER_TEST_F(ExtensionInstallDialogViewInteractiveBrowserTest,
                       InvokeDialog_DetailedPermission) {
  AddPermissionWithDetails("Example header permission",
                           {base::ASCIIToUTF16("Detailed permission 1"),
                            base::ASCIIToUTF16("Detailed permission 2"),
                            base::ASCIIToUTF16("Detailed permission 3")});
  RunDialog();
}
