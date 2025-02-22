// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/display/mirror_window_controller.h"

#include "ash/display/mirror_window_test_api.h"
#include "ash/display/window_tree_host_manager.h"
#include "ash/shell.h"
#include "ash/test/ash_test_base.h"
#include "ash/wm/cursor_manager_test_api.h"
#include "base/command_line.h"
#include "base/strings/stringprintf.h"
#include "base/test/scoped_feature_list.h"
#include "components/viz/common/features.h"
#include "ui/aura/env.h"
#include "ui/aura/test/test_window_delegate.h"
#include "ui/aura/test/test_windows.h"
#include "ui/aura/window.h"
#include "ui/aura/window_event_dispatcher.h"
#include "ui/base/hit_test.h"
#include "ui/display/display_switches.h"
#include "ui/display/manager/display_manager.h"
#include "ui/display/test/display_manager_test_api.h"
#include "ui/events/test/event_generator.h"

namespace ash {

namespace {
display::ManagedDisplayInfo CreateDisplayInfo(int64_t id,
                                              const gfx::Rect& bounds,
                                              float scale = 1.f) {
  display::ManagedDisplayInfo info(
      id, base::StringPrintf("x-%d", static_cast<int>(id)), false);
  info.SetBounds(bounds);
  info.set_device_scale_factor(scale);
  return info;
}

class MirrorOnBootTest : public AshTestBase {
 public:
  MirrorOnBootTest() = default;
  ~MirrorOnBootTest() override = default;

  void SetUp() override {
    base::CommandLine::ForCurrentProcess()->AppendSwitchASCII(
        ::switches::kHostWindowBounds, "1+1-300x300,1+301-300x300");
    base::CommandLine::ForCurrentProcess()->AppendSwitch(
        ::switches::kEnableSoftwareMirroring);
    AshTestBase::SetUp();
  }
  void TearDown() override { AshTestBase::TearDown(); }

 private:
  DISALLOW_COPY_AND_ASSIGN(MirrorOnBootTest);
};

class MirrorUsingSurfaceLayersTest : public AshTestBase {
 public:
  MirrorUsingSurfaceLayersTest() = default;
  ~MirrorUsingSurfaceLayersTest() override = default;

  void SetUp() override {
    scoped_feature_list_.InitWithFeatures({features::kVizDisplayCompositor},
                                          {});
    AshTestBase::SetUp();
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;

  DISALLOW_COPY_AND_ASSIGN(MirrorUsingSurfaceLayersTest);
};
}  // namespace

using MirrorWindowControllerTest = AshTestBase;

// Make sure that the compositor based mirroring can switch
// from/to dock mode.
TEST_F(MirrorWindowControllerTest, DockMode) {
  const int64_t internal_id = 1;
  const int64_t external_id = 2;

  const display::ManagedDisplayInfo internal_display_info =
      CreateDisplayInfo(internal_id, gfx::Rect(0, 0, 500, 500));
  const display::ManagedDisplayInfo external_display_info =
      CreateDisplayInfo(external_id, gfx::Rect(1, 1, 100, 100));
  std::vector<display::ManagedDisplayInfo> display_info_list;

  // software mirroring.
  display_info_list.push_back(internal_display_info);
  display_info_list.push_back(external_display_info);
  display_manager()->OnNativeDisplaysChanged(display_info_list);
  const int64_t internal_display_id =
      display::test::DisplayManagerTestApi(display_manager())
          .SetFirstDisplayAsInternalDisplay();
  EXPECT_EQ(internal_id, internal_display_id);

  display_manager()->SetMirrorMode(display::MirrorMode::kNormal, base::nullopt);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(1U, display_manager()->GetNumDisplays());
  EXPECT_TRUE(display_manager()->IsInSoftwareMirrorMode());
  EXPECT_EQ(external_id,
            display_manager()->GetMirroringDestinationDisplayIdList()[0]);

  // dock mode.
  display_info_list.clear();
  display_info_list.push_back(external_display_info);
  display_manager()->OnNativeDisplaysChanged(display_info_list);
  EXPECT_EQ(1U, display_manager()->GetNumDisplays());
  EXPECT_FALSE(display_manager()->IsInMirrorMode());

  // back to software mirroring.
  display_info_list.clear();
  display_info_list.push_back(internal_display_info);
  display_info_list.push_back(external_display_info);
  display_manager()->OnNativeDisplaysChanged(display_info_list);
  EXPECT_EQ(1U, display_manager()->GetNumDisplays());
  EXPECT_TRUE(display_manager()->IsInMirrorMode());
  EXPECT_EQ(external_id,
            display_manager()->GetMirroringDestinationDisplayIdList()[0]);
}

TEST_F(MirrorOnBootTest, MirrorOnBoot) {
  EXPECT_TRUE(display_manager()->IsInMirrorMode());

  RunAllPendingInMessageLoop();
  MirrorWindowTestApi test_api;
  EXPECT_EQ(1U, test_api.GetHosts().size());
}

// Test that the mirror window matches the size of the host display.
TEST_F(MirrorUsingSurfaceLayersTest, MirrorSize) {
  const int64_t primary_id = 1;
  const int64_t mirror_id = 2;

  // Run the test with and without display scaling.
  int scale_factors[] = {1, 2};
  for (int scale : scale_factors) {
    const display::ManagedDisplayInfo primary_display_info =
        CreateDisplayInfo(primary_id, gfx::Rect(0, 0, 400, 400), scale);
    const display::ManagedDisplayInfo mirror_display_info =
        CreateDisplayInfo(mirror_id, gfx::Rect(400, 0, 600, 600), scale);
    std::vector<display::ManagedDisplayInfo> display_info_list = {
        primary_display_info, mirror_display_info};

    // Start software mirroring.
    display_manager()->OnNativeDisplaysChanged(display_info_list);
    display_manager()->SetMirrorMode(display::MirrorMode::kNormal,
                                     base::nullopt);
    base::RunLoop().RunUntilIdle();
    EXPECT_EQ(1U, display_manager()->GetNumDisplays());
    EXPECT_TRUE(display_manager()->IsInSoftwareMirrorMode());

    // Check the size of the mirror window.
    const display::Display& primary_display =
        display_manager()->GetDisplayForId(primary_id);
    aura::Window* root_window = Shell::GetRootWindowForDisplayId(mirror_id);
    aura::Window* mirror_window = root_window->children()[0];
    EXPECT_EQ(primary_display.GetSizeInPixel(), root_window->bounds().size());
    EXPECT_EQ(primary_display.GetSizeInPixel(), mirror_window->bounds().size());
  }
}

}  // namespace ash
