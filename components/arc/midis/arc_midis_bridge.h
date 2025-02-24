// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_ARC_MIDIS_ARC_MIDIS_BRIDGE_H_
#define COMPONENTS_ARC_MIDIS_ARC_MIDIS_BRIDGE_H_

#include <stdint.h>

#include <vector>

#include "base/macros.h"
#include "components/arc/common/midis.mojom.h"
#include "components/arc/connection_observer.h"
#include "components/keyed_service/core/keyed_service.h"
#include "mojo/public/cpp/bindings/binding.h"

namespace content {
class BrowserContext;
}  // namespace content

namespace arc {

class ArcBridgeService;

class ArcMidisBridge : public KeyedService,
                       public ConnectionObserver<mojom::MidisInstance>,
                       public mojom::MidisHost {
 public:
  // Returns singleton instance for the given BrowserContext,
  // or nullptr if the browser |context| is not allowed to use ARC.
  static ArcMidisBridge* GetForBrowserContext(content::BrowserContext* context);

  ArcMidisBridge(content::BrowserContext* context,
                 ArcBridgeService* bridge_service);
  ~ArcMidisBridge() override;

  // Overridden from ConnectionObserver<mojom::MidisInstance>:
  void OnConnectionReady() override;

  // Midis Mojo host interface
  void Connect(mojom::MidisServerRequest request,
               mojom::MidisClientPtr client_ptr) override;

 private:
  void OnBootstrapMojoConnection(mojom::MidisServerRequest request,
                                 mojom::MidisClientPtr client_ptr,
                                 bool result);

  ArcBridgeService* const arc_bridge_service_;  // Owned by ArcServiceManager.
  mojo::Binding<mojom::MidisHost> binding_;
  mojom::MidisHostPtr midis_host_ptr_;

  // WeakPtrFactory to use for callbacks.
  base::WeakPtrFactory<ArcMidisBridge> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(ArcMidisBridge);
};

}  // namespace arc

#endif  // COMPONENTS_ARC_MIDIS_ARC_MIDIS_BRIDGE_H_
