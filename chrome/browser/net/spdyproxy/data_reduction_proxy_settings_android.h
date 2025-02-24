// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_NET_SPDYPROXY_DATA_REDUCTION_PROXY_SETTINGS_ANDROID_H_
#define CHROME_BROWSER_NET_SPDYPROXY_DATA_REDUCTION_PROXY_SETTINGS_ANDROID_H_

#include <memory>
#include <vector>

#include "base/android/jni_string.h"
#include "base/android/jni_weak_ref.h"
#include "base/android/scoped_java_ref.h"
#include "base/compiler_specific.h"
#include "base/gtest_prod_util.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "components/data_reduction_proxy/proto/data_store.pb.h"
#include "components/prefs/pref_member.h"

using base::android::ScopedJavaLocalRef;

class Profile;

namespace data_reduction_proxy {
class DataReductionProxySettings;
}

// Central point for configuring the data reduction proxy on Android.
// This object lives on the UI thread and all of its methods are expected to
// be called from there.
class DataReductionProxySettingsAndroid {
 public:
  DataReductionProxySettingsAndroid(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);

  virtual ~DataReductionProxySettingsAndroid();

  void InitDataReductionProxySettings(Profile* profile);

  // JNI wrapper interfaces to the indentically-named superclass methods.
  jboolean IsDataReductionProxyPromoAllowed(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);
  jboolean IsDataReductionProxyFREPromoAllowed(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);
  jboolean IsDataReductionProxyEnabled(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);
  jboolean IsDataReductionProxyManaged(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);
  void SetDataReductionProxyEnabled(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj,
      jboolean enabled);
  jlong GetDataReductionLastUpdateTime(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);
  void ClearDataSavingStatistics(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);
  jlong GetTotalHttpContentLengthSaved(
        JNIEnv* env,
        const base::android::JavaParamRef<jobject>& obj);
  ScopedJavaLocalRef<jlongArray> GetDailyOriginalContentLengths(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);
  ScopedJavaLocalRef<jlongArray> GetDailyReceivedContentLengths(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);
  ScopedJavaLocalRef<jstring> GetDataReductionProxyPassThroughHeader(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);

  // Return a Java |ContentLengths| object wrapping the results of a call to
  // DataReductionProxySettings::GetContentLengths.
  base::android::ScopedJavaLocalRef<jobject> GetContentLengths(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);

  // Determines whether the data reduction proxy is unreachable. This is
  // done by keeping a count of requests which go through proxy vs those
  // which should have gone through the proxy based on the config.
  jboolean IsDataReductionProxyUnreachable(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);

  // Return if Lo-Fi previews are enabled via a field trial or the command line.
  jboolean AreLoFiPreviewsEnabled(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);

  ScopedJavaLocalRef<jstring> GetTokenForAuthChallenge(JNIEnv* env,
                                                       jobject obj,
                                                       jstring host,
                                                       jstring realm);

  // Returns a Java string of the Data Reduction Proxy proxy list for HTTP
  // origins as a semi-colon delimited list.
  ScopedJavaLocalRef<jstring> GetHttpProxyList(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);

  // Returns a Java string of the last Data Reduction Proxy bypass event as
  // a JSON object.
  ScopedJavaLocalRef<jstring> GetLastBypassEvent(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);

  // Gets the historical data usage for |numDays| and adds them to a list that
  // groups data use by hostname.
  void QueryDataUsage(JNIEnv* env,
                      const base::android::JavaParamRef<jobject>& obj,
                      const base::android::JavaParamRef<jobject>& j_result_obj,
                      jint num_days);
  void OnQueryDataUsageComplete(
      std::unique_ptr<std::vector<data_reduction_proxy::DataUsageBucket>>
          data_usage);

  JavaObjectWeakGlobalRef j_settings_obj_;
  base::android::ScopedJavaGlobalRef<jobject> j_query_result_obj_;
  int num_day_for_query_;

 private:
  friend class DataReductionProxySettingsAndroidTest;
  friend class TestDataReductionProxySettingsAndroid;
  FRIEND_TEST_ALL_PREFIXES(DataReductionProxySettingsAndroidTest,
                           TestGetDailyContentLengths);

  // For testing purposes.
  DataReductionProxySettingsAndroid();

  ScopedJavaLocalRef<jlongArray> GetDailyContentLengths(JNIEnv* env,
                                                        const char* pref_name);

  virtual data_reduction_proxy::DataReductionProxySettings* Settings();

  base::WeakPtrFactory<DataReductionProxySettingsAndroid> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(DataReductionProxySettingsAndroid);
};

#endif  // CHROME_BROWSER_NET_SPDYPROXY_DATA_REDUCTION_PROXY_SETTINGS_ANDROID_H_
