// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/login/profile_auth_data.h"

#include <memory>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/compiler_specific.h"
#include "base/memory/ptr_util.h"
#include "base/run_loop.h"
#include "base/strings/string16.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_constants.h"
#include "net/cookies/cookie_store.h"
#include "net/http/http_auth.h"
#include "net/http/http_auth_cache.h"
#include "net/http/http_network_session.h"
#include "net/http/http_transaction_factory.h"
#include "net/ssl/channel_id_service.h"
#include "net/ssl/channel_id_store.h"
#include "net/test/channel_id_test_util.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_getter.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace chromeos {

namespace {

const char kProxyAuthURL[] = "https://example.com/";
const char kProxyAuthRealm[] = "realm";
const char kProxyAuthChallenge[] = "challenge";
const char kProxyAuthPassword1[] = "password 1";
const char kProxyAuthPassword2[] = "password 2";

const char kGAIACookieURL[] = "https://google.com/";
const char kSAMLIdPCookieURL[] = "https://example.com/";
const char kCookieName[] = "cookie";
const char kCookieValue1[] = "value 1";
const char kCookieValue2[] = "value 2";
const char kGAIACookieDomain[] = "google.com";
const char kSAMLIdPCookieDomain[] = "example.com";
const char kSAMLIdPCookieDomainWithWildcard[] = ".example.com";

const char kChannelIDServerIdentifier[] = "server";

}  // namespace

class ProfileAuthDataTest : public testing::Test {
 public:
  // testing::Test:
  void SetUp() override;

  void PopulateUserBrowserContext();

  void Transfer(bool transfer_auth_cookies_and_channel_ids_on_first_login,
                bool transfer_saml_auth_cookies_on_subsequent_login);

  net::CookieList GetUserCookies();
  net::ChannelIDStore::ChannelIDList GetUserChannelIDs();

  void VerifyTransferredUserProxyAuthEntry();
  void VerifyUserCookies(const std::string& expected_gaia_cookie_value,
                         const std::string& expected_saml_idp_cookie_value);
  void VerifyUserChannelID(crypto::ECPrivateKey* expected_key);

 protected:
  std::unique_ptr<crypto::ECPrivateKey> channel_id_key1_;
  std::unique_ptr<crypto::ECPrivateKey> channel_id_key2_;

 private:
  void PopulateBrowserContext(
      content::BrowserContext* browser_context,
      const std::string& proxy_auth_password,
      const std::string& cookie_value,
      std::unique_ptr<crypto::ECPrivateKey> channel_id_key);

  net::URLRequestContext* GetRequestContext(
      content::BrowserContext* browser_context);
  net::HttpAuthCache* GetProxyAuth(content::BrowserContext* browser_context);
  net::CookieStore* GetCookies(content::BrowserContext* browser_context);
  net::ChannelIDStore* GetChannelIDs(content::BrowserContext* browser_context);

  void QuitLoop(const net::CookieList& ignored);
  void StoreCookieListAndQuitLoop(const net::CookieList& cookie_list);
  void StoreChannelIDListAndQuitLoop(
      const net::ChannelIDStore::ChannelIDList& channel_id_list);

  content::TestBrowserThreadBundle thread_bundle_;

  TestingProfile login_browser_context_;
  TestingProfile user_browser_context_;

  net::CookieList user_cookie_list_;
  net::ChannelIDStore::ChannelIDList user_channel_id_list_;

  std::unique_ptr<base::RunLoop> run_loop_;
};

void ProfileAuthDataTest::SetUp() {
  channel_id_key1_ = crypto::ECPrivateKey::Create();
  channel_id_key2_ = crypto::ECPrivateKey::Create();
  PopulateBrowserContext(&login_browser_context_, kProxyAuthPassword1,
                         kCookieValue1, channel_id_key1_->Copy());
}

void ProfileAuthDataTest::PopulateUserBrowserContext() {
  PopulateBrowserContext(&user_browser_context_, kProxyAuthPassword2,
                         kCookieValue2, channel_id_key2_->Copy());
}

void ProfileAuthDataTest::Transfer(
    bool transfer_auth_cookies_and_channel_ids_on_first_login,
    bool transfer_saml_auth_cookies_on_subsequent_login) {
  base::RunLoop run_loop;
  ProfileAuthData::Transfer(
      login_browser_context_.GetRequestContext(),
      user_browser_context_.GetRequestContext(),
      transfer_auth_cookies_and_channel_ids_on_first_login,
      transfer_saml_auth_cookies_on_subsequent_login, run_loop.QuitClosure());
  run_loop.Run();
  if (!transfer_auth_cookies_and_channel_ids_on_first_login &&
      !transfer_saml_auth_cookies_on_subsequent_login) {
    // When only proxy auth state is being transferred, the completion callback
    // is invoked before the transfer has actually completed. Spin the loop once
    // more to allow the transfer to complete.
    base::RunLoop().RunUntilIdle();
  }
}

net::CookieList ProfileAuthDataTest::GetUserCookies() {
  run_loop_.reset(new base::RunLoop);
  GetCookies(&user_browser_context_)
      ->GetAllCookiesAsync(
          base::BindOnce(&ProfileAuthDataTest::StoreCookieListAndQuitLoop,
                         base::Unretained(this)));
  run_loop_->Run();
  return user_cookie_list_;
}

net::ChannelIDStore::ChannelIDList ProfileAuthDataTest::GetUserChannelIDs() {
  run_loop_.reset(new base::RunLoop);
  GetChannelIDs(&user_browser_context_)
      ->GetAllChannelIDs(
          base::Bind(&ProfileAuthDataTest::StoreChannelIDListAndQuitLoop,
                     base::Unretained(this)));
  run_loop_->Run();
  return user_channel_id_list_;
}

void ProfileAuthDataTest::VerifyTransferredUserProxyAuthEntry() {
  net::HttpAuthCache::Entry* entry =
      GetProxyAuth(&user_browser_context_)
          ->Lookup(GURL(kProxyAuthURL), kProxyAuthRealm,
                   net::HttpAuth::AUTH_SCHEME_BASIC);
  ASSERT_TRUE(entry);
  EXPECT_EQ(base::ASCIIToUTF16(kProxyAuthPassword1),
            entry->credentials().password());
}

void ProfileAuthDataTest::VerifyUserCookies(
    const std::string& expected_gaia_cookie_value,
    const std::string& expected_saml_idp_cookie_value) {
  net::CookieList user_cookies = GetUserCookies();
  ASSERT_EQ(3u, user_cookies.size());

  // Cookies are returned chronoligically, in the order they were set.
  net::CanonicalCookie* cookie = &user_cookies[0];
  EXPECT_EQ(kCookieName, cookie->Name());
  EXPECT_EQ(expected_saml_idp_cookie_value, cookie->Value());
  EXPECT_EQ(kSAMLIdPCookieDomainWithWildcard, cookie->Domain());

  cookie = &user_cookies[1];
  EXPECT_EQ(kCookieName, cookie->Name());
  EXPECT_EQ(expected_saml_idp_cookie_value, cookie->Value());
  EXPECT_EQ(kSAMLIdPCookieDomain, cookie->Domain());

  cookie = &user_cookies[2];
  EXPECT_EQ(kCookieName, cookie->Name());
  EXPECT_EQ(expected_gaia_cookie_value, cookie->Value());
  EXPECT_EQ(kGAIACookieDomain, cookie->Domain());
}

void ProfileAuthDataTest::VerifyUserChannelID(
    crypto::ECPrivateKey* expected_key) {
  net::ChannelIDStore::ChannelIDList user_channel_ids = GetUserChannelIDs();
  ASSERT_EQ(1u, user_channel_ids.size());
  net::ChannelIDStore::ChannelID* channel_id = &user_channel_ids.front();
  EXPECT_EQ(kChannelIDServerIdentifier, channel_id->server_identifier());
  EXPECT_TRUE(net::KeysEqual(expected_key, channel_id->key()));
}

void ProfileAuthDataTest::PopulateBrowserContext(
    content::BrowserContext* browser_context,
    const std::string& proxy_auth_password,
    const std::string& cookie_value,
    std::unique_ptr<crypto::ECPrivateKey> channel_id_key) {
  GetProxyAuth(browser_context)
      ->Add(GURL(kProxyAuthURL), kProxyAuthRealm,
            net::HttpAuth::AUTH_SCHEME_BASIC, kProxyAuthChallenge,
            net::AuthCredentials(base::string16(),
                                 base::ASCIIToUTF16(proxy_auth_password)),
            std::string());

  net::CookieStore* cookies = GetCookies(browser_context);
  // Ensure |cookies| is fully initialized.
  run_loop_.reset(new base::RunLoop);
  cookies->GetAllCookiesAsync(
      base::BindOnce(&ProfileAuthDataTest::QuitLoop, base::Unretained(this)));
  run_loop_->Run();

  cookies->SetCookieWithDetailsAsync(
      GURL(kSAMLIdPCookieURL), kCookieName, cookie_value,
      kSAMLIdPCookieDomainWithWildcard, std::string(), base::Time(),
      base::Time(), base::Time(), true, false,
      net::CookieSameSite::DEFAULT_MODE, net::COOKIE_PRIORITY_DEFAULT,
      net::CookieStore::SetCookiesCallback());
  cookies->SetCookieWithDetailsAsync(
      GURL(kSAMLIdPCookieURL), kCookieName, cookie_value, std::string(),
      std::string(), base::Time(), base::Time(), base::Time(), true, false,
      net::CookieSameSite::DEFAULT_MODE, net::COOKIE_PRIORITY_DEFAULT,
      net::CookieStore::SetCookiesCallback());
  cookies->SetCookieWithDetailsAsync(
      GURL(kGAIACookieURL), kCookieName, cookie_value, std::string(),
      std::string(), base::Time(), base::Time(), base::Time(), true, false,
      net::CookieSameSite::DEFAULT_MODE, net::COOKIE_PRIORITY_DEFAULT,
      net::CookieStore::SetCookiesCallback());

  GetChannelIDs(browser_context)
      ->SetChannelID(base::MakeUnique<net::ChannelIDStore::ChannelID>(
          kChannelIDServerIdentifier, base::Time(), std::move(channel_id_key)));
}

net::URLRequestContext* ProfileAuthDataTest::GetRequestContext(
    content::BrowserContext* browser_context) {
  return content::BrowserContext::GetDefaultStoragePartition(browser_context)
      ->GetURLRequestContext()
      ->GetURLRequestContext();
}

net::HttpAuthCache* ProfileAuthDataTest::GetProxyAuth(
    content::BrowserContext* browser_context) {
  return GetRequestContext(browser_context)
      ->http_transaction_factory()
      ->GetSession()
      ->http_auth_cache();
}

net::CookieStore* ProfileAuthDataTest::GetCookies(
    content::BrowserContext* browser_context) {
  return GetRequestContext(browser_context)->cookie_store();
}

net::ChannelIDStore* ProfileAuthDataTest::GetChannelIDs(
    content::BrowserContext* browser_context) {
  return GetRequestContext(browser_context)
      ->channel_id_service()
      ->GetChannelIDStore();
}

void ProfileAuthDataTest::QuitLoop(const net::CookieList& ignored) {
  run_loop_->Quit();
}

void ProfileAuthDataTest::StoreCookieListAndQuitLoop(
    const net::CookieList& cookie_list) {
  user_cookie_list_ = cookie_list;
  run_loop_->Quit();
}

void ProfileAuthDataTest::StoreChannelIDListAndQuitLoop(
    const net::ChannelIDStore::ChannelIDList& channel_id_list) {
  user_channel_id_list_ = channel_id_list;
  run_loop_->Quit();
}

// Verifies that when no transfer of auth cookies or channel IDs is requested,
// only the proxy auth state is transferred.
TEST_F(ProfileAuthDataTest, DoNotTransfer) {
  Transfer(false, false);

  VerifyTransferredUserProxyAuthEntry();
  EXPECT_TRUE(GetUserCookies().empty());
  EXPECT_TRUE(GetUserChannelIDs().empty());
}

// Verifies that when the transfer of auth cookies and channel IDs on first
// login is requested, they do get transferred along with the proxy auth state
// on first login.
TEST_F(ProfileAuthDataTest, TransferOnFirstLoginWithNewProfile) {
  Transfer(true, false);

  VerifyTransferredUserProxyAuthEntry();
  VerifyUserCookies(kCookieValue1, kCookieValue1);
  VerifyUserChannelID(channel_id_key1_.get());
}

// Verifies that even if the transfer of auth cookies and channel IDs on first
// login is requested, only the proxy auth state is transferred on subsequent
// login.
TEST_F(ProfileAuthDataTest, TransferOnFirstLoginWithExistingProfile) {
  PopulateUserBrowserContext();

  Transfer(true, false);

  VerifyTransferredUserProxyAuthEntry();
  VerifyUserCookies(kCookieValue2, kCookieValue2);
  VerifyUserChannelID(channel_id_key2_.get());
}

// Verifies that when the transfer of auth cookies set by a SAML IdP on
// subsequent login is requested, they do get transferred along with the proxy
// auth state on subsequent login.
TEST_F(ProfileAuthDataTest, TransferOnSubsequentLogin) {
  PopulateUserBrowserContext();

  Transfer(false, true);

  VerifyTransferredUserProxyAuthEntry();
  VerifyUserCookies(kCookieValue2, kCookieValue1);
  VerifyUserChannelID(channel_id_key2_.get());
}

}  // namespace chromeos
