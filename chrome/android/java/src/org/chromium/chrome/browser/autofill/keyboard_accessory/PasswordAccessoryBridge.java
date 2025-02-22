// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.autofill.keyboard_accessory;

import android.graphics.Bitmap;
import android.support.annotation.Px;

import org.chromium.base.Callback;
import org.chromium.base.annotations.CalledByNative;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.ChromeActivity;
import org.chromium.chrome.browser.autofill.keyboard_accessory.KeyboardAccessoryData.AccessorySheetData;
import org.chromium.chrome.browser.autofill.keyboard_accessory.KeyboardAccessoryData.Action;
import org.chromium.chrome.browser.autofill.keyboard_accessory.KeyboardAccessoryData.FooterCommand;
import org.chromium.chrome.browser.autofill.keyboard_accessory.KeyboardAccessoryData.Item;
import org.chromium.chrome.browser.autofill.keyboard_accessory.KeyboardAccessoryData.UserInfo;
import org.chromium.ui.base.WindowAndroid;

import java.util.ArrayList;
import java.util.List;

class PasswordAccessoryBridge {
    private final KeyboardAccessoryData.PropertyProvider<Item[]> mItemProvider =
            new KeyboardAccessoryData.PropertyProvider<>();
    private final KeyboardAccessoryData.PropertyProvider<Action[]> mActionProvider =
            new KeyboardAccessoryData.PropertyProvider<>(
                    AccessoryAction.GENERATE_PASSWORD_AUTOMATIC);
    private final ManualFillingCoordinator mManualFillingCoordinator;
    private final ChromeActivity mActivity;
    private long mNativeView;

    private PasswordAccessoryBridge(long nativeView, WindowAndroid windowAndroid) {
        mNativeView = nativeView;
        mActivity = (ChromeActivity) windowAndroid.getActivity().get();
        mManualFillingCoordinator = mActivity.getManualFillingController();
        mManualFillingCoordinator.registerPasswordProvider(mItemProvider);
        mManualFillingCoordinator.registerActionProvider(mActionProvider);
    }

    @CalledByNative
    private static PasswordAccessoryBridge create(long nativeView, WindowAndroid windowAndroid) {
        return new PasswordAccessoryBridge(nativeView, windowAndroid);
    }

    @CalledByNative
    private void onItemsAvailable(Object objAccessorySheetData) {
        mItemProvider.notifyObservers(convertToItems((AccessorySheetData) objAccessorySheetData));
    }

    @CalledByNative
    private void onAutomaticGenerationStatusChanged(boolean available) {
        final Action[] generationAction;
        if (available) {
            // This is meant to suppress the warning that the short string is not used.
            // TODO(crbug.com/855581): Switch between strings based on whether they fit on the
            // screen or not.
            boolean useLongString = true;
            String caption = useLongString
                    ? mActivity.getString(R.string.password_generation_accessory_button)
                    : mActivity.getString(R.string.password_generation_accessory_button_short);
            generationAction = new Action[] {
                    new Action(caption, AccessoryAction.GENERATE_PASSWORD_AUTOMATIC, (action) -> {
                        assert mNativeView
                                != 0
                            : "Controller has been destroyed but the bridge wasn't cleaned up!";
                        KeyboardAccessoryMetricsRecorder.recordActionSelected(
                                AccessoryAction.GENERATE_PASSWORD_AUTOMATIC);
                        nativeOnGenerationRequested(mNativeView);
                    })};
        } else {
            generationAction = new Action[0];
        }
        mActionProvider.notifyObservers(generationAction);
    }

    @CalledByNative
    void showWhenKeyboardIsVisible() {
        mManualFillingCoordinator.showWhenKeyboardIsVisible();
    }

    @CalledByNative
    void hide() {
        mManualFillingCoordinator.hide();
    }

    @CalledByNative
    private void closeAccessorySheet() {
        mManualFillingCoordinator.closeAccessorySheet();
    }

    @CalledByNative
    private void swapSheetWithKeyboard() {
        mManualFillingCoordinator.swapSheetWithKeyboard();
    }

    @CalledByNative
    private void destroy() {
        mItemProvider.notifyObservers(new Item[] {}); // There are no more items available!
        mNativeView = 0;
    }

    @CalledByNative
    private static Object createAccessorySheetData(String title) {
        return new AccessorySheetData(title);
    }

    @CalledByNative
    private static Object addUserInfoToAccessorySheetData(Object objAccessorySheetData) {
        UserInfo userInfo = new UserInfo();
        ((AccessorySheetData) objAccessorySheetData).getUserInfoList().add(userInfo);
        return userInfo;
    }

    @CalledByNative
    private static void addFieldToUserInfo(Object objUserInfo, String displayText,
            String a11yDescription, boolean isObfuscated, boolean selectable) {
        ((UserInfo) objUserInfo)
                .getFields()
                .add(new UserInfo.Field(displayText, a11yDescription, isObfuscated, selectable));
    }

    @CalledByNative
    private static void addFooterCommandToAccessorySheetData(
            Object objAccessorySheetData, String displayText) {
        ((AccessorySheetData) objAccessorySheetData)
                .getFooterCommands()
                .add(new FooterCommand(displayText));
    }

    private Item[] convertToItems(AccessorySheetData accessorySheetData) {
        List<Item> items = new ArrayList<>();

        items.add(Item.createTopDivider());

        items.add(Item.createLabel(accessorySheetData.getTitle(), accessorySheetData.getTitle()));

        for (UserInfo userInfo : accessorySheetData.getUserInfoList()) {
            for (UserInfo.Field field : userInfo.getFields()) {
                Callback<Item> itemSelectedCallback = null;
                if (field.isSelectable()) {
                    // TODO(crbug.com/902425): Create the callback in addFieldToUserInfo once the
                    //                         Item type is replaced with AccessorySheetData.
                    itemSelectedCallback = (item) -> {
                        assert mNativeView != 0 : "Controller was destroyed but the bridge wasn't!";
                        KeyboardAccessoryMetricsRecorder.recordSuggestionSelected(
                                AccessoryTabType.PASSWORDS,
                                item.isObfuscated() ? AccessorySuggestionType.PASSWORD
                                                    : AccessorySuggestionType.USERNAME);
                        nativeOnFillingTriggered(
                                mNativeView, item.isObfuscated(), item.getCaption());
                    };
                }
                items.add(Item.createSuggestion(field.getDisplayText(), field.getA11yDescription(),
                        field.isObfuscated(), itemSelectedCallback, this ::fetchFavicon));
            }
        }

        if (!accessorySheetData.getFooterCommands().isEmpty()) {
            items.add(Item.createDivider());
            for (FooterCommand footerCommand : accessorySheetData.getFooterCommands()) {
                items.add(Item.createOption(
                        footerCommand.getDisplayText(), footerCommand.getDisplayText(), (item) -> {
                            assert mNativeView
                                    != 0 : "Controller was destroyed but the bridge wasn't!";
                            nativeOnOptionSelected(mNativeView, item.getCaption());
                        }));
            }
        }

        return items.toArray(new Item[items.size()]);
    }

    public void fetchFavicon(@Px int desiredSize, Callback<Bitmap> faviconCallback) {
        assert mNativeView != 0 : "Favicon was requested after the bridge was destroyed!";
        nativeOnFaviconRequested(mNativeView, desiredSize, faviconCallback);
    }

    private native void nativeOnFaviconRequested(long nativePasswordAccessoryViewAndroid,
            int desiredSizeInPx, Callback<Bitmap> faviconCallback);
    private native void nativeOnFillingTriggered(
            long nativePasswordAccessoryViewAndroid, boolean isObfuscated, String textToFill);
    private native void nativeOnOptionSelected(
            long nativePasswordAccessoryViewAndroid, String selectedOption);
    private native void nativeOnGenerationRequested(long nativePasswordAccessoryViewAndroid);
}