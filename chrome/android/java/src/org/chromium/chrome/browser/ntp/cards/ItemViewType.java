// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.ntp.cards;

import android.support.annotation.IntDef;
import android.support.v7.widget.RecyclerView.Adapter;

import org.chromium.chrome.browser.suggestions.SuggestionsCarousel;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/**
 * View type values for the items that will be held by the NTP's RecyclerView.
 *
 * @see Adapter#getItemViewType(int)
 */
@IntDef({ItemViewType.ABOVE_THE_FOLD, ItemViewType.LOGO, ItemViewType.SITE_SECTION,
        ItemViewType.HEADER, ItemViewType.SNIPPET, ItemViewType.SPACING, ItemViewType.STATUS,
        ItemViewType.PROGRESS, ItemViewType.ACTION, ItemViewType.FOOTER, ItemViewType.PROMO,
        ItemViewType.ALL_DISMISSED, ItemViewType.CAROUSEL})
@Retention(RetentionPolicy.SOURCE)
public @interface ItemViewType {
    /**
     * View type for the above the fold item
     *
     * @see Adapter#getItemViewType(int)
     */
    int ABOVE_THE_FOLD = 1;
    /**
     * View type for the logo
     *
     * @see Adapter#getItemViewType(int)
     */
    int LOGO = 2;
    /**
     * View type for a {@code SiteSection}.
     *
     * @see Adapter#getItemViewType(int)
     */
    int SITE_SECTION = 3;
    /**
     * View type for card group headers
     *
     * @see Adapter#getItemViewType(int)
     */
    int HEADER = 4;
    /**
     * View type for snippet cards
     *
     * @see Adapter#getItemViewType(int)
     */
    int SNIPPET = 5;
    /**
     * View type for a {@link SpacingItem} used to provide spacing at the end of the list.
     *
     * @see Adapter#getItemViewType(int)
     */
    int SPACING = 6;
    /**
     * View type for a {@link StatusItem}, the card displaying status information
     *
     * @see Adapter#getItemViewType(int)
     */
    int STATUS = 7;
    /**
     * View type for a {@link ProgressItem}, the progress indicator.
     *
     * @see Adapter#getItemViewType(int)
     */
    int PROGRESS = 8;
    /**
     * View type for a {@link ActionItem}, an action button.
     *
     * @see Adapter#getItemViewType(int)
     */
    int ACTION = 9;
    /**
     * View type for a {@link Footer}.
     *
     * @see Adapter#getItemViewType(int)
     */
    int FOOTER = 10;
    /**
     * View type for a {@link SignInPromo}.
     *
     * @see Adapter#getItemViewType(int)
     */
    int PROMO = 11;
    /**
     * View type for a {@link AllDismissedItem}.
     *
     * @see Adapter#getItemViewType(int)
     */
    int ALL_DISMISSED = 12;

    /**
     * View type for a {@link SuggestionsCarousel}.
     *
     * @see Adapter#getItemViewType(int)
     */
    int CAROUSEL = 13;

    int COMMENT_FORM  = 100;
}
