// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/arc/oemcrypto/arc_oemcrypto_bridge.h"

#include <utility>

#include "base/bind.h"
#include "base/memory/singleton.h"
#include "chrome/browser/chromeos/settings/cros_settings.h"
#include "chromeos/dbus/arc_oemcrypto_client.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "components/arc/arc_bridge_service.h"
#include "components/arc/arc_browser_context_keyed_service_factory_base.h"
#include "components/arc/common/protected_buffer_manager.mojom.h"
#include "content/public/browser/gpu_service_registry.h"
#include "mojo/edk/embedder/embedder.h"
#include "mojo/edk/embedder/outgoing_broker_client_invitation.h"
#include "mojo/edk/embedder/platform_channel_pair.h"
#include "mojo/edk/embedder/scoped_platform_handle.h"

namespace arc {
namespace {

// Singleton factory for ArcOemCryptoBridge
class ArcOemCryptoBridgeFactory
    : public internal::ArcBrowserContextKeyedServiceFactoryBase<
          ArcOemCryptoBridge,
          ArcOemCryptoBridgeFactory> {
 public:
  // Factory name used by ArcBrowserContextKeyedServiceFactoryBase.
  static constexpr const char* kName = "ArcOemCryptoBridgeFactory";

  static ArcOemCryptoBridgeFactory* GetInstance() {
    return base::Singleton<ArcOemCryptoBridgeFactory>::get();
  }

 private:
  friend base::DefaultSingletonTraits<ArcOemCryptoBridgeFactory>;
  ArcOemCryptoBridgeFactory() = default;
  ~ArcOemCryptoBridgeFactory() override = default;
};

}  // namespace

// static
ArcOemCryptoBridge* ArcOemCryptoBridge::GetForBrowserContext(
    content::BrowserContext* context) {
  return ArcOemCryptoBridgeFactory::GetForBrowserContext(context);
}

ArcOemCryptoBridge::ArcOemCryptoBridge(content::BrowserContext* context,
                                       ArcBridgeService* bridge_service)
    : arc_bridge_service_(bridge_service), binding_(this), weak_factory_(this) {
  arc_bridge_service_->oemcrypto()->AddObserver(this);
}

ArcOemCryptoBridge::~ArcOemCryptoBridge() {
  arc_bridge_service_->oemcrypto()->RemoveObserver(this);
}

void ArcOemCryptoBridge::OnConnectionReady() {
  DVLOG(1) << "ArcOemCryptoBridge::OnConnectionReady() called";
  mojom::OemCryptoInstance* oemcrypto_instance =
      ARC_GET_INSTANCE_FOR_METHOD(arc_bridge_service_->oemcrypto(), Init);
  DCHECK(oemcrypto_instance);

  DVLOG(1) << "Calling Init back on other side of OemCrypto";
  mojom::OemCryptoHostPtr host_proxy;
  binding_.Bind(mojo::MakeRequest(&host_proxy));
  oemcrypto_instance->Init(std::move(host_proxy));
  binding_.set_connection_error_handler(base::Bind(
      &mojo::Binding<OemCryptoHost>::Close, base::Unretained(&binding_)));
}

void ArcOemCryptoBridge::OnBootstrapMojoConnection(
    mojom::OemCryptoServiceRequest request,
    bool result) {
  if (!result) {
    // This can currently happen due to limited device support, so do not log
    // it as an error.
    DVLOG(1) << "ArcOemCryptoBridge had a failure in D-Bus with the daemon";
    // Reset this so we don't think it is bound on future calls to Connect.
    oemcrypto_host_daemon_ptr_.reset();
    return;
  }
  DVLOG(1) << "ArcOemCryptoBridge succeeded with Mojo bootstrapping.";
  ConnectToDaemon(std::move(request));
}

void ArcOemCryptoBridge::Connect(mojom::OemCryptoServiceRequest request) {
  DVLOG(1) << "ArcOemCryptoBridge::Connect called";

  // Check that the user has Attestation for Content Protection enabled in
  // their Chrome settings and if they do not then block this connection since
  // OEMCrypto utilizes Attestation as the root of trust for its DRM
  // implementation.
  bool attestation_enabled = false;
  if (!chromeos::CrosSettings::Get()->GetBoolean(
          chromeos::kAttestationForContentProtectionEnabled,
          &attestation_enabled)) {
    LOG(ERROR) << "Failed to get attestation device setting";
    return;
  }
  if (!attestation_enabled) {
    DVLOG(1) << "OEMCrypto L1 DRM denied because Verified Access is disabled "
                "for this device.";
    return;
  }

  if (oemcrypto_host_daemon_ptr_.is_bound()) {
    DVLOG(1) << "Re-using bootstrap connection for OemCryptoService Connect";
    ConnectToDaemon(std::move(request));
    return;
  }
  DVLOG(1) << "Bootstrapping the OemCrypto connection via D-Bus";
  mojo::edk::OutgoingBrokerClientInvitation invitation;
  mojo::edk::PlatformChannelPair channel_pair;
  mojo::ScopedMessagePipeHandle server_pipe =
      invitation.AttachMessagePipe("arc-oemcrypto-pipe");
  invitation.Send(
      base::kNullProcessHandle,
      mojo::edk::ConnectionParams(mojo::edk::TransportProtocol::kLegacy,
                                  channel_pair.PassServerHandle()));
  mojo::edk::ScopedPlatformHandle child_handle =
      channel_pair.PassClientHandle();
  base::ScopedFD fd(child_handle.release().handle);

  // Bind the Mojo pipe to the interface before we send the D-Bus message
  // to avoid any kind of race condition with detecting it's been bound.
  // It's safe to do this before the other end binds anyways.
  oemcrypto_host_daemon_ptr_.Bind(
      mojo::InterfacePtrInfo<arc_oemcrypto::mojom::OemCryptoHostDaemon>(
          std::move(server_pipe), 0u));
  DVLOG(1) << "Bound remote OemCryptoHostDaemon interface to pipe";
  oemcrypto_host_daemon_ptr_.set_connection_error_handler(base::Bind(
      &mojo::InterfacePtr<arc_oemcrypto::mojom::OemCryptoHostDaemon>::reset,
      base::Unretained(&oemcrypto_host_daemon_ptr_)));
  chromeos::DBusThreadManager::Get()
      ->GetArcOemCryptoClient()
      ->BootstrapMojoConnection(
          std::move(fd),
          base::Bind(&ArcOemCryptoBridge::OnBootstrapMojoConnection,
                     weak_factory_.GetWeakPtr(), base::Passed(&request)));
}

void ArcOemCryptoBridge::ConnectToDaemon(
    mojom::OemCryptoServiceRequest request) {
  // Get the Mojo interface from the GPU for dealing with secure buffers and
  // pass that to the daemon as well in our Connect call.
  mojom::ProtectedBufferManagerPtr gpu_buffer_manager;
  content::BindInterfaceInGpuProcess(mojo::MakeRequest(&gpu_buffer_manager));
  oemcrypto_host_daemon_ptr_->Connect(std::move(request),
                                      std::move(gpu_buffer_manager));
}

}  // namespace arc
