// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MEDIA_ROUTER_MOJO_MEDIA_ROUTER_DESKTOP_H_
#define CHROME_BROWSER_MEDIA_ROUTER_MOJO_MEDIA_ROUTER_DESKTOP_H_

#include "base/gtest_prod_util.h"
#include "build/build_config.h"
#include "chrome/browser/media/router/mojo/extension_media_route_provider_proxy.h"
#include "chrome/browser/media/router/mojo/media_router_mojo_impl.h"

namespace content {
class RenderFrameHost;
}

namespace extensions {
class Extension;
}

namespace media_router {
class CastMediaSinkService;
class DialMediaSinkServiceProxy;
class WiredDisplayMediaRouteProvider;

// MediaRouter implementation that uses the MediaRouteProvider implemented in
// the component extension.
class MediaRouterDesktop : public MediaRouterMojoImpl {
 public:
  ~MediaRouterDesktop() override;

  // Sets up the MediaRouter instance owned by |context| to handle
  // MediaRouterObserver requests from the component extension given by
  // |extension|. Creates the MediaRouterMojoImpl instance if it does not
  // exist.
  // Called by the Mojo module registry.
  // |extension|: The component extension, used for querying
  //     suspension state.
  // |context|: The BrowserContext which owns the extension process.
  // |request|: The Mojo connection request used for binding.
  static void BindToRequest(const extensions::Extension* extension,
                            content::BrowserContext* context,
                            mojom::MediaRouterRequest request,
                            content::RenderFrameHost* source);

  // MediaRouter implementation.
  void OnUserGesture() override;

 private:
  friend class MediaRouterDesktopTest;
  friend class MediaRouterDesktopTestTest;
  friend class MediaRouterFactory;
  FRIEND_TEST_ALL_PREFIXES(MediaRouterDesktopTest, TestProvideSinks);

  enum class FirewallCheck {
    // Skips the firewall check for the benefit of unit tests so they do not
    // have to depend on the system's firewall configuration.
    SKIP_FOR_TESTING,
    // Perform the firewall check (default).
    RUN,
  };

  MediaRouterDesktop(content::BrowserContext* context,
                     FirewallCheck check_firewall = FirewallCheck::RUN);

  // mojom::MediaRouter implementation.
  void RegisterMediaRouteProvider(
      mojom::MediaRouteProvider::Id provider_id,
      mojom::MediaRouteProviderPtr media_route_provider_ptr,
      mojom::MediaRouter::RegisterMediaRouteProviderCallback callback) override;

  // Registers a Mojo pointer to the extension MRP with
  // |extension_provider_proxy_| and does initializations specific to the
  // extension MRP.
  void RegisterExtensionMediaRouteProvider(
      mojom::MediaRouteProviderPtr extension_provider_ptr);

  // Binds |this| to a Mojo interface request, so that clients can acquire a
  // handle to a MediaRouter instance via the Mojo service connector.
  // Passes the extension's ID to the event page request manager.
  void BindToMojoRequest(mojo::InterfaceRequest<mojom::MediaRouter> request,
                         const extensions::Extension& extension);

  // Starts browser side sink discovery.
  void StartDiscovery();

  // Notifies the Media Router that the list of MediaSinks discovered by a
  // MediaSinkService has been updated.
  // |provider_name|: Name of the MediaSinkService providing the sinks.
  // |sinks|: sinks discovered by MediaSinkService.
  void ProvideSinks(const std::string& provider_name,
                    std::vector<MediaSinkInternal> sinks);

  // Initializes MRPs and adds them to |media_route_providers_|.
  void InitializeMediaRouteProviders();

  // Helper methods for InitializeMediaRouteProviders().
  void InitializeExtensionMediaRouteProviderProxy();
  void InitializeWiredDisplayMediaRouteProvider();

#if defined(OS_WIN)
  // Ensures that mDNS discovery is enabled in the MRPM extension. This can be
  // called many times but the MRPM will only be called once per registration
  // period.
  void EnsureMdnsDiscoveryEnabled();

  // Callback used to enable mDNS in the MRPM if a firewall prompt will not be
  // triggered. If a firewall prompt would be triggered, enabling mDNS won't
  // happen until the user is clearly interacting with MR.
  void OnFirewallCheckComplete(bool firewall_can_use_local_ports);
#endif

  // MediaRouteProvider proxy that forwards calls to the MRPM in the component
  // extension.
  std::unique_ptr<ExtensionMediaRouteProviderProxy> extension_provider_proxy_;

  // MediaRouteProvider for casting to local screens.
  std::unique_ptr<WiredDisplayMediaRouteProvider> wired_display_provider_;

  // Media sink service for DIAL devices.
  scoped_refptr<DialMediaSinkServiceProxy> dial_media_sink_service_proxy_;

  // Media sink service for CAST devices.
  scoped_refptr<CastMediaSinkService> cast_media_sink_service_;

  // A flag to ensure that we record the provider version once, during the
  // initial event page wakeup attempt.
  bool provider_version_was_recorded_ = false;

#if defined(OS_WIN)
  // A flag to ensure that mDNS discovery is only enabled on Windows when there
  // will be appropriate context for the user to associate a firewall prompt
  // with Media Router. |should_enable_mdns_discovery_| can only go from
  // |false| to |true|.
  bool should_enable_mdns_discovery_ = false;
#endif

  base::WeakPtrFactory<MediaRouterDesktop> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(MediaRouterDesktop);
};

}  // namespace media_router

#endif  // CHROME_BROWSER_MEDIA_ROUTER_MOJO_MEDIA_ROUTER_DESKTOP_H_
