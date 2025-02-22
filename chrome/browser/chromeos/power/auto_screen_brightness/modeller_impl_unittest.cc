// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/power/auto_screen_brightness/modeller_impl.h"

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/scoped_task_environment.h"
#include "base/test/test_mock_time_task_runner.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "chrome/browser/chromeos/power/auto_screen_brightness/fake_als_reader.h"
#include "chrome/browser/chromeos/power/auto_screen_brightness/fake_brightness_monitor.h"
#include "chrome/browser/chromeos/power/auto_screen_brightness/monotone_cubic_spline.h"
#include "chrome/browser/chromeos/power/auto_screen_brightness/trainer.h"
#include "chrome/browser/chromeos/power/auto_screen_brightness/utils.h"
#include "chrome/test/base/testing_profile.h"
#include "chromeos/chromeos_features.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/user_activity/user_activity_detector.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"

namespace chromeos {
namespace power {
namespace auto_screen_brightness {

namespace {

MonotoneCubicSpline CreateTestCurveFromTrainingData(
    const std::vector<TrainingDataPoint>& data) {
  CHECK_GT(data.size(), 1u);

  std::vector<double> xs;
  std::vector<double> ys;

  const auto& data_point = data[0];
  xs.push_back(data_point.ambient_log_lux);
  ys.push_back((data_point.brightness_old + data_point.brightness_new) / 2);

  for (size_t i = 1; i < data.size(); ++i) {
    xs.push_back(xs[i - 1] + 1);
    ys.push_back(ys[i - 1] + 1);
  }

  return MonotoneCubicSpline(xs, ys);
}

void CheckOptionalCurves(
    const base::Optional<MonotoneCubicSpline>& result_curve,
    const base::Optional<MonotoneCubicSpline>& expected_curve) {
  EXPECT_EQ(result_curve.has_value(), expected_curve.has_value());
  if (result_curve) {
    EXPECT_EQ(*result_curve, *expected_curve);
  }
}

// Fake testing trainer that has configuration status and validity of personal
// curve specified in the constructor.
class FakeTrainer : public Trainer {
 public:
  FakeTrainer(bool is_configured, bool is_personal_curve_valid)
      : is_configured_(is_configured),
        is_personal_curve_valid_(is_personal_curve_valid) {
    // If personal curve is valid, then the trainer must be configured.
    DCHECK(!is_personal_curve_valid_ || is_configured_);
  }
  ~FakeTrainer() override = default;

  // Trainer overrides:
  bool HasValidConfiguration() const override { return is_configured_; }

  bool SetInitialCurves(const MonotoneCubicSpline& global_curve,
                        const MonotoneCubicSpline& current_curve) override {
    DCHECK(is_configured_);
    global_curve_.emplace(global_curve);
    current_curve_.emplace(is_personal_curve_valid_ ? current_curve
                                                    : global_curve);
    return is_personal_curve_valid_;
  }

  MonotoneCubicSpline GetGlobalCurve() const override {
    DCHECK(is_configured_);
    DCHECK(global_curve_);
    return *global_curve_;
  }

  MonotoneCubicSpline GetCurrentCurve() const override {
    DCHECK(is_configured_);
    DCHECK(current_curve_);
    return *current_curve_;
  }

  MonotoneCubicSpline Train(
      const std::vector<TrainingDataPoint>& data) override {
    DCHECK(is_configured_);
    DCHECK(current_curve_);
    current_curve_.emplace(CreateTestCurveFromTrainingData(data));
    return *current_curve_;
  }

 private:
  bool is_configured_;
  bool is_personal_curve_valid_;
  base::Optional<MonotoneCubicSpline> global_curve_;
  base::Optional<MonotoneCubicSpline> current_curve_;

  DISALLOW_COPY_AND_ASSIGN(FakeTrainer);
};

class TestObserver : public Modeller::Observer {
 public:
  TestObserver() {}
  ~TestObserver() override = default;

  // Modeller::Observer overrides:
  void OnModelTrained(const MonotoneCubicSpline& brightness_curve) override {
    personal_curve_.emplace(brightness_curve);
    trained_curve_received_ = true;
  }

  void OnModelInitialized(
      const base::Optional<MonotoneCubicSpline>& global_curve,
      const base::Optional<MonotoneCubicSpline>& personal_curve) override {
    model_initialized_ = true;
    if (global_curve)
      global_curve_.emplace(*global_curve);

    if (personal_curve)
      personal_curve_.emplace(*personal_curve);
  }

  base::Optional<MonotoneCubicSpline> global_curve() const {
    return global_curve_;
  }

  base::Optional<MonotoneCubicSpline> personal_curve() const {
    return personal_curve_;
  }

  void CheckStatus(bool is_model_initialized,
                   const base::Optional<MonotoneCubicSpline>& global_curve,
                   const base::Optional<MonotoneCubicSpline>& personal_curve) {
    EXPECT_EQ(is_model_initialized, model_initialized_);
    CheckOptionalCurves(global_curve, global_curve_);
    CheckOptionalCurves(personal_curve, personal_curve_);
  }

  bool trained_curve_received() { return trained_curve_received_; }

 private:
  bool model_initialized_ = false;
  base::Optional<MonotoneCubicSpline> global_curve_;
  base::Optional<MonotoneCubicSpline> personal_curve_;
  bool trained_curve_received_ = false;

  DISALLOW_COPY_AND_ASSIGN(TestObserver);
};

}  // namespace

class ModellerImplTest : public testing::Test {
 public:
  ModellerImplTest()
      : scoped_task_environment_(
            base::test::ScopedTaskEnvironment::MainThreadType::MOCK_TIME),
        thread_bundle_(content::TestBrowserThreadBundle::PLAIN_MAINLOOP) {
    CHECK(temp_dir_.CreateUniqueTempDir());
    TestingProfile::Builder profile_builder;
    profile_builder.SetProfileName("testuser@gmail.com");
    profile_builder.SetPath(temp_dir_.GetPath().AppendASCII("TestProfile"));
    profile_ = profile_builder.Build();
  }

  ~ModellerImplTest() override {
    base::TaskScheduler::GetInstance()->FlushForTesting();
  }

  // Sets up |modeller_| with a FakeTrainer.
  void SetUpModeller(bool is_trainer_configured, bool is_personal_curve_valid) {
    modeller_ = ModellerImpl::CreateForTesting(
        profile_.get(), &fake_als_reader_, &fake_brightness_monitor_,
        &user_activity_detector_,
        std::make_unique<FakeTrainer>(is_trainer_configured,
                                      is_personal_curve_valid),
        base::SequencedTaskRunnerHandle::Get(),
        scoped_task_environment_.GetMockTickClock());

    test_observer_ = std::make_unique<TestObserver>();
    modeller_->AddObserver(test_observer_.get());
  }

  void Init(AlsReader::AlsInitStatus als_reader_status,
            BrightnessMonitor::Status brightness_monitor_status,
            bool is_trainer_configured = true,
            bool is_personal_curve_valid = true) {
    fake_als_reader_.set_als_init_status(als_reader_status);
    fake_brightness_monitor_.set_status(brightness_monitor_status);
    SetUpModeller(is_trainer_configured, is_personal_curve_valid);
    scoped_task_environment_.RunUntilIdle();
  }

 protected:
  void WriteCurveToFile(const MonotoneCubicSpline& curve) {
    const base::FilePath curve_path =
        ModellerImpl::ModellerImpl::GetCurvePathFromProfile(profile_.get());
    CHECK(!curve_path.empty());

    const std::string data = curve.ToString();
    const int bytes_written =
        base::WriteFile(curve_path, data.data(), data.size());
    ASSERT_EQ(bytes_written, static_cast<int>(data.size()))
        << "Wrote " << bytes_written << " byte(s) instead of " << data.size()
        << " to " << curve_path;
  }

  base::test::ScopedTaskEnvironment scoped_task_environment_;
  content::TestBrowserThreadBundle thread_bundle_;

  ui::UserActivityDetector user_activity_detector_;

  base::ScopedTempDir temp_dir_;
  std::unique_ptr<TestingProfile> profile_;

  FakeAlsReader fake_als_reader_;
  FakeBrightnessMonitor fake_brightness_monitor_;

  std::unique_ptr<ModellerImpl> modeller_;
  std::unique_ptr<TestObserver> test_observer_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ModellerImplTest);
};

// AlsReader is |kDisabled| when Modeller is created.
TEST_F(ModellerImplTest, AlsReaderDisabledOnInit) {
  Init(AlsReader::AlsInitStatus::kDisabled,
       BrightnessMonitor::Status::kSuccess);

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              base::nullopt /* global_curve */,
                              base::nullopt /* personal_curve */);
}

// BrightnessMonitor is |kDisabled| when Modeller is created.
TEST_F(ModellerImplTest, BrightnessMonitorDisabledOnInit) {
  Init(AlsReader::AlsInitStatus::kSuccess,
       BrightnessMonitor::Status::kDisabled);

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              base::nullopt /* global_curve */,
                              base::nullopt /* personal_curve */);
}

// AlsReader is |kDisabled| on later notification.
TEST_F(ModellerImplTest, AlsReaderDisabledOnNotification) {
  Init(AlsReader::AlsInitStatus::kInProgress,
       BrightnessMonitor::Status::kSuccess);

  test_observer_->CheckStatus(false /* is_model_initialized */,
                              base::nullopt /* global_curve */,
                              base::nullopt /* personal_curve */);

  fake_als_reader_.set_als_init_status(AlsReader::AlsInitStatus::kDisabled);
  fake_als_reader_.ReportReaderInitialized();
  scoped_task_environment_.RunUntilIdle();

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              base::nullopt /* global_curve */,
                              base::nullopt /* personal_curve */);
}

// AlsReader is |kSuccess| on later notification.
TEST_F(ModellerImplTest, AlsReaderEnabledOnNotification) {
  Init(AlsReader::AlsInitStatus::kInProgress,
       BrightnessMonitor::Status::kSuccess);

  test_observer_->CheckStatus(false /* is_model_initialized */,
                              base::nullopt /* global_curve */,
                              base::nullopt /* personal_curve */);

  fake_als_reader_.set_als_init_status(AlsReader::AlsInitStatus::kSuccess);
  fake_als_reader_.ReportReaderInitialized();
  scoped_task_environment_.RunUntilIdle();

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              modeller_->GetGlobalCurveForTesting(),
                              base::nullopt /* personal_curve */);
}

// BrightnessMonitor is |kDisabled| on later notification.
TEST_F(ModellerImplTest, BrightnessMonitorDisabledOnNotification) {
  Init(AlsReader::AlsInitStatus::kSuccess,
       BrightnessMonitor::Status::kInitializing);

  test_observer_->CheckStatus(false /* is_model_initialized */,
                              base::nullopt /* global_curve */,
                              base::nullopt /* personal_curve */);

  fake_brightness_monitor_.set_status(BrightnessMonitor::Status::kDisabled);
  fake_brightness_monitor_.ReportBrightnessMonitorInitialized();

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              base::nullopt /* global_curve */,
                              base::nullopt /* personal_curve */);
}

// BrightnessMonitor is |kSuccess| on later notification.
TEST_F(ModellerImplTest, BrightnessMonitorEnabledOnNotification) {
  Init(AlsReader::AlsInitStatus::kSuccess,
       BrightnessMonitor::Status::kInitializing);

  test_observer_->CheckStatus(false /* is_model_initialized */,
                              base::nullopt /* global_curve */,
                              base::nullopt /* personal_curve */);

  fake_brightness_monitor_.set_status(BrightnessMonitor::Status::kSuccess);
  fake_brightness_monitor_.ReportBrightnessMonitorInitialized();
  scoped_task_environment_.RunUntilIdle();

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              modeller_->GetGlobalCurveForTesting(),
                              base::nullopt /* personal_curve */);
}

// There is no saved curve, hence a global curve is created.
TEST_F(ModellerImplTest, NoSavedCurve) {
  Init(AlsReader::AlsInitStatus::kSuccess, BrightnessMonitor::Status::kSuccess);

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              modeller_->GetGlobalCurveForTesting(),
                              base::nullopt /* personal_curve */);
}

// A curve is loaded from disk, this is a personal curve.
TEST_F(ModellerImplTest, CurveLoadedFromProfilePath) {
  const std::vector<double> xs = {0, 10, 20, 40, 60, 80, 90, 100};
  const std::vector<double> ys = {0, 5, 10, 15, 20, 25, 30, 40};
  MonotoneCubicSpline curve(xs, ys);

  WriteCurveToFile(curve);

  scoped_task_environment_.RunUntilIdle();

  Init(AlsReader::AlsInitStatus::kSuccess, BrightnessMonitor::Status::kSuccess);

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              modeller_->GetGlobalCurveForTesting(), curve);
}

// A curve is loaded from disk, this is a personal curve. This personal curve
// doesn't satisfy Trainer slope constraint, hence it's ignored and the global
// curve is used instead.
TEST_F(ModellerImplTest, PersonalCurveError) {
  const std::vector<double> xs = {0, 10, 20, 40, 60, 80, 90, 100};
  const std::vector<double> ys = {0, 5, 10, 15, 20, 25, 30, 40};
  MonotoneCubicSpline curve(xs, ys);

  WriteCurveToFile(curve);

  scoped_task_environment_.RunUntilIdle();

  Init(AlsReader::AlsInitStatus::kSuccess, BrightnessMonitor::Status::kSuccess,
       true /* is_trainer_configured */, false /* is_personal_curve_valid */);

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              modeller_->GetGlobalCurveForTesting(),
                              base::nullopt /* personal_curve */);
}

// Ambient light values are received. We check average ambient light has been
// calculated from the past |kNumberAmbientValuesToTrack| samples only.
TEST_F(ModellerImplTest, OnAmbientLightUpdated) {
  Init(AlsReader::AlsInitStatus::kSuccess, BrightnessMonitor::Status::kSuccess);

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              modeller_->GetGlobalCurveForTesting(),
                              base::nullopt /* personal_curve */);

  const int first_lux = 1000;
  double running_sum = first_lux;
  fake_als_reader_.ReportAmbientLightUpdate(first_lux);
  for (int i = 1; i < ModellerImpl::kNumberAmbientValuesToTrack; ++i) {
    const int lux = i;
    fake_als_reader_.ReportAmbientLightUpdate(lux);
    running_sum += lux;
    EXPECT_DOUBLE_EQ(running_sum / (i + 1),
                     modeller_->AverageAmbientForTesting());
  }

  // Add another one should push the oldest |first_lux| out of the horizon.
  fake_als_reader_.ReportAmbientLightUpdate(100);
  running_sum = running_sum + 100 - first_lux;
  EXPECT_DOUBLE_EQ(running_sum / ModellerImpl::kNumberAmbientValuesToTrack,
                   modeller_->AverageAmbientForTesting());
}

// User brightness changes are received, training example cache reaches
// |kMaxTrainingDataPoints| to trigger early training. This all happens within a
// small window shorter than |kTrainingDelay|.
TEST_F(ModellerImplTest, OnUserBrightnessChanged) {
  Init(AlsReader::AlsInitStatus::kSuccess, BrightnessMonitor::Status::kSuccess);

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              modeller_->GetGlobalCurveForTesting(),
                              base::nullopt /* personal_curve */);

  std::vector<TrainingDataPoint> expected_data;
  for (size_t i = 0; i < ModellerImpl::kMaxTrainingDataPoints - 1; ++i) {
    EXPECT_EQ(i, modeller_->NumberTrainingDataPointsForTesting());
    scoped_task_environment_.FastForwardBy(
        base::TimeDelta::FromMilliseconds(1));
    const base::TimeTicks now =
        scoped_task_environment_.GetMockTickClock()->NowTicks();
    const int lux = i * 20;
    fake_als_reader_.ReportAmbientLightUpdate(lux);
    const double brightness_old = 10.0 + i;
    const double brightness_new = 20.0 + i;
    modeller_->OnUserBrightnessChanged(brightness_old, brightness_new);
    expected_data.push_back(
        {brightness_old, brightness_new,
         ConvertToLog(modeller_->AverageAmbientForTesting()), now});
  }

  // Training should not have started.
  EXPECT_EQ(ModellerImpl::kMaxTrainingDataPoints - 1,
            modeller_->NumberTrainingDataPointsForTesting());

  // Add one more data point to trigger the training early.
  scoped_task_environment_.FastForwardBy(base::TimeDelta::FromMilliseconds(1));
  const base::TimeTicks now =
      scoped_task_environment_.GetMockTickClock()->NowTicks();
  const double brightness_old = 85;
  const double brightness_new = 95;
  modeller_->OnUserBrightnessChanged(brightness_old, brightness_new);
  expected_data.push_back({brightness_old, brightness_new,
                           ConvertToLog(modeller_->AverageAmbientForTesting()),
                           now});
  scoped_task_environment_.RunUntilIdle();

  EXPECT_EQ(0u, modeller_->NumberTrainingDataPointsForTesting());

  const base::Optional<MonotoneCubicSpline>& result_curve =
      test_observer_->personal_curve();
  DCHECK(result_curve);

  const MonotoneCubicSpline expected_curve =
      CreateTestCurveFromTrainingData(expected_data);
  EXPECT_EQ(expected_curve, *result_curve);
}

// User activities resets timer used to start training.
TEST_F(ModellerImplTest, MultipleUserActivities) {
  Init(AlsReader::AlsInitStatus::kSuccess, BrightnessMonitor::Status::kSuccess);

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              modeller_->GetGlobalCurveForTesting(),
                              base::nullopt /* personal_curve */);

  fake_als_reader_.ReportAmbientLightUpdate(30);
  std::vector<TrainingDataPoint> expected_data;
  for (size_t i = 0; i < ModellerImpl::kMinTrainingDataPoints; ++i) {
    EXPECT_EQ(i, modeller_->NumberTrainingDataPointsForTesting());
    scoped_task_environment_.FastForwardBy(
        base::TimeDelta::FromMilliseconds(1));
    const base::TimeTicks now =
        scoped_task_environment_.GetMockTickClock()->NowTicks();
    const int lux = i * 20;
    fake_als_reader_.ReportAmbientLightUpdate(lux);
    const double brightness_old = 10.0 + i;
    const double brightness_new = 20.0 + i;
    modeller_->OnUserBrightnessChanged(brightness_old, brightness_new);
    expected_data.push_back(
        {brightness_old, brightness_new,
         ConvertToLog(modeller_->AverageAmbientForTesting()), now});
  }

  EXPECT_EQ(ModellerImpl::kMinTrainingDataPoints,
            modeller_->NumberTrainingDataPointsForTesting());

  scoped_task_environment_.FastForwardBy(ModellerImpl::kTrainingDelay -
                                         base::TimeDelta::FromSeconds(10));
  // A user activity is received, timer should be reset.
  const ui::MouseEvent mouse_event(ui::ET_MOUSE_EXITED, gfx::Point(0, 0),
                                   gfx::Point(0, 0), base::TimeTicks(), 0, 0);
  modeller_->OnUserActivity(&mouse_event);

  scoped_task_environment_.FastForwardBy(ModellerImpl::kTrainingDelay -
                                         base::TimeDelta::FromSeconds(2));
  EXPECT_EQ(ModellerImpl::kMinTrainingDataPoints,
            modeller_->NumberTrainingDataPointsForTesting());

  // Another user event is received.
  modeller_->OnUserActivity(&mouse_event);

  // After |kTrainingDelay| - 2 seconds, no training has started.
  scoped_task_environment_.FastForwardBy(ModellerImpl::kTrainingDelay -
                                         base::TimeDelta::FromSeconds(2));
  scoped_task_environment_.RunUntilIdle();
  EXPECT_EQ(ModellerImpl::kMinTrainingDataPoints,
            modeller_->NumberTrainingDataPointsForTesting());

  // After another 2 seconds, training is scheduled.
  scoped_task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(2));
  scoped_task_environment_.RunUntilIdle();

  EXPECT_EQ(0u, modeller_->NumberTrainingDataPointsForTesting());
  const base::Optional<MonotoneCubicSpline>& result_curve =
      test_observer_->personal_curve();
  DCHECK(result_curve);

  const MonotoneCubicSpline expected_curve =
      CreateTestCurveFromTrainingData(expected_data);
  EXPECT_EQ(expected_curve, *result_curve);
}

// No training is done because number of training data points is less than
// |kMinTrainingDataPoints|.
TEST_F(ModellerImplTest, MinTrainingDataPointsRequired) {
  Init(AlsReader::AlsInitStatus::kSuccess, BrightnessMonitor::Status::kSuccess);

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              modeller_->GetGlobalCurveForTesting(),
                              base::nullopt /* personal_curve */);

  fake_als_reader_.ReportAmbientLightUpdate(30);
  modeller_->OnUserBrightnessChanged(10, 20);

  // No training is done because we have too few training data points.
  scoped_task_environment_.FastForwardBy(ModellerImpl::kTrainingDelay +
                                         base::TimeDelta::FromSeconds(10));
  scoped_task_environment_.RunUntilIdle();
  EXPECT_EQ(1u, modeller_->NumberTrainingDataPointsForTesting());

  EXPECT_FALSE(test_observer_->trained_curve_received());
}

// Global curve specified by valid experiment parameter.
TEST_F(ModellerImplTest, GlobaCurveFromValidExperimentParam) {
  const std::string global_curve_spec("1,10\n2,20\n3,30");

  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitAndEnableFeatureWithParameters(
      features::kAutoScreenBrightness, {{"global_curve", global_curve_spec}});

  const MonotoneCubicSpline expected_global_curve({1, 2, 3}, {10, 20, 30});

  Init(AlsReader::AlsInitStatus::kSuccess, BrightnessMonitor::Status::kSuccess);
  EXPECT_EQ(modeller_->GetGlobalCurveForTesting(), expected_global_curve);

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              expected_global_curve,
                              base::nullopt /* personal_curve */);
}

// Global curve specified by invalid experiment parameter leads to default curve
// instead.
TEST_F(ModellerImplTest, GlobaCurveFromInvalidExperimentParam) {
  const std::string global_curve_spec("1,10");

  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitAndEnableFeatureWithParameters(
      features::kAutoScreenBrightness, {{"global_curve", global_curve_spec}});

  // Defined by default values.
  const MonotoneCubicSpline expected_global_curve({0, 100}, {50, 100});

  Init(AlsReader::AlsInitStatus::kSuccess, BrightnessMonitor::Status::kSuccess);
  EXPECT_EQ(modeller_->GetGlobalCurveForTesting(), expected_global_curve);

  test_observer_->CheckStatus(true /* is_model_initialized */,
                              expected_global_curve,
                              base::nullopt /* personal_curve */);
}

}  // namespace auto_screen_brightness
}  // namespace power
}  // namespace chromeos
