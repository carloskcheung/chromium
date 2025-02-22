// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.browserservices.trustedwebactivityui.view;

import static org.chromium.chrome.browser.browserservices.trustedwebactivityui.TrustedWebActivityModel.DISCLOSURE_EVENTS_CALLBACK;
import static org.chromium.chrome.browser.browserservices.trustedwebactivityui.TrustedWebActivityModel.DISCLOSURE_STATE;
import static org.chromium.chrome.browser.browserservices.trustedwebactivityui.TrustedWebActivityModel.DISCLOSURE_STATE_NOT_SHOWN;
import static org.chromium.chrome.browser.browserservices.trustedwebactivityui.TrustedWebActivityModel.DISCLOSURE_STATE_SHOWN;

import android.content.res.Resources;
import android.support.annotation.Nullable;

import org.chromium.chrome.R;
import org.chromium.chrome.browser.browserservices.trustedwebactivityui.TrustedWebActivityModel;
import org.chromium.chrome.browser.dependency_injection.ActivityScope;
import org.chromium.chrome.browser.modelutil.PropertyKey;
import org.chromium.chrome.browser.modelutil.PropertyObservable;
import org.chromium.chrome.browser.snackbar.Snackbar;
import org.chromium.chrome.browser.snackbar.SnackbarManager;

import javax.inject.Inject;

import dagger.Lazy;

/**
 * Shows the Trusted Web Activity disclosure when appropriate and notifies of its acceptance.
 *
 * Lifecycle: There should be a 1-1 relationship between this class and instances of Trusted
 * Web Activities.
 * Thread safety: All methods on this class should be called on the UI thread.
 */
@ActivityScope
public class TrustedWebActivityDisclosureView implements
        PropertyObservable.PropertyObserver<PropertyKey> {
    private final Resources mResources;
    private final Lazy<SnackbarManager> mSnackbarManager;
    private final TrustedWebActivityModel mModel;

    /**
     * A {@link SnackbarManager.SnackbarController} that records the users acceptance of the
     * "Running in Chrome" disclosure.
     *
     * It is also used as a key to for our snackbar so we can dismiss it when the user navigates
     * to a page where they don't need to show the disclosure.
     */
    private final SnackbarManager.SnackbarController mSnackbarController =
            new SnackbarManager.SnackbarController() {
                /**
                 * To be called when the user accepts the Running in Chrome disclosure.
                 */
                @Override
                public void onAction(Object actionData) {
                    mModel.get(DISCLOSURE_EVENTS_CALLBACK).onDisclosureAccepted();
                }
            };

    @Inject
    TrustedWebActivityDisclosureView(Resources resources,
            Lazy<SnackbarManager> snackbarManager, TrustedWebActivityModel model) {
        mResources = resources;
        mSnackbarManager = snackbarManager;
        mModel = model;
        mModel.addObserver(this);
    }

    @Override
    public void onPropertyChanged(PropertyObservable<PropertyKey> source,
            @Nullable PropertyKey propertyKey) {
        if (propertyKey != DISCLOSURE_STATE) return;

        switch (mModel.get(DISCLOSURE_STATE)) {
            case DISCLOSURE_STATE_SHOWN:
                mSnackbarManager.get().showSnackbar(makeRunningInChromeInfobar());
                break;
            case DISCLOSURE_STATE_NOT_SHOWN:
                mSnackbarManager.get().dismissSnackbars(mSnackbarController);
                break;
        }
    }

    private Snackbar makeRunningInChromeInfobar() {
        String title = mResources.getString(R.string.twa_running_in_chrome);
        int type = Snackbar.TYPE_PERSISTENT;
        int code = Snackbar.UMA_TWA_PRIVACY_DISCLOSURE;

        String action = mResources.getString(R.string.ok_got_it);
        return Snackbar.make(title, mSnackbarController, type, code)
                .setAction(action, null)
                .setSingleLine(false);
    }
}
