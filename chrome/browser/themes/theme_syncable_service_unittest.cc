// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/themes/theme_syncable_service.h"

#include <memory>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/files/file_path.h"
#include "base/memory/ptr_util.h"
#include "base/run_loop.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/extensions/test_extension_system.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/common/extensions/extension_test_util.h"
#include "chrome/test/base/testing_profile.h"
#include "components/sync/model/fake_sync_change_processor.h"
#include "components/sync/model/sync_change_processor_wrapper_for_test.h"
#include "components/sync/model/sync_error.h"
#include "components/sync/model/sync_error_factory_mock.h"
#include "components/sync/protocol/sync.pb.h"
#include "components/sync/protocol/theme_specifics.pb.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest_constants.h"
#include "extensions/common/manifest_url_handlers.h"
#include "extensions/common/permissions/api_permission_set.h"
#include "extensions/common/permissions/permission_set.h"
#include "testing/gtest/include/gtest/gtest.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/chromeos/login/users/scoped_test_user_manager.h"
#include "chrome/browser/chromeos/settings/scoped_cros_settings_test_helper.h"
#endif

using std::string;

namespace {

static const char kCustomThemeName[] = "name";
static const char kCustomThemeUrl[] = "http://update.url/foo";

#if defined(OS_WIN)
const base::FilePath::CharType kExtensionFilePath[] =
    FILE_PATH_LITERAL("c:\\foo");
#elif defined(OS_POSIX)
const base::FilePath::CharType kExtensionFilePath[] = FILE_PATH_LITERAL("/oo");
#endif

class FakeThemeService : public ThemeService {
 public:
  FakeThemeService() {}

  // ThemeService implementation
  void DoSetTheme(const extensions::Extension* extension,
                  bool suppress_infobar) override {
    is_dirty_ = true;
    theme_extension_ = extension;
    using_system_theme_ = false;
    using_default_theme_ = false;
    might_show_infobar_ = !suppress_infobar;
  }

  void UseDefaultTheme() override {
    is_dirty_ = true;
    using_default_theme_ = true;
    using_system_theme_ = false;
    theme_extension_ = NULL;
  }

  void UseSystemTheme() override {
    is_dirty_ = true;
    using_system_theme_ = true;
    using_default_theme_ = false;
    theme_extension_ = NULL;
  }

  bool IsSystemThemeDistinctFromDefaultTheme() const override {
    return distinct_from_default_theme_;
  }

  void set_distinct_from_default_theme(bool is_distinct) {
    distinct_from_default_theme_ = is_distinct;
  }

  bool UsingDefaultTheme() const override { return using_default_theme_; }

  bool UsingSystemTheme() const override { return using_system_theme_; }

  string GetThemeID() const override {
    if (theme_extension_.get())
      return theme_extension_->id();
    else
      return std::string();
  }

  const extensions::Extension* theme_extension() const {
    return theme_extension_.get();
  }

  bool is_dirty() const { return is_dirty_; }

  void MarkClean() { is_dirty_ = false; }

  bool might_show_infobar() const { return might_show_infobar_; }

 private:
  bool using_system_theme_ = false;
  bool using_default_theme_ = false;
  bool distinct_from_default_theme_ = false;
  scoped_refptr<const extensions::Extension> theme_extension_;
  bool is_dirty_ = false;
  bool might_show_infobar_ = false;
};

std::unique_ptr<KeyedService> BuildMockThemeService(
    content::BrowserContext* profile) {
  return base::WrapUnique(new FakeThemeService);
}

scoped_refptr<extensions::Extension> MakeThemeExtension(
    const base::FilePath& extension_path,
    const string& name,
    extensions::Manifest::Location location,
    const string& update_url) {
  base::DictionaryValue source;
  source.SetString(extensions::manifest_keys::kName, name);
  source.Set(extensions::manifest_keys::kTheme,
             std::make_unique<base::DictionaryValue>());
  source.SetString(extensions::manifest_keys::kUpdateURL, update_url);
  source.SetString(extensions::manifest_keys::kVersion, "0.0.0.0");
  string error;
  scoped_refptr<extensions::Extension> extension =
      extensions::Extension::Create(
          extension_path, location, source,
          extensions::Extension::NO_FLAGS, &error);
  EXPECT_TRUE(extension.get());
  EXPECT_EQ("", error);
  return extension;
}

}  // namespace

class ThemeSyncableServiceTest : public testing::Test {
 protected:
  ThemeSyncableServiceTest() : fake_theme_service_(NULL) {}

  ~ThemeSyncableServiceTest() override {}

  void SetUp() override {
    // Setting a matching update URL is necessary to make the test theme
    // considered syncable.
    extension_test_util::SetGalleryUpdateURL(GURL(kCustomThemeUrl));

    profile_.reset(new TestingProfile);
    fake_theme_service_ = BuildForProfile(profile_.get());
    theme_sync_service_.reset(new ThemeSyncableService(profile_.get(),
                                                       fake_theme_service_));
    fake_change_processor_.reset(new syncer::FakeSyncChangeProcessor);
    SetUpExtension();
  }

  void TearDown() override {
    profile_.reset();
    base::RunLoop().RunUntilIdle();
  }

  void SetUpExtension() {
    base::CommandLine command_line(base::CommandLine::NO_PROGRAM);
    extensions::TestExtensionSystem* test_ext_system =
        static_cast<extensions::TestExtensionSystem*>(
                extensions::ExtensionSystem::Get(profile_.get()));
    extensions::ExtensionService* service =
        test_ext_system->CreateExtensionService(
            &command_line, base::FilePath(kExtensionFilePath), false);
    EXPECT_TRUE(service->extensions_enabled());
    service->Init();
    base::RunLoop().RunUntilIdle();

    // Create and add custom theme extension so the ThemeSyncableService can
    // find it.
    theme_extension_ = MakeThemeExtension(base::FilePath(kExtensionFilePath),
                                          kCustomThemeName,
                                          GetThemeLocation(),
                                          kCustomThemeUrl);
    extensions::ExtensionPrefs::Get(profile_.get())
        ->AddGrantedPermissions(theme_extension_->id(),
                                extensions::PermissionSet());
    service->AddExtension(theme_extension_.get());
    extensions::ExtensionRegistry* registry =
        extensions::ExtensionRegistry::Get(profile_.get());
    ASSERT_EQ(1u, registry->enabled_extensions().size());
  }

  // Overridden in PolicyInstalledThemeTest below.
  virtual extensions::Manifest::Location GetThemeLocation() {
    return extensions::Manifest::INTERNAL;
  }

  FakeThemeService* BuildForProfile(Profile* profile) {
    return static_cast<FakeThemeService*>(
        ThemeServiceFactory::GetInstance()->SetTestingFactoryAndUse(
            profile, base::BindRepeating(&BuildMockThemeService)));
  }

  syncer::SyncDataList MakeThemeDataList(
      const sync_pb::ThemeSpecifics& theme_specifics) {
    syncer::SyncDataList list;
    sync_pb::EntitySpecifics entity_specifics;
    entity_specifics.mutable_theme()->CopyFrom(theme_specifics);
    list.push_back(syncer::SyncData::CreateLocalData(
        ThemeSyncableService::kCurrentThemeClientTag,
        ThemeSyncableService::kCurrentThemeNodeTitle,
        entity_specifics));
    return list;
  }

  // Needed for setting up extension service.
  content::TestBrowserThreadBundle test_browser_thread_bundle_;

#if defined OS_CHROMEOS
  chromeos::ScopedCrosSettingsTestHelper cros_settings_test_helper_;
  chromeos::ScopedTestUserManager test_user_manager_;
#endif

  std::unique_ptr<TestingProfile> profile_;
  FakeThemeService* fake_theme_service_;
  scoped_refptr<extensions::Extension> theme_extension_;
  std::unique_ptr<ThemeSyncableService> theme_sync_service_;
  std::unique_ptr<syncer::FakeSyncChangeProcessor> fake_change_processor_;
};

class PolicyInstalledThemeTest : public ThemeSyncableServiceTest {
  extensions::Manifest::Location GetThemeLocation() override {
    return extensions::Manifest::EXTERNAL_POLICY_DOWNLOAD;
  }
};

TEST_F(ThemeSyncableServiceTest, AreThemeSpecificsEqual) {
  sync_pb::ThemeSpecifics a, b;
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, false));
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, true));

  // Custom vs. non-custom.

  a.set_use_custom_theme(true);
  EXPECT_FALSE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, false));
  EXPECT_FALSE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, true));

  // Custom theme equality.

  b.set_use_custom_theme(true);
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, false));
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, true));

  a.set_custom_theme_id("id");
  EXPECT_FALSE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, false));
  EXPECT_FALSE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, true));

  b.set_custom_theme_id("id");
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, false));
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, true));

  a.set_custom_theme_update_url("http://update.url");
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, false));
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, true));

  a.set_custom_theme_name("name");
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, false));
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, true));

  // Non-custom theme equality.

  a.set_use_custom_theme(false);
  b.set_use_custom_theme(false);
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, false));
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, true));

  a.set_use_system_theme_by_default(true);
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, false));
  EXPECT_FALSE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, true));

  b.set_use_system_theme_by_default(true);
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, false));
  EXPECT_TRUE(ThemeSyncableService::AreThemeSpecificsEqual(a, b, true));
}

TEST_F(ThemeSyncableServiceTest, SetCurrentThemeDefaultTheme) {
  // Set up theme service to use custom theme.
  fake_theme_service_->SetTheme(theme_extension_.get());

  syncer::SyncError error =
      theme_sync_service_
          ->MergeDataAndStartSyncing(
              syncer::THEMES, MakeThemeDataList(sync_pb::ThemeSpecifics()),
              std::unique_ptr<syncer::SyncChangeProcessor>(
                  new syncer::SyncChangeProcessorWrapperForTest(
                      fake_change_processor_.get())),
              std::unique_ptr<syncer::SyncErrorFactory>(
                  new syncer::SyncErrorFactoryMock()))
          .error();
  EXPECT_FALSE(error.IsSet()) << error.message();
  EXPECT_FALSE(fake_theme_service_->UsingDefaultTheme());
  EXPECT_EQ(fake_theme_service_->theme_extension(), theme_extension_.get());
}

TEST_F(ThemeSyncableServiceTest, SetCurrentThemeSystemTheme) {
  sync_pb::ThemeSpecifics theme_specifics;
  theme_specifics.set_use_system_theme_by_default(true);

  // Set up theme service to use custom theme.
  fake_theme_service_->SetTheme(theme_extension_.get());
  syncer::SyncError error =
      theme_sync_service_
          ->MergeDataAndStartSyncing(
              syncer::THEMES, MakeThemeDataList(theme_specifics),
              std::unique_ptr<syncer::SyncChangeProcessor>(
                  new syncer::SyncChangeProcessorWrapperForTest(
                      fake_change_processor_.get())),
              std::unique_ptr<syncer::SyncErrorFactory>(
                  new syncer::SyncErrorFactoryMock()))
          .error();
  EXPECT_FALSE(error.IsSet()) << error.message();
  EXPECT_FALSE(fake_theme_service_->UsingSystemTheme());
  EXPECT_EQ(fake_theme_service_->theme_extension(), theme_extension_.get());
}

TEST_F(ThemeSyncableServiceTest, SetCurrentThemeCustomTheme) {
  sync_pb::ThemeSpecifics theme_specifics;
  theme_specifics.set_use_custom_theme(true);
  theme_specifics.set_custom_theme_id(theme_extension_->id());
  theme_specifics.set_custom_theme_name(kCustomThemeName);
  theme_specifics.set_custom_theme_name(kCustomThemeUrl);

  // Set up theme service to use default theme.
  fake_theme_service_->UseDefaultTheme();
  syncer::SyncError error =
      theme_sync_service_
          ->MergeDataAndStartSyncing(
              syncer::THEMES, MakeThemeDataList(theme_specifics),
              std::unique_ptr<syncer::SyncChangeProcessor>(
                  new syncer::SyncChangeProcessorWrapperForTest(
                      fake_change_processor_.get())),
              std::unique_ptr<syncer::SyncErrorFactory>(
                  new syncer::SyncErrorFactoryMock()))
          .error();
  EXPECT_FALSE(error.IsSet()) << error.message();
  EXPECT_EQ(fake_theme_service_->theme_extension(), theme_extension_.get());
}

TEST_F(ThemeSyncableServiceTest, DontResetThemeWhenSpecificsAreEqual) {
  // Set up theme service to use default theme and expect no changes.
  fake_theme_service_->UseDefaultTheme();
  fake_theme_service_->MarkClean();
  syncer::SyncError error =
      theme_sync_service_
          ->MergeDataAndStartSyncing(
              syncer::THEMES, MakeThemeDataList(sync_pb::ThemeSpecifics()),
              std::unique_ptr<syncer::SyncChangeProcessor>(
                  new syncer::SyncChangeProcessorWrapperForTest(
                      fake_change_processor_.get())),
              std::unique_ptr<syncer::SyncErrorFactory>(
                  new syncer::SyncErrorFactoryMock()))
          .error();
  EXPECT_FALSE(error.IsSet()) << error.message();
  EXPECT_FALSE(fake_theme_service_->is_dirty());
}

TEST_F(ThemeSyncableServiceTest, UpdateThemeSpecificsFromCurrentTheme) {
  // Set up theme service to use custom theme.
  fake_theme_service_->SetTheme(theme_extension_.get());

  syncer::SyncError error =
      theme_sync_service_
          ->MergeDataAndStartSyncing(
              syncer::THEMES, syncer::SyncDataList(),
              std::unique_ptr<syncer::SyncChangeProcessor>(
                  new syncer::SyncChangeProcessorWrapperForTest(
                      fake_change_processor_.get())),
              std::unique_ptr<syncer::SyncErrorFactory>(
                  new syncer::SyncErrorFactoryMock()))
          .error();
  EXPECT_FALSE(error.IsSet()) << error.message();
  const syncer::SyncChangeList& changes = fake_change_processor_->changes();
  ASSERT_EQ(1u, changes.size());
  EXPECT_TRUE(changes[0].IsValid());
  EXPECT_EQ(syncer::SyncChange::ACTION_ADD, changes[0].change_type());
  EXPECT_EQ(syncer::THEMES, changes[0].sync_data().GetDataType());

  const sync_pb::ThemeSpecifics& theme_specifics =
      changes[0].sync_data().GetSpecifics().theme();
  EXPECT_TRUE(theme_specifics.use_custom_theme());
  EXPECT_EQ(theme_extension_->id(), theme_specifics.custom_theme_id());
  EXPECT_EQ(theme_extension_->name(), theme_specifics.custom_theme_name());
  EXPECT_EQ(
      extensions::ManifestURL::GetUpdateURL(theme_extension_.get()).spec(),
      theme_specifics.custom_theme_update_url());
}

TEST_F(ThemeSyncableServiceTest, GetAllSyncData) {
  // Set up theme service to use custom theme.
  fake_theme_service_->SetTheme(theme_extension_.get());

  syncer::SyncDataList data_list =
      theme_sync_service_->GetAllSyncData(syncer::THEMES);

  ASSERT_EQ(1u, data_list.size());
  const sync_pb::ThemeSpecifics& theme_specifics =
      data_list[0].GetSpecifics().theme();
  EXPECT_TRUE(theme_specifics.use_custom_theme());
  EXPECT_EQ(theme_extension_->id(), theme_specifics.custom_theme_id());
  EXPECT_EQ(theme_extension_->name(), theme_specifics.custom_theme_name());
  EXPECT_EQ(
      extensions::ManifestURL::GetUpdateURL(theme_extension_.get()).spec(),
      theme_specifics.custom_theme_update_url());
}

TEST_F(ThemeSyncableServiceTest, ProcessSyncThemeChange) {
  // Set up theme service to use default theme.
  fake_theme_service_->UseDefaultTheme();
  fake_theme_service_->MarkClean();

  // Start syncing.
  syncer::SyncError error =
      theme_sync_service_
          ->MergeDataAndStartSyncing(
              syncer::THEMES, MakeThemeDataList(sync_pb::ThemeSpecifics()),
              std::unique_ptr<syncer::SyncChangeProcessor>(
                  new syncer::SyncChangeProcessorWrapperForTest(
                      fake_change_processor_.get())),
              std::unique_ptr<syncer::SyncErrorFactory>(
                  new syncer::SyncErrorFactoryMock()))
          .error();
  EXPECT_FALSE(error.IsSet()) << error.message();
  // Don't expect theme change initially because specifics are equal.
  EXPECT_FALSE(fake_theme_service_->is_dirty());

  // Change specifics to use custom theme and update.
  sync_pb::ThemeSpecifics theme_specifics;
  theme_specifics.set_use_custom_theme(true);
  theme_specifics.set_custom_theme_id(theme_extension_->id());
  theme_specifics.set_custom_theme_name(kCustomThemeName);
  theme_specifics.set_custom_theme_name(kCustomThemeUrl);
  sync_pb::EntitySpecifics entity_specifics;
  entity_specifics.mutable_theme()->CopyFrom(theme_specifics);
  syncer::SyncChangeList change_list;
  change_list.push_back(syncer::SyncChange(
      FROM_HERE, syncer::SyncChange::ACTION_UPDATE,
      syncer::SyncData::CreateRemoteData(1, entity_specifics, base::Time())));
  error = theme_sync_service_->ProcessSyncChanges(FROM_HERE, change_list);
  EXPECT_FALSE(error.IsSet()) << error.message();
  EXPECT_EQ(fake_theme_service_->theme_extension(), theme_extension_.get());
  // Don't show an infobar for theme installation. Regression test for
  // crbug.com/731688
  EXPECT_FALSE(fake_theme_service_->might_show_infobar());
}

TEST_F(ThemeSyncableServiceTest, OnThemeChangeByUser) {
  // Set up theme service to use default theme.
  fake_theme_service_->UseDefaultTheme();

  // Start syncing.
  syncer::SyncError error =
      theme_sync_service_
          ->MergeDataAndStartSyncing(
              syncer::THEMES, MakeThemeDataList(sync_pb::ThemeSpecifics()),
              std::unique_ptr<syncer::SyncChangeProcessor>(
                  new syncer::SyncChangeProcessorWrapperForTest(
                      fake_change_processor_.get())),
              std::unique_ptr<syncer::SyncErrorFactory>(
                  new syncer::SyncErrorFactoryMock()))
          .error();
  EXPECT_FALSE(error.IsSet()) << error.message();
  const syncer::SyncChangeList& changes = fake_change_processor_->changes();
  EXPECT_EQ(0u, changes.size());

  // Change current theme to custom theme and notify theme_sync_service_.
  fake_theme_service_->SetTheme(theme_extension_.get());
  theme_sync_service_->OnThemeChange();
  EXPECT_EQ(1u, changes.size());
  const sync_pb::ThemeSpecifics& change_specifics =
      changes[0].sync_data().GetSpecifics().theme();
  EXPECT_TRUE(change_specifics.use_custom_theme());
  EXPECT_EQ(theme_extension_->id(), change_specifics.custom_theme_id());
  EXPECT_EQ(theme_extension_->name(), change_specifics.custom_theme_name());
  EXPECT_EQ(
      extensions::ManifestURL::GetUpdateURL(theme_extension_.get()).spec(),
      change_specifics.custom_theme_update_url());
}

TEST_F(ThemeSyncableServiceTest, StopSync) {
  // Set up theme service to use default theme.
  fake_theme_service_->UseDefaultTheme();

  // Start syncing.
  syncer::SyncError error =
      theme_sync_service_
          ->MergeDataAndStartSyncing(
              syncer::THEMES, MakeThemeDataList(sync_pb::ThemeSpecifics()),
              std::unique_ptr<syncer::SyncChangeProcessor>(
                  new syncer::SyncChangeProcessorWrapperForTest(
                      fake_change_processor_.get())),
              std::unique_ptr<syncer::SyncErrorFactory>(
                  new syncer::SyncErrorFactoryMock()))
          .error();
  EXPECT_FALSE(error.IsSet()) << error.message();
  const syncer::SyncChangeList& changes = fake_change_processor_->changes();
  EXPECT_EQ(0u, changes.size());

  // Stop syncing.
  theme_sync_service_->StopSyncing(syncer::THEMES);

  // Change current theme to custom theme and notify theme_sync_service_.
  // No change is output because sync has stopped.
  fake_theme_service_->SetTheme(theme_extension_.get());
  theme_sync_service_->OnThemeChange();
  EXPECT_EQ(0u, changes.size());

  // ProcessSyncChanges() should return error when sync has stopped.
  error = theme_sync_service_->ProcessSyncChanges(FROM_HERE, changes);
  EXPECT_TRUE(error.IsSet());
  EXPECT_EQ(syncer::THEMES, error.model_type());
  EXPECT_EQ("Theme syncable service is not started.", error.message());
}

TEST_F(ThemeSyncableServiceTest, RestoreSystemThemeBitWhenChangeToCustomTheme) {
  // Initialize to use system theme.
  fake_theme_service_->UseDefaultTheme();
  sync_pb::ThemeSpecifics theme_specifics;
  theme_specifics.set_use_system_theme_by_default(true);
  syncer::SyncError error =
      theme_sync_service_
          ->MergeDataAndStartSyncing(
              syncer::THEMES, MakeThemeDataList(theme_specifics),
              std::unique_ptr<syncer::SyncChangeProcessor>(
                  new syncer::SyncChangeProcessorWrapperForTest(
                      fake_change_processor_.get())),
              std::unique_ptr<syncer::SyncErrorFactory>(
                  new syncer::SyncErrorFactoryMock()))
          .error();

  // Change to custom theme and notify theme_sync_service_.
  // use_system_theme_by_default bit should be preserved.
  fake_theme_service_->SetTheme(theme_extension_.get());
  theme_sync_service_->OnThemeChange();
  const syncer::SyncChangeList& changes = fake_change_processor_->changes();
  EXPECT_EQ(1u, changes.size());
  const sync_pb::ThemeSpecifics& change_specifics =
      changes[0].sync_data().GetSpecifics().theme();
  EXPECT_TRUE(change_specifics.use_system_theme_by_default());
}

TEST_F(ThemeSyncableServiceTest, DistinctSystemTheme) {
  fake_theme_service_->set_distinct_from_default_theme(true);

  // Initialize to use native theme.
  fake_theme_service_->UseSystemTheme();
  fake_theme_service_->MarkClean();
  sync_pb::ThemeSpecifics theme_specifics;
  theme_specifics.set_use_system_theme_by_default(true);
  syncer::SyncError error =
      theme_sync_service_
          ->MergeDataAndStartSyncing(
              syncer::THEMES, MakeThemeDataList(theme_specifics),
              std::unique_ptr<syncer::SyncChangeProcessor>(
                  new syncer::SyncChangeProcessorWrapperForTest(
                      fake_change_processor_.get())),
              std::unique_ptr<syncer::SyncErrorFactory>(
                  new syncer::SyncErrorFactoryMock()))
          .error();
  EXPECT_FALSE(fake_theme_service_->is_dirty());

  // Change to default theme and notify theme_sync_service_.
  // use_system_theme_by_default bit should be false.
  fake_theme_service_->UseDefaultTheme();
  theme_sync_service_->OnThemeChange();
  syncer::SyncChangeList& changes = fake_change_processor_->changes();
  EXPECT_EQ(1u, changes.size());
  EXPECT_FALSE(changes[0]
                   .sync_data()
                   .GetSpecifics()
                   .theme()
                   .use_system_theme_by_default());

  // Change to native theme and notify theme_sync_service_.
  // use_system_theme_by_default bit should be true.
  changes.clear();
  fake_theme_service_->UseSystemTheme();
  theme_sync_service_->OnThemeChange();
  EXPECT_EQ(1u, changes.size());
  EXPECT_TRUE(changes[0]
                  .sync_data()
                  .GetSpecifics()
                  .theme()
                  .use_system_theme_by_default());
}

TEST_F(ThemeSyncableServiceTest, SystemThemeSameAsDefaultTheme) {
  fake_theme_service_->set_distinct_from_default_theme(false);

  // Set up theme service to use default theme.
  fake_theme_service_->UseDefaultTheme();

  // Initialize to use custom theme with use_system_theme_by_default set true.
  sync_pb::ThemeSpecifics theme_specifics;
  theme_specifics.set_use_custom_theme(true);
  theme_specifics.set_custom_theme_id(theme_extension_->id());
  theme_specifics.set_custom_theme_name(kCustomThemeName);
  theme_specifics.set_custom_theme_name(kCustomThemeUrl);
  theme_specifics.set_use_system_theme_by_default(true);
  syncer::SyncError error =
      theme_sync_service_
          ->MergeDataAndStartSyncing(
              syncer::THEMES, MakeThemeDataList(theme_specifics),
              std::unique_ptr<syncer::SyncChangeProcessor>(
                  new syncer::SyncChangeProcessorWrapperForTest(
                      fake_change_processor_.get())),
              std::unique_ptr<syncer::SyncErrorFactory>(
                  new syncer::SyncErrorFactoryMock()))
          .error();
  EXPECT_EQ(fake_theme_service_->theme_extension(), theme_extension_.get());

  // Change to default theme and notify theme_sync_service_.
  // use_system_theme_by_default bit should be preserved.
  fake_theme_service_->UseDefaultTheme();
  theme_sync_service_->OnThemeChange();
  const syncer::SyncChangeList& changes = fake_change_processor_->changes();
  EXPECT_EQ(1u, changes.size());
  const sync_pb::ThemeSpecifics& change_specifics =
      changes[0].sync_data().GetSpecifics().theme();
  EXPECT_FALSE(change_specifics.use_custom_theme());
  EXPECT_TRUE(change_specifics.use_system_theme_by_default());
}

TEST_F(PolicyInstalledThemeTest, InstallThemeByPolicy) {
  // Set up theme service to use custom theme that was installed by policy.
  fake_theme_service_->SetTheme(theme_extension_.get());

  syncer::SyncDataList data_list =
      theme_sync_service_->GetAllSyncData(syncer::THEMES);

  ASSERT_EQ(0u, data_list.size());
}
