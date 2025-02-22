// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

cr.define('onboarding_welcome_email_chooser', function() {
  suite('EmailChooserTest', function() {
    let emails = [
      {
        id: 0,
        name: 'Gmail',
        icon: 'gmail',
        url: 'http://www.gmail.com',
      },
      {
        id: 1,
        name: 'Yahoo',
        icon: 'yahoo',
        url: 'http://mail.yahoo.com',
      }
    ];

    /** @type {nux.NuxEmailProxy} */
    let testEmailBrowserProxy;

    /** @type {nux.BookmarkProxy} */
    let testBookmarkBrowserProxy;

    /** @type {EmailChooserElement} */
    let testElement;

    setup(function() {
      testEmailBrowserProxy = new TestNuxEmailProxy();
      nux.NuxEmailProxyImpl.instance_ = testEmailBrowserProxy;
      testBookmarkBrowserProxy = new TestBookmarkProxy();
      nux.BookmarkProxyImpl.instance_ = testBookmarkBrowserProxy;

      testEmailBrowserProxy.setEmailList(emails);

      PolymerTest.clearBody();
      testElement = document.createElement('email-chooser');
      document.body.appendChild(testElement);
      return Promise.all([
        testEmailBrowserProxy.whenCalled('recordPageInitialized'),
        testEmailBrowserProxy.whenCalled('getEmailList'),
      ]);
    });

    teardown(function() {
      testElement.remove();
    });

    test('test email chooser options', function() {
      let options = testElement.shadowRoot.querySelectorAll('.option');
      assertEquals(2, options.length);

      // First option is default selected and action button should be enabled.
      assertEquals(testElement.$$('.option[active]'), options[0]);
      assertFalse(testElement.$$('.action-button').disabled);

      options[0].click();
      return Promise
          .all([
            testBookmarkBrowserProxy.whenCalled('removeBookmark'),
            testEmailBrowserProxy.whenCalled('recordClickedOption'),
          ])
          .then(responses => {
            let removedId = responses[0];
            assertEquals(removedId, 1);
            assertFalse(!!testElement.$$('.option[active]'));
            assertTrue(testElement.$$('.action-button').disabled);

            // Click second option, it should be selected.
            testBookmarkBrowserProxy.reset();
            options[1].click();
            return Promise.all([
              testBookmarkBrowserProxy.whenCalled('addBookmark'),
            ]);
          })
          .then(responses => {
            let addResponse = responses[0];

            assertEquals(addResponse.title, emails[1].name);
            assertEquals(addResponse.url, emails[1].url);
            assertEquals(addResponse.parentId, '1');
            assertEquals(testElement.$$('.option[active]'), options[1]);
            assertFalse(testElement.$$('.action-button').disabled);

            // Click second option again, it should be deselected and action
            // button should be disabled.
            testBookmarkBrowserProxy.reset();
            options[1].click();
            return testBookmarkBrowserProxy.whenCalled('removeBookmark');
          })
          .then(removedId => {
            assertEquals(removedId, 2);

            assertFalse(!!testElement.$$('.option[active]'));
            assertTrue(testElement.$$('.action-button').disabled);
          });
    });

    test('test email chooser skip button', function() {
      let options = testElement.shadowRoot.querySelectorAll('.option');
      testElement.bookmarkBarWasShown_ = true;

      // First option should be selected and action button should be enabled.
      testElement.$.noThanksButton.click();
      return Promise
          .all([
            testBookmarkBrowserProxy.whenCalled('removeBookmark'),
            testBookmarkBrowserProxy.whenCalled('toggleBookmarkBar'),
            testEmailBrowserProxy.whenCalled('recordNoThanks'),
          ])
          .then(responses => {
            let removeBookmarkResponse = responses[0];
            let toggleBookmarkBarResponse = responses[1];

            assertEquals(1, removeBookmarkResponse);
            assertEquals(true, toggleBookmarkBarResponse);
          });
    });

    test('test email chooser next button', function() {
      let options = testElement.shadowRoot.querySelectorAll('.option');
      testElement.bookmarkBarWasShown_ = true;

      // First option should be selected and action button should be enabled.
      testElement.$$('.action-button').click();
      return Promise
          .all([
            testEmailBrowserProxy.whenCalled('recordProviderSelected'),
            testEmailBrowserProxy.whenCalled('recordGetStarted'),
          ])
          .then(responses => {
            let recordProviderSelectedResponse = responses[0];

            // Id for the provider that was selected.
            assertEquals(0, recordProviderSelectedResponse[0]);
            // Length of the email list array.
            assertEquals(2, recordProviderSelectedResponse[1]);
          });
    });
  });
});
