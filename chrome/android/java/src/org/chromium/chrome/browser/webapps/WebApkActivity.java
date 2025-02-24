// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.webapps;

import static org.chromium.webapk.lib.common.WebApkConstants.WEBAPK_PACKAGE_PREFIX;

import android.content.Intent;
import android.os.Bundle;
import android.os.SystemClock;

import org.chromium.base.ApiCompatibilityUtils;
import org.chromium.base.library_loader.LibraryProcessType;
import org.chromium.base.metrics.RecordHistogram;
import org.chromium.chrome.browser.metrics.WebApkUma;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.util.IntentUtils;
import org.chromium.content.browser.ChildProcessCreationParams;
import org.chromium.net.NetError;
import org.chromium.net.NetworkChangeNotifier;
import org.chromium.webapk.lib.common.WebApkConstants;

import java.util.concurrent.TimeUnit;

/**
 * An Activity is designed for WebAPKs (native Android apps) and displays a webapp in a nearly
 * UI-less Chrome.
 */
public class WebApkActivity extends WebappActivity {
    /** Manages whether to check update for the WebAPK, and starts update check if needed. */
    private WebApkUpdateManager mUpdateManager;

    /** Indicates whether launching renderer in WebAPK process is enabled. */
    private boolean mCanLaunchRendererInWebApkProcess;

    private final ChildProcessCreationParams mDefaultParams =
            ChildProcessCreationParams.getDefault();

    /** The start time that the activity becomes focused. */
    private long mStartTime;

    private static final String TAG = "cr_WebApkActivity";

    /** A {@link WebappSplashScreenController} that also handles WebAPK logic. */
    private class WebApkSplashScreenController extends WebappSplashScreenController {
        // TODO(yusufo): Move this to be shared with Trusted Web Activity.
        /** The error code of the navigation. */
        private int mErrorCode;

        private WebApkOfflineDialog mOfflineDialog;

        /** Indicates whether reloading is allowed. */
        private boolean mAllowReloads;

        @Override
        public void onDidFinishNavigation(final Tab tab, final String url, boolean isInMainFrame,
                boolean isErrorPage, boolean hasCommitted, boolean isSameDocument,
                boolean isFragmentNavigation, Integer pageTransition, int errorCode,
                int httpStatusCode) {
            super.onDidFinishNavigation(tab, url, isInMainFrame, isErrorPage, hasCommitted,
                    isSameDocument, isFragmentNavigation, pageTransition, errorCode,
                    httpStatusCode);
            mErrorCode = errorCode;

            switch (mErrorCode) {
                case NetError.ERR_NETWORK_CHANGED:
                    onNetworkChanged(tab);
                    break;
                case NetError.ERR_INTERNET_DISCONNECTED:
                    onNetworkDisconnected(tab);
                    break;
                default:
                    if (mOfflineDialog != null) {
                        mOfflineDialog.cancel();
                        mOfflineDialog = null;
                    }
                    break;
            }
        }

        @Override
        protected boolean canHideSplashScreen() {
            return mErrorCode != NetError.ERR_INTERNET_DISCONNECTED
                    && mErrorCode != NetError.ERR_NETWORK_CHANGED;
        }

        private void onNetworkChanged(Tab tab) {
            if (!mAllowReloads) return;

            // It is possible that we get {@link NetError.ERR_NETWORK_CHANGED} during the first
            // reload after the device is online. The navigation will fail until the next auto
            // reload fired by {@link NetErrorHelperCore}. We call reload explicitly to reduce the
            // waiting time.
            tab.reloadIgnoringCache();
            mAllowReloads = false;
        }

        private void onNetworkDisconnected(final Tab tab) {
            if (mOfflineDialog != null) return;

            final NetworkChangeNotifier.ConnectionTypeObserver observer =
                    new NetworkChangeNotifier.ConnectionTypeObserver() {
                        @Override
                        public void onConnectionTypeChanged(int connectionType) {
                            if (!NetworkChangeNotifier.isOnline()) return;

                            NetworkChangeNotifier.removeConnectionTypeObserver(this);
                            tab.reloadIgnoringCache();
                            // One more reload is allowed after the network connection is back.
                            mAllowReloads = true;
                        }
                    };

            NetworkChangeNotifier.addConnectionTypeObserver(observer);
            mOfflineDialog = new WebApkOfflineDialog();
            mOfflineDialog.show(WebApkActivity.this, new WebApkOfflineDialog.DialogListener() {
                @Override
                public void onQuit() {
                    ApiCompatibilityUtils.finishAndRemoveTask(WebApkActivity.this);
                }
            }, mWebappInfo.name());
        }
    }

    @Override
    protected boolean isVerified() {
        return true;
    }

    @Override
    protected WebappSplashScreenController createWebappSplashScreenController() {
        return new WebApkSplashScreenController();
    }

    @Override
    protected WebappInfo createWebappInfo(Intent intent) {
        return (intent == null) ? WebApkInfo.createEmpty() : WebApkInfo.create(intent);
    }

    @Override
    protected void initializeUI(Bundle savedInstance) {
        super.initializeUI(savedInstance);
        getActivityTab().setWebappManifestScope(mWebappInfo.scopeUri().toString());
    }

    @Override
    public boolean shouldPreferLightweightFre(Intent intent) {
        // We cannot use getWebApkPackageName() because {@link WebappActivity#preInflationStartup()}
        // may not have been called yet.
        String webApkPackageName =
                IntentUtils.safeGetStringExtra(intent, WebApkConstants.EXTRA_WEBAPK_PACKAGE_NAME);

        // Use the lightweight FRE for unbound WebAPKs.
        return webApkPackageName != null && !webApkPackageName.startsWith(WEBAPK_PACKAGE_PREFIX);
    }

    @Override
    public void finishNativeInitialization() {
        super.finishNativeInitialization();
        if (!isInitialized()) return;
        mCanLaunchRendererInWebApkProcess = ChromeWebApkHost.canLaunchRendererInWebApkProcess();
    }

    @Override
    public String getNativeClientPackageName() {
        return getWebappInfo().apkPackageName();
    }

    @Override
    public void onResumeWithNative() {
        super.onResumeWithNative();

        // When launching Chrome renderer in WebAPK process is enabled, WebAPK hosts Chrome's
        // renderer processes by declaring the Chrome's renderer service in its AndroidManifest.xml
        // and sets {@link ChildProcessCreationParams} for WebAPK's renderer process so the
        // {@link ChildProcessLauncher} knows which application's renderer service to connect to.
        initializeChildProcessCreationParams(mCanLaunchRendererInWebApkProcess);
    }

    @Override
    public void onResume() {
        super.onResume();
        mStartTime = SystemClock.elapsedRealtime();
    }

    @Override
    protected void recordIntentToCreationTime(long timeMs) {
        super.recordIntentToCreationTime(timeMs);

        RecordHistogram.recordTimesHistogram(
                "MobileStartup.IntentToCreationTime.WebApk", timeMs, TimeUnit.MILLISECONDS);
    }

    @Override
    protected void onDeferredStartupWithStorage(WebappDataStorage storage) {
        super.onDeferredStartupWithStorage(storage);

        WebApkInfo info = (WebApkInfo) mWebappInfo;
        WebApkUma.recordShellApkVersion(info.shellApkVersion(), info.apkPackageName());

        mUpdateManager = new WebApkUpdateManager(storage);
        mUpdateManager.updateIfNeeded(getActivityTab(), info);
    }

    @Override
    protected void onUpdatedLastUsedTime(
            WebappDataStorage storage, boolean previouslyLaunched, long previousUsageTimestamp) {
        if (previouslyLaunched) {
            WebApkUma.recordLaunchInterval(storage.getLastUsedTime() - previousUsageTimestamp);
        }
    }

    @Override
    public void onPause() {
        super.onPause();
        initializeChildProcessCreationParams(false);
    }

    @Override
    public void onPauseWithNative() {
        WebApkUma.recordWebApkSessionDuration(SystemClock.elapsedRealtime() - mStartTime);
        super.onPauseWithNative();
    }

    /**
     * Initializes {@link ChildProcessCreationParams} as a WebAPK's renderer process if
     * {@link isForWebApk}} is true; as Chrome's child process otherwise.
     * @param isForWebApk: Whether the {@link ChildProcessCreationParams} is initialized as a
     *                     WebAPK renderer process.
     */
    private void initializeChildProcessCreationParams(boolean isForWebApk) {
        // TODO(hanxi): crbug.com/664530. WebAPKs shouldn't use a global ChildProcessCreationParams.
        ChildProcessCreationParams params = mDefaultParams;
        if (isForWebApk) {
            boolean isExternalService = false;
            boolean bindToCaller = false;
            boolean ignoreVisibilityForImportance = false;
            params = new ChildProcessCreationParams(getWebappInfo().apkPackageName(),
                    isExternalService, LibraryProcessType.PROCESS_CHILD, bindToCaller,
                    ignoreVisibilityForImportance);
        }
        ChildProcessCreationParams.registerDefault(params);
    }

    @Override
    protected void onDestroyInternal() {
        if (mUpdateManager != null) {
            mUpdateManager.destroy();
        }
        super.onDestroyInternal();
    }
}
