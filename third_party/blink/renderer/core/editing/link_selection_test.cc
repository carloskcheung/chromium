// Copyright (c) 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/platform/web_coalesced_input_event.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/renderer/core/dom/range.h"
#include "third_party/blink/renderer/core/frame/frame_test_helpers.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/input/event_handler.h"
#include "third_party/blink/renderer/core/page/chrome_client.h"
#include "third_party/blink/renderer/core/page/context_menu_controller.h"
#include "third_party/blink/renderer/core/page/focus_controller.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/platform/cursor.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"
#include "third_party/blink/renderer/platform/testing/url_test_helpers.h"

using testing::_;

namespace blink {

IntSize Scaled(IntSize p, float scale) {
  p.Scale(scale, scale);
  return p;
}

class LinkSelectionTestBase : public testing::Test {
 protected:
  enum DragFlag { kSendDownEvent = 1, kSendUpEvent = 1 << 1 };
  using DragFlags = unsigned;

  void EmulateMouseDrag(const IntPoint& down_point,
                        const IntPoint& up_point,
                        int modifiers,
                        DragFlags = kSendDownEvent | kSendUpEvent);

  void EmulateMouseClick(const IntPoint& click_point,
                         WebMouseEvent::Button,
                         int modifiers,
                         int count = 1);
  void EmulateMouseDown(const IntPoint& click_point,
                        WebMouseEvent::Button,
                        int modifiers,
                        int count = 1);

  String GetSelectionText();

  frame_test_helpers::WebViewHelper helper_;
  WebViewImpl* web_view_ = nullptr;
  Persistent<WebLocalFrameImpl> main_frame_ = nullptr;
};

void LinkSelectionTestBase::EmulateMouseDrag(const IntPoint& down_point,
                                             const IntPoint& up_point,
                                             int modifiers,
                                             DragFlags drag_flags) {
  if (drag_flags & kSendDownEvent) {
    const auto& down_event = frame_test_helpers::CreateMouseEvent(
        WebMouseEvent::kMouseDown, WebMouseEvent::Button::kLeft, down_point,
        modifiers);
    web_view_->HandleInputEvent(WebCoalescedInputEvent(down_event));
  }

  const int kMoveEventsNumber = 10;
  const float kMoveIncrementFraction = 1. / kMoveEventsNumber;
  const auto& up_down_vector = up_point - down_point;
  for (int i = 0; i < kMoveEventsNumber; ++i) {
    const auto& move_point =
        down_point + Scaled(up_down_vector, i * kMoveIncrementFraction);
    const auto& move_event = frame_test_helpers::CreateMouseEvent(
        WebMouseEvent::kMouseMove, WebMouseEvent::Button::kLeft, move_point,
        modifiers);
    web_view_->HandleInputEvent(WebCoalescedInputEvent(move_event));
  }

  if (drag_flags & kSendUpEvent) {
    const auto& up_event = frame_test_helpers::CreateMouseEvent(
        WebMouseEvent::kMouseUp, WebMouseEvent::Button::kLeft, up_point,
        modifiers);
    web_view_->HandleInputEvent(WebCoalescedInputEvent(up_event));
  }
}

void LinkSelectionTestBase::EmulateMouseClick(const IntPoint& click_point,
                                              WebMouseEvent::Button button,
                                              int modifiers,
                                              int count) {
  auto event = frame_test_helpers::CreateMouseEvent(
      WebMouseEvent::kMouseDown, button, click_point, modifiers);
  event.click_count = count;
  web_view_->HandleInputEvent(WebCoalescedInputEvent(event));
  event.SetType(WebMouseEvent::kMouseUp);
  web_view_->HandleInputEvent(WebCoalescedInputEvent(event));
}

void LinkSelectionTestBase::EmulateMouseDown(const IntPoint& click_point,
                                             WebMouseEvent::Button button,
                                             int modifiers,
                                             int count) {
  auto event = frame_test_helpers::CreateMouseEvent(
      WebMouseEvent::kMouseDown, button, click_point, modifiers);
  event.click_count = count;
  web_view_->HandleInputEvent(WebCoalescedInputEvent(event));
}

String LinkSelectionTestBase::GetSelectionText() {
  return main_frame_->SelectionAsText();
}

class TestFrameClient : public frame_test_helpers::TestWebFrameClient {
 public:
  void BeginNavigation(
      std::unique_ptr<blink::WebNavigationInfo> info) override {
    last_policy_ = info->navigation_policy;
  }

  WebNavigationPolicy GetLastNavigationPolicy() const { return last_policy_; }

 private:
  WebNavigationPolicy last_policy_ = kWebNavigationPolicyCurrentTab;
};

class LinkSelectionTest : public LinkSelectionTestBase {
 protected:
  void SetUp() override {
    constexpr char kHTMLString[] =
        "<a id='link' href='foo.com' style='font-size:20pt'>Text to select "
        "foobar</a>"
        "<div id='page_text'>Lorem ipsum dolor sit amet</div>";

    web_view_ = helper_.Initialize(&test_frame_client_);
    main_frame_ = web_view_->MainFrameImpl();
    frame_test_helpers::LoadHTMLString(
        main_frame_, kHTMLString,
        url_test_helpers::ToKURL("http://foobar.com"));
    web_view_->Resize(WebSize(800, 600));
    web_view_->GetPage()->GetFocusController().SetActive(true);

    auto* document = main_frame_->GetFrame()->GetDocument();
    ASSERT_NE(nullptr, document);
    auto* link_to_select = document->getElementById("link")->firstChild();
    ASSERT_NE(nullptr, link_to_select);
    // We get larger range that we actually want to select, because we need a
    // slightly larger rect to include the last character to the selection.
    auto* const range_to_select =
        Range::Create(*document, link_to_select, 5, link_to_select, 16);

    const auto& selection_rect = range_to_select->BoundingBox();
    const auto& selection_rect_center_y = selection_rect.Center().Y();
    left_point_in_link_ = selection_rect.MinXMinYCorner();
    left_point_in_link_.SetY(selection_rect_center_y);

    right_point_in_link_ = selection_rect.MaxXMinYCorner();
    right_point_in_link_.SetY(selection_rect_center_y);
    right_point_in_link_.Move(-2, 0);
  }

  void TearDown() override {
    // Manually reset since |test_frame_client_| won't outlive |helper_|.
    helper_.Reset();
  }

  TestFrameClient test_frame_client_;
  IntPoint left_point_in_link_;
  IntPoint right_point_in_link_;
};

TEST_F(LinkSelectionTest, MouseDragWithoutAltAllowNoLinkSelection) {
  EmulateMouseDrag(left_point_in_link_, right_point_in_link_, 0);
  EXPECT_TRUE(GetSelectionText().IsEmpty());
}

TEST_F(LinkSelectionTest, MouseDragWithAltAllowSelection) {
  EmulateMouseDrag(left_point_in_link_, right_point_in_link_,
                   WebInputEvent::kAltKey);
  EXPECT_EQ("to select", GetSelectionText());
}

TEST_F(LinkSelectionTest, HandCursorDuringLinkDrag) {
  EmulateMouseDrag(right_point_in_link_, left_point_in_link_, 0,
                   kSendDownEvent);
  main_frame_->GetFrame()
      ->LocalFrameRoot()
      .GetEventHandler()
      .ScheduleCursorUpdate();
  test::RunDelayedTasks(TimeDelta::FromMilliseconds(50));
  const auto& cursor =
      main_frame_->GetFrame()->GetChromeClient().LastSetCursorForTesting();
  EXPECT_EQ(Cursor::kHand, cursor.GetType());
}

TEST_F(LinkSelectionTest, DragOnNothingShowsPointer) {
  EmulateMouseDrag(IntPoint(100, 500), IntPoint(300, 500), 0, kSendDownEvent);
  main_frame_->GetFrame()
      ->LocalFrameRoot()
      .GetEventHandler()
      .ScheduleCursorUpdate();
  test::RunDelayedTasks(TimeDelta::FromMilliseconds(50));
  const auto& cursor =
      main_frame_->GetFrame()->GetChromeClient().LastSetCursorForTesting();
  EXPECT_EQ(Cursor::kPointer, cursor.GetType());
}

TEST_F(LinkSelectionTest, CaretCursorOverLinkDuringSelection) {
  EmulateMouseDrag(right_point_in_link_, left_point_in_link_,
                   WebInputEvent::kAltKey, kSendDownEvent);
  main_frame_->GetFrame()
      ->LocalFrameRoot()
      .GetEventHandler()
      .ScheduleCursorUpdate();
  test::RunDelayedTasks(TimeDelta::FromMilliseconds(50));
  const auto& cursor =
      main_frame_->GetFrame()->GetChromeClient().LastSetCursorForTesting();
  EXPECT_EQ(Cursor::kIBeam, cursor.GetType());
}

TEST_F(LinkSelectionTest, HandCursorOverLinkAfterContextMenu) {
  // Move mouse.
  EmulateMouseDrag(right_point_in_link_, left_point_in_link_, 0, 0);

  // Show context menu. We don't send mouseup event here since in browser it
  // doesn't reach blink because of shown context menu.
  EmulateMouseDown(left_point_in_link_, WebMouseEvent::Button::kRight, 0, 1);

  LocalFrame* frame = main_frame_->GetFrame();
  // Hide context menu.
  frame->GetPage()->GetContextMenuController().ClearContextMenu();

  frame->LocalFrameRoot().GetEventHandler().ScheduleCursorUpdate();
  test::RunDelayedTasks(TimeDelta::FromMilliseconds(50));
  const auto& cursor =
      main_frame_->GetFrame()->GetChromeClient().LastSetCursorForTesting();
  EXPECT_EQ(Cursor::kHand, cursor.GetType());
}

TEST_F(LinkSelectionTest, SingleClickWithAltStartsDownload) {
  EmulateMouseClick(left_point_in_link_, WebMouseEvent::Button::kLeft,
                    WebInputEvent::kAltKey);
  EXPECT_EQ(kWebNavigationPolicyDownload,
            test_frame_client_.GetLastNavigationPolicy());
}

TEST_F(LinkSelectionTest, SingleClickWithAltStartsDownloadWhenTextSelected) {
  auto* document = main_frame_->GetFrame()->GetDocument();
  auto* text_to_select = document->getElementById("page_text")->firstChild();
  ASSERT_NE(nullptr, text_to_select);

  // Select some page text outside the link element.
  const Range* range_to_select =
      Range::Create(*document, text_to_select, 1, text_to_select, 20);
  const auto& selection_rect = range_to_select->BoundingBox();
  main_frame_->MoveRangeSelection(selection_rect.MinXMinYCorner(),
                                  selection_rect.MaxXMaxYCorner());
  EXPECT_FALSE(GetSelectionText().IsEmpty());

  EmulateMouseClick(left_point_in_link_, WebMouseEvent::Button::kLeft,
                    WebInputEvent::kAltKey);
  EXPECT_EQ(kWebNavigationPolicyDownload,
            test_frame_client_.GetLastNavigationPolicy());
}

class LinkSelectionClickEventsTest : public LinkSelectionTestBase {
 protected:
  class MockEventListener final : public EventListener {
   public:
    static MockEventListener* Create() { return new MockEventListener(); }

    bool operator==(const EventListener& other) const final {
      return this == &other;
    }

    MOCK_METHOD2(Invoke, void(ExecutionContext* executionContext, Event*));

   private:
    MockEventListener() : EventListener(kCPPEventListenerType) {}
  };

  void SetUp() override {
    const char* const kHTMLString =
        "<div id='empty_div' style='width: 100px; height: 100px;'></div>"
        "<span id='text_div'>Sometexttoshow</span>";

    web_view_ = helper_.Initialize();
    main_frame_ = web_view_->MainFrameImpl();
    frame_test_helpers::LoadHTMLString(
        main_frame_, kHTMLString,
        url_test_helpers::ToKURL("http://foobar.com"));
    web_view_->Resize(WebSize(800, 600));
    web_view_->GetPage()->GetFocusController().SetActive(true);

    auto* document = main_frame_->GetFrame()->GetDocument();
    ASSERT_NE(nullptr, document);

    auto* empty_div = document->getElementById("empty_div");
    auto* text_div = document->getElementById("text_div");
    ASSERT_NE(nullptr, empty_div);
    ASSERT_NE(nullptr, text_div);
  }

  void CheckMouseClicks(Element& element, bool double_click_event) {
    struct ScopedListenersCleaner {
      ScopedListenersCleaner(Element* element) : element_(element) {}

      ~ScopedListenersCleaner() { element_->RemoveAllEventListeners(); }

      Persistent<Element> element_;
    } const listeners_cleaner(&element);

    MockEventListener* event_handler = MockEventListener::Create();
    element.addEventListener(double_click_event ? event_type_names::kDblclick
                                                : event_type_names::kClick,
                             event_handler);

    testing::InSequence s;
    EXPECT_CALL(*event_handler, Invoke(_, _)).Times(1);

    const auto& elem_bounds = element.BoundsInViewport();
    const int click_count = double_click_event ? 2 : 1;
    EmulateMouseClick(elem_bounds.Center(), WebMouseEvent::Button::kLeft, 0,
                      click_count);

    if (double_click_event) {
      EXPECT_EQ(element.innerText().IsEmpty(), GetSelectionText().IsEmpty());
    }
  }
};

TEST_F(LinkSelectionClickEventsTest, SingleAndDoubleClickWillBeHandled) {
  auto* document = main_frame_->GetFrame()->GetDocument();
  auto* element = document->getElementById("empty_div");

  {
    SCOPED_TRACE("Empty div, single click");
    CheckMouseClicks(*element, false);
  }

  {
    SCOPED_TRACE("Empty div, double click");
    CheckMouseClicks(*element, true);
  }

  element = document->getElementById("text_div");

  {
    SCOPED_TRACE("Text div, single click");
    CheckMouseClicks(*element, false);
  }

  {
    SCOPED_TRACE("Text div, double click");
    CheckMouseClicks(*element, true);
  }
}

}  // namespace blink
