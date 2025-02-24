// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.components.safe_browsing;

import android.content.Context;
import android.support.annotation.IntDef;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/**
 * Java interface that a SafeBrowsingApiHandler must implement when used with
 * {@code SafeBrowsingApiBridge}
 */
public interface SafeBrowsingApiHandler {
    // Implementors must provide a no-arg constructor to be instantiated via reflection.

    /**
     * Observer to be notified when the SafeBrowsingApiHandler determines the verdict for a url.
     */
    public interface Observer {
        // Note: |checkDelta| is the time the remote call took in microseconds.
        void onUrlCheckDone(long callbackId, @SafeBrowsingResult int resultStatus, String metadata,
                long checkDelta);
        // TODO(csharrison): Temporary to avoid breaking downstream.
        void onUrlCheckDone(long callbackId, @SafeBrowsingResult int resultStatus, String metadata);
    }

    // Possible values for resultStatus. Native side has the same definitions.
    @Retention(RetentionPolicy.SOURCE)
    @IntDef({STATUS_INTERNAL_ERROR, STATUS_SUCCESS, STATUS_TIMEOUT})
    @interface SafeBrowsingResult {}
    static final int STATUS_INTERNAL_ERROR = -1;
    static final int STATUS_SUCCESS = 0;
    static final int STATUS_TIMEOUT = 1;

    /**
     * Verifies that SafeBrowsingApiHandler can operate and initializes if feasible.
     * Should be called on the same sequence as |startUriLookup|.
     *
     * @return the handler if it's usable, or null if the API is not supported.
     */
    public boolean init(Context context, Observer result);

    /**
     * Start a URI-lookup to determine if it matches one of the specified threats.
     * This is called on every URL resource Chrome loads, on the same sequence as |init|.
     */
    public void startUriLookup(long callbackId, String uri, int[] threatsOfInterest);
}
