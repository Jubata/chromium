// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/page_info/page_info_bubble_view.h"

#include "base/run_loop.h"
#include "base/test/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/safe_browsing/chrome_password_protection_service.h"
#include "chrome/browser/ssl/security_state_tab_helper.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/test/test_browser_dialog.h"
#include "chrome/browser/ui/view_ids.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/location_bar/location_bar_view.h"
#include "chrome/browser/ui/views/location_bar/location_icon_view.h"
#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/url_constants.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/safe_browsing/features.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/cert_test_util.h"
#include "net/test/test_data_directory.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/accessibility/ax_action_data.h"
#include "ui/events/event_constants.h"

namespace {

const char kSyncPasswordPageInfoHistogramName[] =
    "PasswordProtection.PageInfoAction.SyncPasswordEntry";

class ClickEvent : public ui::Event {
 public:
  ClickEvent() : ui::Event(ui::ET_UNKNOWN, base::TimeTicks(), 0) {}
};

void PerformMouseClickOnView(views::View* view) {
  ui::AXActionData data;
  data.action = ui::AX_ACTION_DO_DEFAULT;
  view->HandleAccessibleAction(data);
}

// Clicks the location icon to open the page info bubble.
void OpenPageInfoBubble(Browser* browser) {
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  LocationIconView* location_icon_view =
      browser_view->toolbar()->location_bar()->location_icon_view();
  ASSERT_TRUE(location_icon_view);
  ClickEvent event;
  location_icon_view->ShowBubble(event);
  views::BubbleDialogDelegateView* page_info =
      PageInfoBubbleView::GetPageInfoBubble();
  EXPECT_NE(nullptr, page_info);
  page_info->set_close_on_deactivate(false);
}

// Opens the Page Info bubble and retrieves the UI view identified by
// |view_id|.
views::View* GetView(Browser* browser, int view_id) {
  views::Widget* page_info_bubble =
      PageInfoBubbleView::GetPageInfoBubble()->GetWidget();
  EXPECT_TRUE(page_info_bubble);

  views::View* view = page_info_bubble->GetRootView()->GetViewByID(view_id);
  EXPECT_TRUE(view);
  return view;
}

// Clicks the "Site settings" button from Page Info and waits for a "Settings"
// tab to open.
void ClickAndWaitForSettingsPageToOpen(views::View* site_settings_button) {
  content::WebContentsAddedObserver new_tab_observer;
  PerformMouseClickOnView(site_settings_button);

  base::string16 expected_title(base::ASCIIToUTF16("Settings"));
  content::TitleWatcher title_watcher(new_tab_observer.GetWebContents(),
                                      expected_title);
  EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
}

// Returns the URL of the new tab that's opened on clicking the "Site settings"
// button from Page Info.
const GURL OpenSiteSettingsForUrl(Browser* browser, const GURL& url) {
  ui_test_utils::NavigateToURL(browser, url);
  OpenPageInfoBubble(browser);
  // Get site settings button.
  views::View* site_settings_button = GetView(
      browser,
      PageInfoBubbleView::VIEW_ID_PAGE_INFO_LINK_OR_BUTTON_SITE_SETTINGS);
  ClickAndWaitForSettingsPageToOpen(site_settings_button);

  return browser->tab_strip_model()
      ->GetActiveWebContents()
      ->GetLastCommittedURL();
}

}  // namespace

class PageInfoBubbleViewBrowserTest : public DialogBrowserTest {
 public:
  PageInfoBubbleViewBrowserTest() {}

  // DialogBrowserTest:
  void ShowDialog(const std::string& name) override {
    // All the possible test names.
    constexpr char kInsecure[] = "Insecure";
    constexpr char kInternal[] = "Internal";
    constexpr char kInternalExtension[] = "InternalExtension";
    constexpr char kInternalViewSource[] = "InternalViewSource";
    constexpr char kSecure[] = "Secure";
    constexpr char kMalware[] = "Malware";
    constexpr char kDeceptive[] = "Deceptive";
    constexpr char kUnwantedSoftware[] = "UnwantedSoftware";
    constexpr char kPasswordReuseSoft[] = "PasswordReuseSoft";
    constexpr char kPasswordReuse[] = "PasswordReuse";
    constexpr char kMixedContentForm[] = "MixedContentForm";
    constexpr char kMixedContent[] = "MixedContent";

    const GURL internal_url("chrome://settings");
    const GURL internal_extension_url("chrome-extension://example");
    // Note the following two URLs are not really necessary to get the different
    // versions of Page Info to appear, but are here to indicate the type of
    // URL each IdentityInfo type would normally be associated with.
    const GURL https_url("https://example.com");
    const GURL http_url("http://example.com");

    GURL url = http_url;
    if (name == kSecure || name == kMixedContentForm || name == kMixedContent)
      url = https_url;
    if (name == kInternal) {
      url = internal_url;
    } else if (name == kInternalExtension) {
      url = internal_extension_url;
    } else if (name == kInternalViewSource) {
      constexpr char kTestHtml[] = "/viewsource/test.html";
      ASSERT_TRUE(embedded_test_server()->Start());
      url = GURL(content::kViewSourceScheme +
                 std::string(url::kStandardSchemeSeparator) +
                 embedded_test_server()->GetURL(kTestHtml).spec());
    }

    ui_test_utils::NavigateToURL(browser(), url);
    OpenPageInfoBubble(browser());

    PageInfoUI::IdentityInfo identity;
    if (name == kInsecure) {
      identity.identity_status = PageInfo::SITE_IDENTITY_STATUS_NO_CERT;
    } else if (name == kSecure) {
      // Generate a valid mock HTTPS identity, with a certificate.
      identity.identity_status = PageInfo::SITE_IDENTITY_STATUS_CERT;
      constexpr char kGoodCertificateFile[] = "ok_cert.pem";
      identity.certificate = net::ImportCertFromFile(
          net::GetTestCertsDirectory(), kGoodCertificateFile);
    } else if (name == kMalware) {
      identity.identity_status = PageInfo::SITE_IDENTITY_STATUS_MALWARE;
    } else if (name == kDeceptive) {
      identity.identity_status =
          PageInfo::SITE_IDENTITY_STATUS_SOCIAL_ENGINEERING;
    } else if (name == kUnwantedSoftware) {
      identity.identity_status =
          PageInfo::SITE_IDENTITY_STATUS_UNWANTED_SOFTWARE;
    } else if (name == kPasswordReuseSoft) {
      softer_warning_feature_.InitAndEnableFeatureWithParameters(
          safe_browsing::kGoogleBrandedPhishingWarning,
          {{"softer_warning", "true"}});
      identity.identity_status = PageInfo::SITE_IDENTITY_STATUS_PASSWORD_REUSE;
    } else if (name == kPasswordReuse) {
      softer_warning_feature_.InitAndEnableFeatureWithParameters(
          safe_browsing::kGoogleBrandedPhishingWarning,
          {{"softer_warning", "false"}});
      identity.identity_status = PageInfo::SITE_IDENTITY_STATUS_PASSWORD_REUSE;
    } else if (name == kMixedContentForm) {
      identity.identity_status =
          PageInfo::SITE_IDENTITY_STATUS_ADMIN_PROVIDED_CERT;
      identity.connection_status =
          PageInfo::SITE_CONNECTION_STATUS_INSECURE_FORM_ACTION;
    } else if (name == kMixedContent) {
      identity.identity_status =
          PageInfo::SITE_IDENTITY_STATUS_ADMIN_PROVIDED_CERT;
      identity.connection_status =
          PageInfo::SITE_CONNECTION_STATUS_INSECURE_PASSIVE_SUBRESOURCE;
    }

    if (name != kInsecure && name.find(kInternal) == std::string::npos) {
      // The bubble may be PageInfoBubbleView or InternalPageInfoBubbleView. The
      // latter is only used for |kInternal|, so it is safe to static_cast here.
      static_cast<PageInfoBubbleView*>(PageInfoBubbleView::GetPageInfoBubble())
          ->SetIdentityInfo(identity);
    }
  }

 private:
  base::test::ScopedFeatureList softer_warning_feature_;

  DISALLOW_COPY_AND_ASSIGN(PageInfoBubbleViewBrowserTest);
};

IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest, ShowBubble) {
  OpenPageInfoBubble(browser());
  EXPECT_EQ(PageInfoBubbleView::BUBBLE_PAGE_INFO,
            PageInfoBubbleView::GetShownBubbleType());
}

IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest, ChromeURL) {
  ui_test_utils::NavigateToURL(browser(), GURL("chrome://settings"));
  OpenPageInfoBubble(browser());
  EXPECT_EQ(PageInfoBubbleView::BUBBLE_INTERNAL_PAGE,
            PageInfoBubbleView::GetShownBubbleType());
}

IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest, ChromeExtensionURL) {
  ui_test_utils::NavigateToURL(
      browser(), GURL("chrome-extension://extension-id/options.html"));
  OpenPageInfoBubble(browser());
  EXPECT_EQ(PageInfoBubbleView::BUBBLE_INTERNAL_PAGE,
            PageInfoBubbleView::GetShownBubbleType());
}

// Times out due to isolation, see crbug.com/733767
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest,
                       DISABLED_ChromeDevtoolsURL) {
  ui_test_utils::NavigateToURL(
      browser(), GURL("chrome-devtools://devtools/bundled/inspector.html"));
  OpenPageInfoBubble(browser());
  EXPECT_EQ(PageInfoBubbleView::BUBBLE_INTERNAL_PAGE,
            PageInfoBubbleView::GetShownBubbleType());
}

IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest, ViewSourceURL) {
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));
  browser()
      ->tab_strip_model()
      ->GetActiveWebContents()
      ->GetMainFrame()
      ->ViewSource();
  OpenPageInfoBubble(browser());
  EXPECT_EQ(PageInfoBubbleView::BUBBLE_INTERNAL_PAGE,
            PageInfoBubbleView::GetShownBubbleType());
}

// Test opening "Site Details" via Page Info from an ASCII origin does the
// correct URL canonicalization.
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest, SiteSettingsLink) {
  GURL url = GURL("https://www.google.com/");
  std::string expected_origin = "https%3A%2F%2Fwww.google.com";
  EXPECT_EQ(GURL(chrome::kChromeUISiteDetailsPrefixURL + expected_origin),
            OpenSiteSettingsForUrl(browser(), url));
}

// Test opening "Site Details" via Page Info from a non-ASCII URL converts it to
// an origin and does punycode conversion as well as URL canonicalization.
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest,
                       SiteSettingsLinkWithNonAsciiUrl) {
  GURL url = GURL("http://🥄.ws/other/stuff.htm");
  std::string expected_origin = "http%3A%2F%2Fxn--9q9h.ws";
  EXPECT_EQ(GURL(chrome::kChromeUISiteDetailsPrefixURL + expected_origin),
            OpenSiteSettingsForUrl(browser(), url));
}

// Test opening "Site Details" via Page Info from an origin with a non-default
// (scheme, port) pair will specify port # in the origin passed to query params.
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest,
                       SiteSettingsLinkWithNonDefaultPort) {
  GURL url = GURL("https://www.example.com:8372");
  std::string expected_origin = "https%3A%2F%2Fwww.example.com%3A8372";
  EXPECT_EQ(GURL(chrome::kChromeUISiteDetailsPrefixURL + expected_origin),
            OpenSiteSettingsForUrl(browser(), url));
}

// Test opening "Site Details" via Page Info from about:blank goes to "Content
// Settings" (the alternative is a blank origin being sent to "Site Details").
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest,
                       SiteSettingsLinkWithAboutBlankURL) {
  EXPECT_EQ(GURL(chrome::kChromeUIContentSettingsURL),
            OpenSiteSettingsForUrl(browser(), GURL(url::kAboutBlankURL)));
}

// Test opening "Site Details" via Page Info from a file:// URL goes to "Content
// Settings".
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest,
                       SiteSettingsLinkWithFileUrl) {
  GURL url = GURL("file:///Users/homedirname/folder/file.pdf");
  EXPECT_EQ(GURL(chrome::kChromeUIContentSettingsURL),
            OpenSiteSettingsForUrl(browser(), url));
}

// Test opening page info bubble that matches SB_THREAT_TYPE_PASSWORD_REUSE
// threat type.
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest,
                       VerifyPasswordReusePageInfoBubble) {
  ASSERT_TRUE(embedded_test_server()->Start());
  base::HistogramTester histograms;
  histograms.ExpectTotalCount(kSyncPasswordPageInfoHistogramName, 0);
  ui_test_utils::NavigateToURL(browser(), embedded_test_server()->GetURL("/"));

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      safe_browsing::kGoogleBrandedPhishingWarning);
  // Update security state of the current page to match
  // SB_THREAT_TYPE_PASSWORD_REUSE.
  safe_browsing::ChromePasswordProtectionService* service =
      safe_browsing::ChromePasswordProtectionService::
          GetPasswordProtectionService(browser()->profile());
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  service->ShowModalWarning(contents, "token");
  base::RunLoop().RunUntilIdle();

  OpenPageInfoBubble(browser());
  views::View* change_password_button = GetView(
      browser(), PageInfoBubbleView::VIEW_ID_PAGE_INFO_BUTTON_CHANGE_PASSWORD);
  views::View* whitelist_password_reuse_button = GetView(
      browser(),
      PageInfoBubbleView::VIEW_ID_PAGE_INFO_BUTTON_WHITELIST_PASSWORD_REUSE);

  SecurityStateTabHelper* helper =
      SecurityStateTabHelper::FromWebContents(contents);
  security_state::SecurityInfo security_info;
  helper->GetSecurityInfo(&security_info);
  ASSERT_EQ(security_state::MALICIOUS_CONTENT_STATUS_PASSWORD_REUSE,
            security_info.malicious_content_status);

  // Verify these two buttons are showing.
  EXPECT_TRUE(change_password_button->visible());
  EXPECT_TRUE(whitelist_password_reuse_button->visible());

  // Verify clicking on button will increment corresponding bucket of
  // PasswordProtection.PageInfoAction.SyncPasswordEntry histogram.
  PerformMouseClickOnView(change_password_button);
  EXPECT_THAT(histograms.GetAllSamples(kSyncPasswordPageInfoHistogramName),
              testing::ElementsAre(base::Bucket(0 /*SHOWN*/, 1),
                                   base::Bucket(1 /*CHANGE_PASSWORD*/, 1)));

  PerformMouseClickOnView(whitelist_password_reuse_button);
  EXPECT_THAT(histograms.GetAllSamples(kSyncPasswordPageInfoHistogramName),
              testing::ElementsAre(base::Bucket(0 /*SHOWN*/, 1),
                                   base::Bucket(1 /*CHANGE_PASSWORD*/, 1),
                                   base::Bucket(4 /*MARK_AS_LEGITIMATE*/, 1)));
  // Security state will change after whitelisting.
  helper->GetSecurityInfo(&security_info);
  EXPECT_EQ(security_state::MALICIOUS_CONTENT_STATUS_NONE,
            security_info.malicious_content_status);
}

// Shows the Page Info bubble for a HTTP page (specifically, about:blank).
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest, InvokeDialog_Insecure) {
  RunDialog();
}

// Shows the Page Info bubble for a HTTPS page.
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest, InvokeDialog_Secure) {
  RunDialog();
}

// Shows the Page Info bubble for an internal page, e.g. chrome://settings.
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest, InvokeDialog_Internal) {
  RunDialog();
}

// Shows the Page Info bubble for an extensions page.
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest,
                       InvokeDialog_InternalExtension) {
  RunDialog();
}

// Shows the Page Info bubble for a chrome page that displays the source HTML.
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest,
                       InvokeDialog_InternalViewSource) {
  RunDialog();
}

// Shows the Page Info bubble for a site flagged for malware by Safe Browsing.
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest, InvokeDialog_Malware) {
  RunDialog();
}

// Shows the Page Info bubble for a site flagged for social engineering by Safe
// Browsing.
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest, InvokeDialog_Deceptive) {
  RunDialog();
}

// Shows the Page Info bubble for a site flagged for distributing unwanted
// software by Safe Browsing.
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest,
                       InvokeDialog_UnwantedSoftware) {
  RunDialog();
}

// Shows the Page Info bubble Safe Browsing soft warning after detecting the
// user has re-used an existing password on a site, e.g. due to phishing.
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest,
                       InvokeDialog_PasswordReuseSoft) {
  RunDialog();
}

// Shows the Page Info bubble Safe Browsing warning after detecting the user has
// re-used an existing password on a site, e.g. due to phishing.
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest,
                       InvokeDialog_PasswordReuse) {
  RunDialog();
}

// Shows the Page Info bubble for an admin-provided cert when the page is
// secure, but has a form that submits to an insecure url.
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest,
                       InvokeDialog_MixedContentForm) {
  RunDialog();
}

// Shows the Page Info bubble for an admin-provided cert when the page is
// secure, but it uses insecure resources (e.g. images).
IN_PROC_BROWSER_TEST_F(PageInfoBubbleViewBrowserTest,
                       InvokeDialog_MixedContent) {
  RunDialog();
}
