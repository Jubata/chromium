// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_ARC_INTENT_HELPER_ARC_INTENT_HELPER_BRIDGE_H_
#define COMPONENTS_ARC_INTENT_HELPER_ARC_INTENT_HELPER_BRIDGE_H_

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/macros.h"
#include "base/observer_list.h"
#include "base/threading/thread_checker.h"
#include "components/arc/common/intent_helper.mojom.h"
#include "components/arc/connection_observer.h"
#include "components/arc/intent_helper/activity_icon_loader.h"
#include "components/arc/intent_helper/arc_intent_helper_observer.h"
#include "components/keyed_service/core/keyed_service.h"
#include "mojo/public/cpp/bindings/binding.h"
#include "url/gurl.h"

class KeyedServiceBaseFactory;

namespace content {
class BrowserContext;
}  // namespace content

namespace arc {

class ArcBridgeService;
class IntentFilter;

// Receives intents from ARC.
class ArcIntentHelperBridge
    : public KeyedService,
      public ConnectionObserver<mojom::IntentHelperInstance>,
      public mojom::IntentHelperHost {
 public:
  // Returns singleton instance for the given BrowserContext,
  // or nullptr if the browser |context| is not allowed to use ARC.
  static ArcIntentHelperBridge* GetForBrowserContext(
      content::BrowserContext* context);

  // Returns factory for the ArcIntentHelperBridge.
  static KeyedServiceBaseFactory* GetFactory();

  // Appends '.' + |to_append| to the intent helper package name.
  static std::string AppendStringToIntentHelperPackageName(
      const std::string& to_append);

  ArcIntentHelperBridge(content::BrowserContext* context,
                        ArcBridgeService* bridge_service);
  ~ArcIntentHelperBridge() override;

  void AddObserver(ArcIntentHelperObserver* observer);
  void RemoveObserver(ArcIntentHelperObserver* observer);
  bool HasObserver(ArcIntentHelperObserver* observer) const;

  // ConnectionObserver<mojom::IntentHelperInstance>
  void OnConnectionReady() override;
  void OnConnectionClosed() override;

  // mojom::IntentHelperHost
  void OnIconInvalidated(const std::string& package_name) override;
  void OnIntentFiltersUpdated(
      std::vector<IntentFilter> intent_filters) override;
  void OnOpenDownloads() override;
  void OnOpenUrl(const std::string& url) override;
  void OnOpenChromeSettings(mojom::SettingsPage page) override;
  void OpenWallpaperPicker() override;
  void SetWallpaperDeprecated(const std::vector<uint8_t>& jpeg_data) override;
  void OpenVolumeControl() override;

  class OpenUrlDelegate {
   public:
    virtual ~OpenUrlDelegate() = default;

    // Opens the given URL in the Chrome browser.
    virtual void OpenUrl(const GURL& url) = 0;
  };

  // Retrieves icons for the |activities| and calls |callback|.
  // See ActivityIconLoader::GetActivityIcons() for more details.
  using ActivityName = internal::ActivityIconLoader::ActivityName;
  // A part of OnIconsReadyCallback signature.
  using ActivityToIconsMap = internal::ActivityIconLoader::ActivityToIconsMap;
  using OnIconsReadyCallback =
      internal::ActivityIconLoader::OnIconsReadyCallback;
  using GetResult = internal::ActivityIconLoader::GetResult;
  GetResult GetActivityIcons(const std::vector<ActivityName>& activities,
                             const OnIconsReadyCallback& callback);

  // Returns true when |url| can only be handled by Chrome. Otherwise, which is
  // when there might be one or more ARC apps that can handle |url|, returns
  // false. This function synchronously checks the |url| without making any IPC
  // to ARC side. Note that this function only supports http and https. If url's
  // scheme is neither http nor https, the function immediately returns true
  // without checking the filters.
  bool ShouldChromeHandleUrl(const GURL& url);

  // Returns false if |package_name| is for the intent_helper apk.
  static bool IsIntentHelperPackage(const std::string& package_name);

  // Filters out handlers that belong to the intent_helper apk and returns
  // a new array.
  static std::vector<mojom::IntentHandlerInfoPtr> FilterOutIntentHelper(
      std::vector<mojom::IntentHandlerInfoPtr> handlers);

  void SetOpenUrlDelegateForTesting(
      std::unique_ptr<OpenUrlDelegate> open_url_delegate);

  static const char kArcIntentHelperPackageName[];

 private:
  THREAD_CHECKER(thread_checker_);

  // Returns true if |url| is whitelisted. This function also returns true when
  // |url| is neither chrome:// nor about:.
  // TODO(yusukes): Properly fix b/68953603 and remove the function.
  bool IsWhitelistedChromeUrl(const GURL& url);

  content::BrowserContext* const context_;
  ArcBridgeService* const arc_bridge_service_;  // Owned by ArcServiceManager.

  mojo::Binding<mojom::IntentHelperHost> binding_;
  std::unique_ptr<OpenUrlDelegate> open_url_delegate_;
  internal::ActivityIconLoader icon_loader_;

  // List of intent filters from Android. Used to determine if Chrome should
  // handle a URL without handing off to Android.
  std::vector<IntentFilter> intent_filters_;

  base::ObserverList<ArcIntentHelperObserver> observer_list_;

  // TODO(yusukes): Properly fix b/68953603 and remove the variable.
  std::set<GURL> whitelisted_urls_;

  DISALLOW_COPY_AND_ASSIGN(ArcIntentHelperBridge);
};

}  // namespace arc

#endif  // COMPONENTS_ARC_INTENT_HELPER_ARC_INTENT_HELPER_BRIDGE_H_
