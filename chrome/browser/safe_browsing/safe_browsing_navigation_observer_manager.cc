// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/safe_browsing/safe_browsing_navigation_observer_manager.h"

#include "base/memory/ptr_util.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/safe_browsing/safe_browsing_navigation_observer.h"
#include "chrome/browser/safe_browsing/safe_browsing_service.h"
#include "chrome/browser/sessions/session_tab_helper.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/safe_browsing/common/utils.h"
#include "content/public/browser/navigation_details.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"

using content::WebContents;

namespace safe_browsing {

namespace {

// Given when an event happened and its TTL, determine if it is already expired.
// Note, if for some reason this event's timestamp is in the future, this
// event's timestamp is invalid, hence we treat it as expired.
bool IsEventExpired(const base::Time& event_time, double ttl_in_second) {
  double current_time_in_second = base::Time::Now().ToDoubleT();
  double event_time_in_second = event_time.ToDoubleT();
  if (current_time_in_second <= event_time_in_second)
    return true;
  return current_time_in_second - event_time_in_second > ttl_in_second;
}

// Helper function to determine if the URL type should be LANDING_REFERRER or
// LANDING_PAGE, and modify AttributionResult accordingly.
ReferrerChainEntry::URLType GetURLTypeAndAdjustAttributionResult(
    bool at_user_gesture_limit,
    SafeBrowsingNavigationObserverManager::AttributionResult* out_result) {
  // Landing page refers to the page user directly interacts with to trigger
  // this event (e.g. clicking on download button). Landing referrer page is the
  // one user interacts with right before navigating to the landing page.
  // Since we are tracing navigations backwards, if we've reached
  // user gesture limit before this navigation event, this is a navigation
  // leading to the landing referrer page, otherwise it leads to landing page.
  if (at_user_gesture_limit) {
    *out_result =
        SafeBrowsingNavigationObserverManager::SUCCESS_LANDING_REFERRER;
    return ReferrerChainEntry::LANDING_REFERRER;
  } else {
    *out_result = SafeBrowsingNavigationObserverManager::SUCCESS_LANDING_PAGE;
    return ReferrerChainEntry::LANDING_PAGE;
  }
}

std::string GetOrigin(const std::string& url) {
  return GURL(url).GetOrigin().spec();
}

}  // namespace

// The expiration period of a user gesture. Any user gesture that happened 1.0
// second ago is considered as expired and not relevant to upcoming navigation
// events.
static const double kUserGestureTTLInSecond = 1.0;
// The expiration period of navigation events and resolved IP addresses. Any
// navigation related records that happened 2 minutes ago are considered as
// expired. So we clean up these navigation footprints every 2 minutes.
static const double kNavigationFootprintTTLInSecond = 120.0;
// The maximum number of latest NavigationEvent we keep. It is used to limit
// memory usage of navigation tracking. This number if picked based on UMA
// metric "SafeBrowsing.NavigationObserver.NavigationEventCleanUpCount".
// Lowering it could make room for abuse.
static const int kNavigationRecordMaxSize = 100;
// The maximum number of ReferrerChainEntry. It is used to limit the size of
// reports (e.g. ClientDownloadRequest) we send to SB server.
static const int kReferrerChainMaxLength = 10;

// -------------------------ReferrerChainData-----------------------
ReferrerChainData::ReferrerChainData(
    std::unique_ptr<ReferrerChain> referrer_chain)
    : referrer_chain_(std::move(referrer_chain)) {}

ReferrerChainData::~ReferrerChainData() {}

ReferrerChain* ReferrerChainData::GetReferrerChain() {
  return referrer_chain_.get();
}

// -------------------------NavigationEventList---------------------
NavigationEventList::NavigationEventList(std::size_t size_limit)
    : size_limit_(size_limit) {
  DCHECK_GT(size_limit_, 0U);
}

NavigationEventList::~NavigationEventList() {}

NavigationEvent* NavigationEventList::FindNavigationEvent(
    const GURL& target_url,
    const GURL& target_main_frame_url,
    int target_tab_id) {
  if (target_url.is_empty() && target_main_frame_url.is_empty())
    return nullptr;

  // If target_url is empty, we should back trace navigation based on its
  // main frame URL instead.
  GURL search_url = target_url.is_empty() ? target_main_frame_url : target_url;

  // Since navigation events are recorded in chronological order, we traverse
  // the vector in reverse order to get the latest match.
  for (auto rit = navigation_events_.rbegin(); rit != navigation_events_.rend();
       ++rit) {
    auto* nav_event = rit->get();
    // If tab id is not valid, we only compare url, otherwise we compare both.
    if (nav_event->GetDestinationUrl() == search_url &&
        (target_tab_id == -1 || nav_event->target_tab_id == target_tab_id)) {
      // If both source_url and source_main_frame_url are empty, and this
      // navigation is not triggered by user, a retargeting navigation probably
      // causes this navigation. In this case, we skip this navigation event and
      // looks for the retargeting navigation event.
      if (nav_event->source_url.is_empty() &&
          nav_event->source_main_frame_url.is_empty() &&
          !nav_event->is_user_initiated) {
        // If there is a server redirection immediately after retargeting, we
        // need to adjust our search url to the original request.
        if (!nav_event->server_redirect_urls.empty()) {
          NavigationEvent* retargeting_nav_event =
              FindRetargetingNavigationEvent(nav_event->original_request_url,
                                             nav_event->target_tab_id);
          if (!retargeting_nav_event)
            return nullptr;
          // Adjust retargeting navigation event's attributes.
          retargeting_nav_event->server_redirect_urls.push_back(
              std::move(search_url));
          return retargeting_nav_event;
        } else {
          continue;
        }
      } else {
        return nav_event;
      }
    }
  }
  return nullptr;
}

NavigationEvent* NavigationEventList::FindRetargetingNavigationEvent(
    const GURL& target_url,
    int target_tab_id) {
  if (target_url.is_empty())
    return nullptr;

  // Since navigation events are recorded in chronological order, we traverse
  // the vector in reverse order to get the latest match.
  for (auto rit = navigation_events_.rbegin(); rit != navigation_events_.rend();
       ++rit) {
    auto* nav_event = rit->get();
    // In addition to url and tab_id checking, we need to compare the
    // source_tab_id and target_tab_id to make sure it is a retargeting event.
    if (nav_event->original_request_url == target_url &&
        nav_event->target_tab_id == target_tab_id &&
        nav_event->source_tab_id != nav_event->target_tab_id) {
      return nav_event;
    }
  }
  return nullptr;
}

void NavigationEventList::RecordNavigationEvent(
    std::unique_ptr<NavigationEvent> nav_event) {
  // Skip page refresh and in-page navigation.
  if (nav_event->source_url == nav_event->GetDestinationUrl() &&
      nav_event->source_tab_id == nav_event->target_tab_id)
    return;

  if (navigation_events_.size() == size_limit_)
    navigation_events_.pop_front();
  navigation_events_.push_back(std::move(nav_event));
}

std::size_t NavigationEventList::CleanUpNavigationEvents() {
  // Remove any stale NavigationEnvent, if it is older than
  // kNavigationFootprintTTLInSecond.
  std::size_t removal_count = 0;
  while (navigation_events_.size() > 0 &&
         IsEventExpired(navigation_events_[0]->last_updated,
                        kNavigationFootprintTTLInSecond)) {
    navigation_events_.pop_front();
    removal_count++;
  }
  return removal_count;
}

// -----------------SafeBrowsingNavigationObserverManager-----------
// static
bool SafeBrowsingNavigationObserverManager::IsUserGestureExpired(
    const base::Time& timestamp) {
  return IsEventExpired(timestamp, kUserGestureTTLInSecond);
}

// static
GURL SafeBrowsingNavigationObserverManager::ClearURLRef(const GURL& url) {
  if (url.has_ref()) {
    url::Replacements<char> replacements;
    replacements.ClearRef();
    return url.ReplaceComponents(replacements);
  }
  return url;
}

// static
bool SafeBrowsingNavigationObserverManager::IsEnabledAndReady(
    Profile* profile) {
  return profile->GetPrefs()->GetBoolean(prefs::kSafeBrowsingEnabled) &&
         g_browser_process->safe_browsing_service() &&
         g_browser_process->safe_browsing_service()
             ->navigation_observer_manager();
}

// static
void SafeBrowsingNavigationObserverManager::SanitizeReferrerChain(
    ReferrerChain* referrer_chain) {
  for (int i = 0; i < referrer_chain->size(); i++) {
    ReferrerChainEntry* entry = referrer_chain->Mutable(i);
    ReferrerChainEntry entry_copy(*entry);
    entry->Clear();
    if (entry_copy.has_url())
      entry->set_url(GetOrigin(entry_copy.url()));
    if (entry_copy.has_main_frame_url())
      entry->set_main_frame_url(GetOrigin(entry_copy.main_frame_url()));
    entry->set_type(entry_copy.type());
    for (int j = 0; j < entry_copy.ip_addresses_size(); j++)
      entry->add_ip_addresses(entry_copy.ip_addresses(j));
    if (entry_copy.has_referrer_url())
      entry->set_referrer_url(GetOrigin(entry_copy.referrer_url()));
    if (entry_copy.has_referrer_main_frame_url())
      entry->set_referrer_main_frame_url(
          GetOrigin(entry_copy.referrer_main_frame_url()));
    entry->set_is_retargeting(entry_copy.is_retargeting());
    entry->set_navigation_time_msec(entry_copy.navigation_time_msec());
    for (int j = 0; j < entry_copy.server_redirect_chain_size(); j++) {
      ReferrerChainEntry::ServerRedirect* server_redirect_entry =
          entry->add_server_redirect_chain();
      if (entry_copy.server_redirect_chain(j).has_url()) {
        server_redirect_entry->set_url(
            GetOrigin(entry_copy.server_redirect_chain(j).url()));
      }
    }
  }
}

SafeBrowsingNavigationObserverManager::SafeBrowsingNavigationObserverManager()
    : navigation_event_list_(kNavigationRecordMaxSize) {

  // Schedule clean up in 2 minutes.
  ScheduleNextCleanUpAfterInterval(
      base::TimeDelta::FromSecondsD(kNavigationFootprintTTLInSecond));
}

void SafeBrowsingNavigationObserverManager::RecordNavigationEvent(
    std::unique_ptr<NavigationEvent> nav_event) {
  navigation_event_list_.RecordNavigationEvent(std::move(nav_event));
}

void SafeBrowsingNavigationObserverManager::RecordUserGestureForWebContents(
    content::WebContents* web_contents,
    const base::Time& timestamp) {
  auto insertion_result =
      user_gesture_map_.insert(std::make_pair(web_contents, timestamp));
  // Update the timestamp if entry already exists.
  if (!insertion_result.second)
    insertion_result.first->second = timestamp;
}

void SafeBrowsingNavigationObserverManager::OnUserGestureConsumed(
    content::WebContents* web_contents,
    const base::Time& timestamp) {
  auto it = user_gesture_map_.find(web_contents);
  // Remove entry from |user_gesture_map_| as a user_gesture is consumed by
  // a navigation event.
  if (it != user_gesture_map_.end() && timestamp >= it->second)
    user_gesture_map_.erase(it);
}

bool SafeBrowsingNavigationObserverManager::HasUserGesture(
    content::WebContents* web_contents) {
  if (!web_contents)
    return false;
  if (user_gesture_map_.find(web_contents) != user_gesture_map_.end())
    return true;
  return false;
}

void SafeBrowsingNavigationObserverManager::RecordHostToIpMapping(
    const std::string& host,
    const std::string& ip) {
  auto insert_result = host_to_ip_map_.insert(
      std::make_pair(host, std::vector<ResolvedIPAddress>()));
  if (!insert_result.second) {
    // host_to_ip_map already contains this key.
    // If this IP is already in the vector, we update its timestamp.
    for (auto& vector_entry : insert_result.first->second) {
      if (vector_entry.ip == ip) {
        vector_entry.timestamp = base::Time::Now();
        return;
      }
    }
  }
  // If this is a new IP of this host, and we added to the end of the vector.
  insert_result.first->second.push_back(
      ResolvedIPAddress(base::Time::Now(), ip));
}

void SafeBrowsingNavigationObserverManager::OnWebContentDestroyed(
    content::WebContents* web_contents) {
  user_gesture_map_.erase(web_contents);
}

void SafeBrowsingNavigationObserverManager::CleanUpStaleNavigationFootprints() {
  CleanUpNavigationEvents();
  CleanUpUserGestures();
  CleanUpIpAddresses();
  ScheduleNextCleanUpAfterInterval(
      base::TimeDelta::FromSecondsD(kNavigationFootprintTTLInSecond));
}

SafeBrowsingNavigationObserverManager::AttributionResult
SafeBrowsingNavigationObserverManager::IdentifyReferrerChainByEventURL(
    const GURL& event_url,
    int event_tab_id,
    int user_gesture_count_limit,
    ReferrerChain* out_referrer_chain) {
  if (!event_url.is_valid())
    return INVALID_URL;

  NavigationEvent* nav_event = navigation_event_list_.FindNavigationEvent(
      ClearURLRef(event_url), GURL(), event_tab_id);
  if (!nav_event) {
    // We cannot find a single navigation event related to this event.
    return NAVIGATION_EVENT_NOT_FOUND;
  }
  AttributionResult result = SUCCESS;
  AddToReferrerChain(out_referrer_chain, nav_event, GURL(),
                     ReferrerChainEntry::EVENT_URL);
  int user_gesture_count = 0;
  GetRemainingReferrerChain(
      nav_event,
      user_gesture_count,
      user_gesture_count_limit,
      out_referrer_chain,
      &result);
  return result;
}

SafeBrowsingNavigationObserverManager::AttributionResult
SafeBrowsingNavigationObserverManager::IdentifyReferrerChainByWebContents(
    content::WebContents* web_contents,
    int user_gesture_count_limit,
    ReferrerChain* out_referrer_chain) {
  if (!web_contents)
    return INVALID_URL;
  GURL last_committed_url = web_contents->GetLastCommittedURL();
  if (!last_committed_url.is_valid())
    return INVALID_URL;
  bool has_user_gesture = HasUserGesture(web_contents);
  int tab_id = SessionTabHelper::IdForTab(web_contents);
  return IdentifyReferrerChainByHostingPage(
      ClearURLRef(last_committed_url), GURL(), tab_id, has_user_gesture,
      user_gesture_count_limit, out_referrer_chain);
}

SafeBrowsingNavigationObserverManager::AttributionResult
SafeBrowsingNavigationObserverManager::IdentifyReferrerChainByHostingPage(
    const GURL& initiating_frame_url,
    const GURL& initiating_main_frame_url,
    int tab_id,
    bool has_user_gesture,
    int user_gesture_count_limit,
    ReferrerChain* out_referrer_chain) {
  if (!initiating_frame_url.is_valid())
    return INVALID_URL;

  NavigationEvent* nav_event = navigation_event_list_.FindNavigationEvent(
      ClearURLRef(initiating_frame_url), ClearURLRef(initiating_main_frame_url),
      tab_id);
  if (!nav_event) {
    // We cannot find a single navigation event related to this hosting page.
    return NAVIGATION_EVENT_NOT_FOUND;
  }

  AttributionResult result = SUCCESS;

  int user_gesture_count = 0;
  // If this initiating_frame has user gesture, we consider this as the landing
  // page of this event.
  if (has_user_gesture) {
    user_gesture_count = 1;
    AddToReferrerChain(
        out_referrer_chain, nav_event, initiating_main_frame_url,
        GetURLTypeAndAdjustAttributionResult(
            user_gesture_count == user_gesture_count_limit, &result));
  } else {
    AddToReferrerChain(out_referrer_chain, nav_event, initiating_main_frame_url,
                       ReferrerChainEntry::CLIENT_REDIRECT);
  }

  GetRemainingReferrerChain(
      nav_event,
      user_gesture_count,
      user_gesture_count_limit,
      out_referrer_chain,
      &result);
  return result;
}

SafeBrowsingNavigationObserverManager::
    ~SafeBrowsingNavigationObserverManager() {}

void SafeBrowsingNavigationObserverManager::RecordNewWebContents(
    content::WebContents* source_web_contents,
    int source_render_process_id,
    int source_render_frame_id,
    GURL target_url,
    content::WebContents* target_web_contents,
    bool not_yet_in_tabstrip) {
  DCHECK(source_web_contents);
  DCHECK(target_web_contents);

  content::RenderFrameHost* rfh = content::RenderFrameHost::FromID(
      source_render_process_id, source_render_frame_id);
  // Remove the "#" at the end of URL, since it does not point to any actual
  // page fragment ID.
  GURL cleaned_target_url =
      SafeBrowsingNavigationObserverManager::ClearURLRef(target_url);

  std::unique_ptr<NavigationEvent> nav_event =
      base::MakeUnique<NavigationEvent>();
  if (rfh) {
    nav_event->source_url = SafeBrowsingNavigationObserverManager::ClearURLRef(
        rfh->GetLastCommittedURL());
  }
  nav_event->source_tab_id = SessionTabHelper::IdForTab(source_web_contents);
  nav_event->source_main_frame_url =
      SafeBrowsingNavigationObserverManager::ClearURLRef(
          source_web_contents->GetLastCommittedURL());
  nav_event->original_request_url = cleaned_target_url;
  nav_event->target_tab_id = SessionTabHelper::IdForTab(target_web_contents);
  nav_event->frame_id = rfh ? rfh->GetFrameTreeNodeId() : -1;
  auto it = user_gesture_map_.find(source_web_contents);
  if (it != user_gesture_map_.end() &&
      !SafeBrowsingNavigationObserverManager::IsUserGestureExpired(
          it->second)) {
    nav_event->is_user_initiated = true;
    OnUserGestureConsumed(it->first, it->second);
  } else {
    nav_event->is_user_initiated = false;
  }

  navigation_event_list_.RecordNavigationEvent(std::move(nav_event));
}

void SafeBrowsingNavigationObserverManager::CleanUpNavigationEvents() {
  std::size_t removal_count = navigation_event_list_.CleanUpNavigationEvents();

  UMA_HISTOGRAM_COUNTS_10000(
      "SafeBrowsing.NavigationObserver.NavigationEventCleanUpCount",
      removal_count);
}

void SafeBrowsingNavigationObserverManager::CleanUpUserGestures() {
  for (auto it = user_gesture_map_.begin(); it != user_gesture_map_.end();) {
    if (IsEventExpired(it->second, kNavigationFootprintTTLInSecond))
      it = user_gesture_map_.erase(it);
    else
      ++it;
  }
}

void SafeBrowsingNavigationObserverManager::CleanUpIpAddresses() {
  std::size_t remove_count = 0;
  for (auto it = host_to_ip_map_.begin(); it != host_to_ip_map_.end();) {
    std::size_t size_before_removal = it->second.size();
    it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
                                    [](const ResolvedIPAddress& resolved_ip) {
                                      return IsEventExpired(
                                          resolved_ip.timestamp,
                                          kNavigationFootprintTTLInSecond);
                                    }),
                     it->second.end());
    std::size_t size_after_removal = it->second.size();
    remove_count += (size_before_removal - size_after_removal);
    if (size_after_removal == 0)
      it = host_to_ip_map_.erase(it);
    else
      ++it;
  }
  UMA_HISTOGRAM_COUNTS_10000(
      "SafeBrowsing.NavigationObserver.IPAddressCleanUpCount", remove_count);
}

bool SafeBrowsingNavigationObserverManager::IsCleanUpScheduled() const {
  return cleanup_timer_.IsRunning();
}

void SafeBrowsingNavigationObserverManager::ScheduleNextCleanUpAfterInterval(
    base::TimeDelta interval) {
  DCHECK_GT(interval, base::TimeDelta());
  cleanup_timer_.Stop();
  cleanup_timer_.Start(
      FROM_HERE, interval, this,
      &SafeBrowsingNavigationObserverManager::CleanUpStaleNavigationFootprints);
}

void SafeBrowsingNavigationObserverManager::AddToReferrerChain(
    ReferrerChain* referrer_chain,
    NavigationEvent* nav_event,
    const GURL& destination_main_frame_url,
    ReferrerChainEntry::URLType type) {
  std::unique_ptr<ReferrerChainEntry> referrer_chain_entry =
      base::MakeUnique<ReferrerChainEntry>();
  const GURL destination_url = nav_event->GetDestinationUrl();
  referrer_chain_entry->set_url(ShortURLForReporting(destination_url));
  if (destination_main_frame_url.is_valid() &&
      destination_url != destination_main_frame_url)
    referrer_chain_entry->set_main_frame_url(
        ShortURLForReporting(destination_main_frame_url));
  referrer_chain_entry->set_type(type);
  auto ip_it = host_to_ip_map_.find(destination_url.host());
  if (ip_it != host_to_ip_map_.end()) {
    for (ResolvedIPAddress entry : ip_it->second) {
      referrer_chain_entry->add_ip_addresses(entry.ip);
    }
  }
  // Since we only track navigation to landing referrer, we will not log the
  // referrer of the landing referrer page.
  if (type != ReferrerChainEntry::LANDING_REFERRER) {
    referrer_chain_entry->set_referrer_url(
        ShortURLForReporting(nav_event->source_url));
    // Only set |referrer_main_frame_url| if it is diff from |referrer_url|.
    if (nav_event->source_main_frame_url.is_valid() &&
        nav_event->source_url != nav_event->source_main_frame_url) {
      referrer_chain_entry->set_referrer_main_frame_url(
          ShortURLForReporting(nav_event->source_main_frame_url));
    }
  }
  referrer_chain_entry->set_is_retargeting(nav_event->source_tab_id !=
                                           nav_event->target_tab_id);
  referrer_chain_entry->set_navigation_time_msec(
      nav_event->last_updated.ToJavaTime());
  if (!nav_event->server_redirect_urls.empty()) {
    // The first entry in |server_redirect_chain| should be the original request
    // url.
    ReferrerChainEntry::ServerRedirect* server_redirect =
        referrer_chain_entry->add_server_redirect_chain();
    server_redirect->set_url(
        ShortURLForReporting(nav_event->original_request_url));
    for (const GURL& redirect : nav_event->server_redirect_urls) {
      server_redirect = referrer_chain_entry->add_server_redirect_chain();
      server_redirect->set_url(ShortURLForReporting(redirect));
    }
  }
  referrer_chain->Add()->Swap(referrer_chain_entry.get());
}

void SafeBrowsingNavigationObserverManager::GetRemainingReferrerChain(
    NavigationEvent* last_nav_event_traced,
    int current_user_gesture_count,
    int user_gesture_count_limit,
    ReferrerChain* out_referrer_chain,
    SafeBrowsingNavigationObserverManager::AttributionResult* out_result) {
  GURL last_main_frame_url_traced(last_nav_event_traced->source_main_frame_url);
  while (current_user_gesture_count < user_gesture_count_limit) {
    // Back trace to the next nav_event that was initiated by the user.
    while (!last_nav_event_traced->is_user_initiated) {
      last_nav_event_traced = navigation_event_list_.FindNavigationEvent(
          last_nav_event_traced->source_url,
          last_nav_event_traced->source_main_frame_url,
          last_nav_event_traced->source_tab_id);
      if (!last_nav_event_traced)
        return;
      AddToReferrerChain(out_referrer_chain, last_nav_event_traced,
                         last_main_frame_url_traced,
                         ReferrerChainEntry::CLIENT_REDIRECT);
      // Stop searching if the size of out_referrer_chain already reached its
      // limit.
      if (out_referrer_chain->size() == kReferrerChainMaxLength)
        return;
      last_main_frame_url_traced = last_nav_event_traced->source_main_frame_url;
    }

    current_user_gesture_count++;


    // If the source_url and source_main_frame_url of current navigation event
    // are empty, and is_user_initiated is true, this is a browser initiated
    // navigation (e.g. trigged by typing in address bar, clicking on bookmark,
    // etc). We reached the end of the referrer chain.
    if (last_nav_event_traced->source_url.is_empty() &&
        last_nav_event_traced->source_main_frame_url.is_empty()) {
      DCHECK(last_nav_event_traced->is_user_initiated);
      return;
    }

    last_nav_event_traced = navigation_event_list_.FindNavigationEvent(
        last_nav_event_traced->source_url,
        last_nav_event_traced->source_main_frame_url,
        last_nav_event_traced->source_tab_id);
    if (!last_nav_event_traced)
      return;

    AddToReferrerChain(
        out_referrer_chain, last_nav_event_traced, last_main_frame_url_traced,
        GetURLTypeAndAdjustAttributionResult(
            current_user_gesture_count == user_gesture_count_limit,
            out_result));
    // Stop searching if the size of out_referrer_chain already reached its
    // limit.
    if (out_referrer_chain->size() == kReferrerChainMaxLength)
      return;
    last_main_frame_url_traced = last_nav_event_traced->source_main_frame_url;
  }
}

}  // namespace safe_browsing
