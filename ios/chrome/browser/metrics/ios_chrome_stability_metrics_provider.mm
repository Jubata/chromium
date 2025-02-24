// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ios/chrome/browser/metrics/ios_chrome_stability_metrics_provider.h"

#include "base/metrics/histogram_macros.h"
#include "ios/chrome/browser/chrome_url_constants.h"
#import "ios/web/public/web_state/navigation_context.h"
#import "ios/web/public/web_state/web_state.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

const char
    IOSChromeStabilityMetricsProvider::kPageLoadCountMigrationEventKey[] =
        "IOS.PageLoadCountMigration.Counts";

IOSChromeStabilityMetricsProvider::IOSChromeStabilityMetricsProvider(
    PrefService* local_state)
    : helper_(local_state), recording_enabled_(false) {}

IOSChromeStabilityMetricsProvider::~IOSChromeStabilityMetricsProvider() {}

void IOSChromeStabilityMetricsProvider::OnRecordingEnabled() {
  recording_enabled_ = true;
}

void IOSChromeStabilityMetricsProvider::OnRecordingDisabled() {
  recording_enabled_ = false;
}

void IOSChromeStabilityMetricsProvider::ProvideStabilityMetrics(
    metrics::SystemProfileProto* system_profile_proto) {
  helper_.ProvideStabilityMetrics(system_profile_proto);
}

void IOSChromeStabilityMetricsProvider::ClearSavedStabilityMetrics() {
  helper_.ClearSavedStabilityMetrics();
}

void IOSChromeStabilityMetricsProvider::LogRendererCrash() {
  if (!recording_enabled_)
    return;

  // The actual termination code isn't provided on iOS; use a dummy value.
  // TODO(blundell): Think about having StabilityMetricsHelper have a variant
  // that doesn't supply these arguments to make this cleaner.
  int dummy_termination_code = 105;
  helper_.LogRendererCrash(false /* not an extension process */,
                           base::TERMINATION_STATUS_ABNORMAL_TERMINATION,
                           dummy_termination_code);
}

void IOSChromeStabilityMetricsProvider::WebStateDidStartLoading(
    web::WebState* web_state) {
  if (!recording_enabled_)
    return;

  UMA_HISTOGRAM_ENUMERATION(kPageLoadCountMigrationEventKey,
                            StabilityMetricEventType::LOADING_STARTED,
                            StabilityMetricEventType::COUNT);
  helper_.LogLoadStarted();
}

void IOSChromeStabilityMetricsProvider::WebStateDidStartNavigation(
    web::WebState* web_state,
    web::NavigationContext* navigation_context) {
  if (!recording_enabled_)
    return;

  StabilityMetricEventType type =
      StabilityMetricEventType::PAGE_LOAD_NAVIGATION;
  if (navigation_context->GetUrl().SchemeIs(kChromeUIScheme)) {
    type = StabilityMetricEventType::CHROME_URL_NAVIGATION;
  } else if (navigation_context->IsSameDocument()) {
    type = StabilityMetricEventType::SAME_DOCUMENT_WEB_NAVIGATION;
  } else {
    // TODO(crbug.com/786547): Move helper_.LogLoadStarted() here.
  }
  UMA_HISTOGRAM_ENUMERATION(kPageLoadCountMigrationEventKey, type,
                            StabilityMetricEventType::COUNT);
}

void IOSChromeStabilityMetricsProvider::RenderProcessGone(
    web::WebState* web_state) {
  if (!recording_enabled_)
    return;
  LogRendererCrash();
  // TODO(crbug.com/685649): web_state->GetLastCommittedURL() is likely the URL
  // that caused a renderer crash and can be logged here.
}
