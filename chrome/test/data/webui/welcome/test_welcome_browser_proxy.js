// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/** @implements {welcome.WelcomeBrowserProxy} */
class TestWelcomeBrowserProxy extends TestBrowserProxy {
  constructor() {
    super([
      'handleActivateSignIn',
      'goToNewTabPage',
      'goToURL',
    ]);
  }

  /** @override */
  handleActivateSignIn() {
    this.methodCalled('handleActivateSignIn');
  }

  /** @override */
  goToNewTabPage() {
    this.methodCalled('goToNewTabPage');
  }

  /** @override */
  goToURL() {
    this.methodCalled('goToURL');
  }
}
