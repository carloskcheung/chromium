// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.download;

import android.content.Context;
import android.content.res.Resources;
import android.text.TextUtils;
import android.view.LayoutInflater;

import org.chromium.base.annotations.CalledByNative;
import org.chromium.base.metrics.RecordHistogram;
import org.chromium.chrome.browser.ChromeActivity;
import org.chromium.chrome.browser.modaldialog.DialogDismissalCause;
import org.chromium.chrome.browser.modaldialog.ModalDialogManager;
import org.chromium.chrome.browser.modaldialog.ModalDialogProperties;
import org.chromium.chrome.browser.modaldialog.ModalDialogView;
import org.chromium.chrome.browser.modaldialog.ModalDialogViewBinder;
import org.chromium.chrome.browser.modelutil.PropertyModel;
import org.chromium.chrome.browser.modelutil.PropertyModelChangeProcessor;
import org.chromium.chrome.browser.preferences.PrefServiceBridge;
import org.chromium.chrome.download.R;
import org.chromium.ui.base.WindowAndroid;

import java.io.File;
import java.util.ArrayList;

/**
 * Helper class to handle communication between download location dialog and native.
 */
public class DownloadLocationDialogBridge implements ModalDialogView.Controller {
    private long mNativeDownloadLocationDialogBridge;
    private ModalDialogView mLocationDialog;
    private DownloadLocationCustomView mCustomView;
    private ModalDialogManager mModalDialogManager;
    private long mTotalBytes;
    private @DownloadLocationDialogType int mDialogType;
    private String mSuggestedPath;
    private Context mContext;

    private DownloadLocationDialogBridge(long nativeDownloadLocationDialogBridge) {
        mNativeDownloadLocationDialogBridge = nativeDownloadLocationDialogBridge;
    }

    @CalledByNative
    public static DownloadLocationDialogBridge create(long nativeDownloadLocationDialogBridge) {
        return new DownloadLocationDialogBridge(nativeDownloadLocationDialogBridge);
    }

    @CalledByNative
    private void destroy() {
        mNativeDownloadLocationDialogBridge = 0;
        if (mModalDialogManager != null) {
            mModalDialogManager.dismissDialog(
                    mLocationDialog, DialogDismissalCause.DISMISSED_BY_NATIVE);
        }
    }

    @CalledByNative
    public void showDialog(WindowAndroid windowAndroid, long totalBytes,
            @DownloadLocationDialogType int dialogType, String suggestedPath) {
        ChromeActivity activity = (ChromeActivity) windowAndroid.getActivity().get();
        // If the activity has gone away, just clean up the native pointer.
        if (activity == null) {
            onDismiss(DialogDismissalCause.ACTIVITY_DESTROYED);
            return;
        }

        mModalDialogManager = activity.getModalDialogManager();
        mContext = activity;
        mTotalBytes = totalBytes;
        mDialogType = dialogType;
        mSuggestedPath = suggestedPath;

        DownloadDirectoryProvider.getInstance().getAllDirectoriesOptions(
                (ArrayList<DirectoryOption> dirs) -> { onDirectoryOptionsRetrieved(dirs); });
    }

    @Override
    public void onClick(@ModalDialogView.ButtonType int buttonType) {
        switch (buttonType) {
            case ModalDialogView.ButtonType.POSITIVE:
                mModalDialogManager.dismissDialog(
                        mLocationDialog, DialogDismissalCause.POSITIVE_BUTTON_CLICKED);
                break;
            case ModalDialogView.ButtonType.NEGATIVE:
                mModalDialogManager.dismissDialog(
                        mLocationDialog, DialogDismissalCause.NEGATIVE_BUTTON_CLICKED);
                break;
            default:
        }
    }

    @Override
    public void onDismiss(@DialogDismissalCause int dismissalCause) {
        switch (dismissalCause) {
            case DialogDismissalCause.POSITIVE_BUTTON_CLICKED:
                handleResponses(mCustomView.getFileName(), mCustomView.getDirectoryOption(),
                        mCustomView.getDontShowAgain());
                break;
            default:
                cancel();
                break;
        }
        mLocationDialog = null;
        mCustomView = null;
    }

    /**
     * Called after retrieved the download directory options.
     * @param dirs An list of available download directories.
     */
    private void onDirectoryOptionsRetrieved(ArrayList<DirectoryOption> dirs) {
        // If there is only one directory available, don't show the default dialog, and set the
        // download directory to default. Dialog will still show for other types of dialogs, like
        // name conflict or disk error.
        if (dirs.size() == 1 && mDialogType == DownloadLocationDialogType.DEFAULT) {
            final DirectoryOption dir = dirs.get(0);
            if (dir.type == DirectoryOption.DownloadLocationDirectoryType.DEFAULT) {
                assert(!TextUtils.isEmpty(dir.location));
                PrefServiceBridge.getInstance().setDownloadAndSaveFileDefaultDirectory(
                        dir.location);
                nativeOnComplete(mNativeDownloadLocationDialogBridge, mSuggestedPath);
            }
            return;
        }

        // Already showing the dialog.
        if (mLocationDialog != null) return;

        // Actually show the dialog.
        mCustomView = (DownloadLocationCustomView) LayoutInflater.from(mContext).inflate(
                R.layout.download_location_dialog, null);
        mCustomView.initialize(mDialogType, new File(mSuggestedPath));

        Resources resources = mContext.getResources();
        PropertyModel model =
                new PropertyModel.Builder(ModalDialogProperties.ALL_KEYS)
                        .with(ModalDialogProperties.CONTROLLER, this)
                        .with(ModalDialogProperties.TITLE, getTitle(mTotalBytes, mDialogType))
                        .with(ModalDialogProperties.CUSTOM_VIEW, mCustomView)
                        .with(ModalDialogProperties.POSITIVE_BUTTON_TEXT, resources,
                                R.string.duplicate_download_infobar_download_button)
                        .with(ModalDialogProperties.NEGATIVE_BUTTON_TEXT, resources,
                                R.string.cancel)
                        .build();

        mLocationDialog = new ModalDialogView(mContext);
        PropertyModelChangeProcessor.create(model, mLocationDialog, new ModalDialogViewBinder());

        mModalDialogManager.showDialog(mLocationDialog, ModalDialogManager.ModalDialogType.APP);
    }

    private String getTitle(long totalBytes, @DownloadLocationDialogType int dialogType) {
        switch (dialogType) {
            case DownloadLocationDialogType.LOCATION_FULL:
                return mContext.getString(R.string.download_location_not_enough_space);

            case DownloadLocationDialogType.LOCATION_NOT_FOUND:
                return mContext.getString(R.string.download_location_no_sd_card);

            case DownloadLocationDialogType.NAME_CONFLICT:
                return mContext.getString(R.string.download_location_download_again);

            case DownloadLocationDialogType.NAME_TOO_LONG:
                return mContext.getString(R.string.download_location_rename_file);

            case DownloadLocationDialogType.DEFAULT:
                String title = mContext.getString(R.string.download_location_dialog_title);
                if (totalBytes > 0) {
                    StringBuilder stringBuilder = new StringBuilder(title);
                    stringBuilder.append(" ");
                    stringBuilder.append(DownloadUtils.getStringForBytes(mContext, totalBytes));
                    title = stringBuilder.toString();
                }
                return title;
        }
        assert false;
        return null;
    }

    /**
     * Pass along information from location dialog to native.
     *
     * @param fileName      Name the user gave the file.
     * @param directoryOption  Location the user wants the file saved to.
     * @param dontShowAgain Whether the user wants the "Save download to..." dialog shown again.
     */
    private void handleResponses(
            String fileName, DirectoryOption directoryOption, boolean dontShowAgain) {
        // If there's no file location, treat as a cancellation.
        if (directoryOption == null || directoryOption.location == null || fileName == null) {
            cancel();
            return;
        }

        // Update native with new path.
        if (mNativeDownloadLocationDialogBridge != 0) {
            PrefServiceBridge.getInstance().setDownloadAndSaveFileDefaultDirectory(
                    directoryOption.location);

            RecordHistogram.recordEnumeratedHistogram(
                    "MobileDownload.Location.Dialog.DirectoryType", directoryOption.type,
                    DirectoryOption.DownloadLocationDirectoryType.NUM_ENTRIES);

            File file = new File(directoryOption.location, fileName);
            nativeOnComplete(mNativeDownloadLocationDialogBridge, file.getAbsolutePath());
        }

        // Update preference to show prompt based on whether checkbox is checked only when the user
        // click the positive button.
        if (dontShowAgain) {
            PrefServiceBridge.getInstance().setPromptForDownloadAndroid(
                    DownloadPromptStatus.DONT_SHOW);
        } else {
            PrefServiceBridge.getInstance().setPromptForDownloadAndroid(
                    DownloadPromptStatus.SHOW_PREFERENCE);
        }
    }

    private void cancel() {
        if (mNativeDownloadLocationDialogBridge != 0) {
            nativeOnCanceled(mNativeDownloadLocationDialogBridge);
        }
    }

    public native void nativeOnComplete(
            long nativeDownloadLocationDialogBridge, String returnedPath);
    public native void nativeOnCanceled(long nativeDownloadLocationDialogBridge);
}
