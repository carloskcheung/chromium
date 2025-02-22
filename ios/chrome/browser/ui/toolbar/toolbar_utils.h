// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_CHROME_BROWSER_UI_TOOLBAR_TOOLBAR_UTILS_H_
#define IOS_CHROME_BROWSER_UI_TOOLBAR_TOOLBAR_UTILS_H_

#import <UIKit/UIKit.h>

// Returns the multiplier for the font size associated with the current content
// size |category|, clamped to have it not too big or not too small.
CGFloat ToolbarClampedFontSizeMultiplier(UIContentSizeCategory category);

// Returns the height of the toolbar when it is collapsed, based on the current
// |category|.
CGFloat ToolbarCollapsedHeight(UIContentSizeCategory category);

// Returns the height of the toolbar when it is expanded, based on the current
// |category|.
CGFloat ToolbarExpandedHeight(UIContentSizeCategory category);

// Returns the height of the location bar, based on the |category|.
CGFloat LocationBarHeight(UIContentSizeCategory category);

#endif  // IOS_CHROME_BROWSER_UI_TOOLBAR_TOOLBAR_UTILS_H_
