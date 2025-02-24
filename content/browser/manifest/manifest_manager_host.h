// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_MANIFEST_MANIFEST_MANAGER_HOST_H_
#define CONTENT_BROWSER_MANIFEST_MANIFEST_MANAGER_HOST_H_

#include "base/callback_forward.h"
#include "base/containers/id_map.h"
#include "base/macros.h"
#include "content/common/manifest_observer.mojom.h"
#include "content/public/browser/web_contents_binding_set.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/WebKit/public/platform/modules/manifest/manifest_manager.mojom.h"

namespace content {

class RenderFrameHost;
class WebContents;
struct Manifest;

// ManifestManagerHost is a helper class that allows callers to get the Manifest
// associated with the main frame of the observed WebContents. It handles the
// IPC messaging with the child process.
// TODO(mlamouri): keep a cached version and a dirty bit here.
class ManifestManagerHost : public WebContentsObserver,
                            public mojom::ManifestUrlChangeObserver {
 public:
  explicit ManifestManagerHost(WebContents* web_contents);
  ~ManifestManagerHost() override;

  using GetManifestCallback =
      base::Callback<void(const GURL&, const Manifest&)>;

  // Calls the given callback with the manifest associated with the main frame.
  // If the main frame has no manifest or if getting it failed the callback will
  // have an empty manifest.
  void GetManifest(const GetManifestCallback& callback);

  // WebContentsObserver
  void RenderFrameDeleted(RenderFrameHost* render_frame_host) override;

 private:
  using CallbackMap = base::IDMap<std::unique_ptr<GetManifestCallback>>;

  blink::mojom::ManifestManager& GetManifestManager();
  void OnConnectionError();

  void OnRequestManifestResponse(int request_id,
                                 const GURL& url,
                                 const base::Optional<Manifest>& manifest);

  // mojom::ManifestUrlChangeObserver:
  void ManifestUrlChanged(const base::Optional<GURL>& manifest_url) override;

  RenderFrameHost* manifest_manager_frame_ = nullptr;
  blink::mojom::ManifestManagerPtr manifest_manager_;
  CallbackMap callbacks_;

  WebContentsFrameBindingSet<mojom::ManifestUrlChangeObserver>
      manifest_url_change_observer_bindings_;

  DISALLOW_COPY_AND_ASSIGN(ManifestManagerHost);
};

}  // namespace content

#endif  // CONTENT_BROWSER_MANIFEST_MANIFEST_MANAGER_HOST_H_
