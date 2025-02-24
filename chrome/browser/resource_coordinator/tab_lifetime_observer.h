// Copyright (c) 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_RESOURCE_COORDINATOR_TAB_LIFETIME_OBSERVER_H_
#define CHROME_BROWSER_RESOURCE_COORDINATOR_TAB_LIFETIME_OBSERVER_H_

namespace content {
class WebContents;
}

namespace resource_coordinator {

// Interface to be notified of changes to the lifetime of tabs.
class TabLifetimeObserver {
 public:
  // Invoked after |contents| is discarded or reloaded after a discard.
  // |is_discarded| indicates if |contents| is currently discarded.
  virtual void OnDiscardedStateChange(content::WebContents* contents,
                                      bool is_discarded);

  // Invoked when
  virtual void OnAutoDiscardableStateChange(content::WebContents* contents,
                                            bool is_auto_discardable);

 protected:
  virtual ~TabLifetimeObserver();
};

}  // namespace resource_coordinator

#endif  // CHROME_BROWSER_RESOURCE_COORDINATOR_TAB_LIFETIME_OBSERVER_H_
