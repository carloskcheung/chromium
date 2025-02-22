// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/command_buffer/service/shared_image_backing_factory_gl_texture.h"

#include "gpu/command_buffer/common/gpu_memory_buffer_support.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/service/mailbox_manager_impl.h"
#include "gpu/command_buffer/service/raster_decoder_context_state.h"
#include "gpu/command_buffer/service/service_utils.h"
#include "gpu/command_buffer/service/shared_image_backing.h"
#include "gpu/command_buffer/service/shared_image_factory.h"
#include "gpu/command_buffer/service/shared_image_manager.h"
#include "gpu/command_buffer/service/shared_image_representation.h"
#include "gpu/command_buffer/service/texture_manager.h"
#include "gpu/command_buffer/tests/texture_image_factory.h"
#include "gpu/config/gpu_driver_bug_workarounds.h"
#include "gpu/config/gpu_feature_info.h"
#include "gpu/config/gpu_preferences.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/gpu/GrBackendSurface.h"
#include "ui/gfx/buffer_format_util.h"
#include "ui/gfx/color_space.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_image_shared_memory.h"
#include "ui/gl/gl_image_stub.h"
#include "ui/gl/gl_surface.h"
#include "ui/gl/init/gl_factory.h"

namespace gpu {
namespace {

class SharedImageBackingFactoryGLTextureTestBase
    : public testing::TestWithParam<bool> {
 public:
  void SetUpBase(const GpuDriverBugWorkarounds& workarounds,
                 ImageFactory* factory) {
    surface_ = gl::init::CreateOffscreenGLSurface(gfx::Size());
    ASSERT_TRUE(surface_);
    context_ = gl::init::CreateGLContext(nullptr, surface_.get(),
                                         gl::GLContextAttribs());
    ASSERT_TRUE(context_);
    bool result = context_->MakeCurrent(surface_.get());
    ASSERT_TRUE(result);

    GpuPreferences preferences;
    preferences.use_passthrough_cmd_decoder = use_passthrough();
    backing_factory_ = std::make_unique<SharedImageBackingFactoryGLTexture>(
        preferences, workarounds, GpuFeatureInfo(), factory);

    scoped_refptr<gl::GLShareGroup> share_group = new gl::GLShareGroup();
    context_state_ = new raster::RasterDecoderContextState(
        std::move(share_group), surface_, context_,
        false /* use_virtualized_gl_contexts */);
    context_state_->InitializeGrContext(workarounds, nullptr);

    memory_type_tracker_ = std::make_unique<MemoryTypeTracker>(nullptr);
    shared_image_representation_factory_ =
        std::make_unique<SharedImageRepresentationFactory>(
            &shared_image_manager_, nullptr);
  }

  bool use_passthrough() {
    return GetParam() && gles2::PassthroughCommandDecoderSupported();
  }

  GrContext* gr_context() { return context_state_->gr_context; }

 protected:
  scoped_refptr<gl::GLSurface> surface_;
  scoped_refptr<gl::GLContext> context_;
  scoped_refptr<raster::RasterDecoderContextState> context_state_;
  std::unique_ptr<SharedImageBackingFactoryGLTexture> backing_factory_;
  gles2::MailboxManagerImpl mailbox_manager_;
  SharedImageManager shared_image_manager_;
  std::unique_ptr<MemoryTypeTracker> memory_type_tracker_;
  std::unique_ptr<SharedImageRepresentationFactory>
      shared_image_representation_factory_;
};

class SharedImageBackingFactoryGLTextureTest
    : public SharedImageBackingFactoryGLTextureTestBase {
 public:
  void SetUp() override {
    GpuDriverBugWorkarounds workarounds;
    workarounds.max_texture_size = INT_MAX - 1;
    SetUpBase(workarounds, &image_factory_);
  }

 protected:
  TextureImageFactory image_factory_;
};

TEST_P(SharedImageBackingFactoryGLTextureTest, Basic) {
  auto mailbox = Mailbox::Generate();
  auto format = viz::ResourceFormat::RGBA_8888;
  gfx::Size size(256, 256);
  auto color_space = gfx::ColorSpace::CreateSRGB();
  uint32_t usage = SHARED_IMAGE_USAGE_GLES2;
  auto backing = backing_factory_->CreateSharedImage(mailbox, format, size,
                                                     color_space, usage);
  EXPECT_TRUE(backing);

  // Check clearing.
  if (!backing->IsCleared()) {
    backing->SetCleared();
    EXPECT_TRUE(backing->IsCleared());
  }

  // First, validate via a legacy mailbox.
  EXPECT_TRUE(backing->ProduceLegacyMailbox(&mailbox_manager_));
  TextureBase* texture_base = mailbox_manager_.ConsumeTexture(mailbox);
  ASSERT_TRUE(texture_base);
  GLenum expected_target = GL_TEXTURE_2D;
  EXPECT_EQ(texture_base->target(), expected_target);
  if (!use_passthrough()) {
    auto* texture = static_cast<gles2::Texture*>(texture_base);
    EXPECT_TRUE(texture->IsImmutable());
    int width, height, depth;
    bool has_level =
        texture->GetLevelSize(GL_TEXTURE_2D, 0, &width, &height, &depth);
    EXPECT_TRUE(has_level);
    EXPECT_EQ(width, size.width());
    EXPECT_EQ(height, size.height());
  }

  // Next, validate via a SharedImageRepresentationGLTexture.
  std::unique_ptr<SharedImageRepresentationFactoryRef> shared_image =
      shared_image_manager_.Register(std::move(backing),
                                     memory_type_tracker_.get());
  EXPECT_TRUE(shared_image);
  if (!use_passthrough()) {
    auto gl_representation =
        shared_image_representation_factory_->ProduceGLTexture(mailbox);
    EXPECT_TRUE(gl_representation);
    EXPECT_TRUE(gl_representation->GetTexture()->service_id());
    EXPECT_EQ(expected_target, gl_representation->GetTexture()->target());
    EXPECT_EQ(size, gl_representation->size());
    EXPECT_EQ(format, gl_representation->format());
    EXPECT_EQ(color_space, gl_representation->color_space());
    EXPECT_EQ(usage, gl_representation->usage());
    gl_representation.reset();
  }

  // Next, validate a SharedImageRepresentationGLTexturePassthrough.
  if (use_passthrough()) {
    auto gl_representation =
        shared_image_representation_factory_->ProduceGLTexturePassthrough(
            mailbox);
    EXPECT_TRUE(gl_representation);
    EXPECT_TRUE(gl_representation->GetTexturePassthrough()->service_id());
    EXPECT_EQ(expected_target,
              gl_representation->GetTexturePassthrough()->target());
    EXPECT_EQ(size, gl_representation->size());
    EXPECT_EQ(format, gl_representation->format());
    EXPECT_EQ(color_space, gl_representation->color_space());
    EXPECT_EQ(usage, gl_representation->usage());
    gl_representation.reset();
  }

  // Finally, validate a SharedImageRepresentationSkia.
  auto skia_representation =
      shared_image_representation_factory_->ProduceSkia(mailbox);
  EXPECT_TRUE(skia_representation);
  auto surface = skia_representation->BeginWriteAccess(
      gr_context(), 0, kRGBA_8888_SkColorType,
      SkSurfaceProps(0, kUnknown_SkPixelGeometry));
  EXPECT_TRUE(surface);
  EXPECT_EQ(size.width(), surface->width());
  EXPECT_EQ(size.height(), surface->height());
  skia_representation->EndWriteAccess(std::move(surface));
  GrBackendTexture backend_texture;
  EXPECT_TRUE(skia_representation->BeginReadAccess(

      kRGBA_8888_SkColorType, &backend_texture));
  EXPECT_EQ(size.width(), backend_texture.width());
  EXPECT_EQ(size.width(), backend_texture.width());
  skia_representation->EndReadAccess();
  skia_representation.reset();

  shared_image.reset();
  EXPECT_FALSE(mailbox_manager_.ConsumeTexture(mailbox));
}

TEST_P(SharedImageBackingFactoryGLTextureTest, Image) {
  auto mailbox = Mailbox::Generate();
  auto format = viz::ResourceFormat::RGBA_8888;
  gfx::Size size(256, 256);
  auto color_space = gfx::ColorSpace::CreateSRGB();
  uint32_t usage = SHARED_IMAGE_USAGE_SCANOUT;
  auto backing = backing_factory_->CreateSharedImage(mailbox, format, size,
                                                     color_space, usage);
  EXPECT_TRUE(backing);

  // Check clearing.
  if (!backing->IsCleared()) {
    backing->SetCleared();
    EXPECT_TRUE(backing->IsCleared());
  }

  // First, validate via a legacy mailbox.
  EXPECT_TRUE(backing->ProduceLegacyMailbox(&mailbox_manager_));
  TextureBase* texture_base = mailbox_manager_.ConsumeTexture(mailbox);
  ASSERT_TRUE(texture_base);
  GLenum target = texture_base->target();
  scoped_refptr<gl::GLImage> image;
  if (use_passthrough()) {
    auto* texture = static_cast<gles2::TexturePassthrough*>(texture_base);
    image = texture->GetLevelImage(target, 0);
  } else {
    auto* texture = static_cast<gles2::Texture*>(texture_base);
    image = texture->GetLevelImage(target, 0);
  }
  ASSERT_TRUE(image);
  EXPECT_EQ(size, image->GetSize());

  // Next, validate via a SharedImageRepresentationGLTexture.
  std::unique_ptr<SharedImageRepresentationFactoryRef> shared_image =
      shared_image_manager_.Register(std::move(backing),
                                     memory_type_tracker_.get());
  EXPECT_TRUE(shared_image);
  if (!use_passthrough()) {
    auto gl_representation =
        shared_image_representation_factory_->ProduceGLTexture(mailbox);
    EXPECT_TRUE(gl_representation);
    EXPECT_TRUE(gl_representation->GetTexture()->service_id());
    EXPECT_EQ(size, gl_representation->size());
    EXPECT_EQ(format, gl_representation->format());
    EXPECT_EQ(color_space, gl_representation->color_space());
    EXPECT_EQ(usage, gl_representation->usage());
    gl_representation.reset();
  }

  // Next, validate a SharedImageRepresentationGLTexturePassthrough.
  if (use_passthrough()) {
    auto gl_representation =
        shared_image_representation_factory_->ProduceGLTexturePassthrough(
            mailbox);
    EXPECT_TRUE(gl_representation);
    EXPECT_TRUE(gl_representation->GetTexturePassthrough()->service_id());
    EXPECT_EQ(size, gl_representation->size());
    EXPECT_EQ(format, gl_representation->format());
    EXPECT_EQ(color_space, gl_representation->color_space());
    EXPECT_EQ(usage, gl_representation->usage());
    gl_representation.reset();
  }

  // Finally, validate a SharedImageRepresentationSkia.
  auto skia_representation =
      shared_image_representation_factory_->ProduceSkia(mailbox);
  EXPECT_TRUE(skia_representation);
  auto surface = skia_representation->BeginWriteAccess(
      gr_context(), 0, kRGBA_8888_SkColorType,
      SkSurfaceProps(0, kUnknown_SkPixelGeometry));
  EXPECT_TRUE(surface);
  EXPECT_EQ(size.width(), surface->width());
  EXPECT_EQ(size.height(), surface->height());
  skia_representation->EndWriteAccess(std::move(surface));
  GrBackendTexture backend_texture;
  EXPECT_TRUE(skia_representation->BeginReadAccess(

      kRGBA_8888_SkColorType, &backend_texture));
  EXPECT_EQ(size.width(), backend_texture.width());
  EXPECT_EQ(size.width(), backend_texture.width());
  skia_representation->EndReadAccess();
  skia_representation.reset();

  shared_image.reset();
  EXPECT_FALSE(mailbox_manager_.ConsumeTexture(mailbox));

  if (!use_passthrough()) {
    // Create a R-8 image texture, and check that the internal_format is that of
    // the image (GL_RGBA for TextureImageFactory). This only matters for the
    // validating decoder.
    auto format = viz::ResourceFormat::RED_8;
    backing = backing_factory_->CreateSharedImage(mailbox, format, size,
                                                  color_space, usage);
    EXPECT_TRUE(backing);
    shared_image = shared_image_manager_.Register(std::move(backing),
                                                  memory_type_tracker_.get());
    auto gl_representation =
        shared_image_representation_factory_->ProduceGLTexture(mailbox);
    ASSERT_TRUE(gl_representation);
    gles2::Texture* texture = gl_representation->GetTexture();
    ASSERT_TRUE(texture);
    GLenum type = 0;
    GLenum internal_format = 0;
    EXPECT_TRUE(texture->GetLevelType(target, 0, &type, &internal_format));
    EXPECT_EQ(internal_format, static_cast<GLenum>(GL_RGBA));
    gl_representation.reset();
    shared_image.reset();
  }
}

TEST_P(SharedImageBackingFactoryGLTextureTest, InvalidFormat) {
  auto mailbox = Mailbox::Generate();
  auto format = viz::ResourceFormat::UYVY_422;
  gfx::Size size(256, 256);
  auto color_space = gfx::ColorSpace::CreateSRGB();
  uint32_t usage = SHARED_IMAGE_USAGE_GLES2;
  auto backing = backing_factory_->CreateSharedImage(mailbox, format, size,
                                                     color_space, usage);
  EXPECT_FALSE(backing);
}

TEST_P(SharedImageBackingFactoryGLTextureTest, InvalidSize) {
  auto mailbox = Mailbox::Generate();
  auto format = viz::ResourceFormat::RGBA_8888;
  gfx::Size size(0, 0);
  auto color_space = gfx::ColorSpace::CreateSRGB();
  uint32_t usage = SHARED_IMAGE_USAGE_GLES2;
  auto backing = backing_factory_->CreateSharedImage(mailbox, format, size,
                                                     color_space, usage);
  EXPECT_FALSE(backing);

  size = gfx::Size(INT_MAX, INT_MAX);
  backing = backing_factory_->CreateSharedImage(mailbox, format, size,
                                                color_space, usage);
  EXPECT_FALSE(backing);
}

TEST_P(SharedImageBackingFactoryGLTextureTest, EstimatedSize) {
  auto mailbox = Mailbox::Generate();
  auto format = viz::ResourceFormat::RGBA_8888;
  gfx::Size size(256, 256);
  auto color_space = gfx::ColorSpace::CreateSRGB();
  uint32_t usage = SHARED_IMAGE_USAGE_GLES2;
  auto backing = backing_factory_->CreateSharedImage(mailbox, format, size,
                                                     color_space, usage);
  EXPECT_TRUE(backing);

  size_t backing_estimated_size = backing->estimated_size();
  EXPECT_GT(backing_estimated_size, 0u);

  std::unique_ptr<SharedImageRepresentationFactoryRef> shared_image =
      shared_image_manager_.Register(std::move(backing),
                                     memory_type_tracker_.get());
  EXPECT_EQ(backing_estimated_size, memory_type_tracker_->GetMemRepresented());

  shared_image.reset();
}

class StubImage : public gl::GLImageStub {
 public:
  StubImage(const gfx::Size& size, gfx::BufferFormat format)
      : size_(size), format_(format) {}

  gfx::Size GetSize() override { return size_; }
  unsigned GetInternalFormat() override {
    return InternalFormatForGpuMemoryBufferFormat(format_);
  }

  bool BindTexImage(unsigned target) override {
    if (!bound_) {
      bound_ = true;
      ++update_counter_;
    }
    return true;
  };

  void ReleaseTexImage(unsigned target) override { bound_ = false; }

  bool bound() const { return bound_; }
  int update_counter() const { return update_counter_; }

 private:
  ~StubImage() override = default;

  gfx::Size size_;
  gfx::BufferFormat format_;
  bool bound_ = false;
  int update_counter_ = 0;
};

class SharedImageBackingFactoryGLTextureWithGMBTest
    : public SharedImageBackingFactoryGLTextureTestBase,
      public gpu::ImageFactory {
 public:
  void SetUp() override { SetUpBase(GpuDriverBugWorkarounds(), this); }

  scoped_refptr<gl::GLImage> GetImageFromMailbox(Mailbox mailbox) {
    if (!use_passthrough()) {
      auto representation =
          shared_image_representation_factory_->ProduceGLTexture(mailbox);
      DCHECK(representation);
      return representation->GetTexture()->GetLevelImage(GL_TEXTURE_2D, 0);
    } else {
      auto representation =
          shared_image_representation_factory_->ProduceGLTexturePassthrough(
              mailbox);
      DCHECK(representation);
      return representation->GetTexturePassthrough()->GetLevelImage(
          GL_TEXTURE_2D, 0);
    }
  }

 protected:
  // gpu::ImageFactory implementation.
  scoped_refptr<gl::GLImage> CreateImageForGpuMemoryBuffer(
      gfx::GpuMemoryBufferHandle handle,
      const gfx::Size& size,
      gfx::BufferFormat format,
      int client_id,
      gpu::SurfaceHandle surface_handle) override {
    // pretend to handle NATIVE_PIXMAP types.
    if (handle.type != gfx::NATIVE_PIXMAP)
      return nullptr;
    if (client_id != kClientId)
      return nullptr;
    return base::MakeRefCounted<StubImage>(size, format);
  }

  static constexpr int kClientId = 3;
};

TEST_P(SharedImageBackingFactoryGLTextureWithGMBTest,
       GpuMemoryBufferImportEmpty) {
  auto mailbox = Mailbox::Generate();
  gfx::Size size(256, 256);
  gfx::BufferFormat format = gfx::BufferFormat::RGBA_8888;
  auto color_space = gfx::ColorSpace::CreateSRGB();
  uint32_t usage = SHARED_IMAGE_USAGE_GLES2;

  gfx::GpuMemoryBufferHandle handle;
  auto backing = backing_factory_->CreateSharedImage(
      mailbox, kClientId, std::move(handle), format, kNullSurfaceHandle, size,
      color_space, usage);
  EXPECT_FALSE(backing);
}

TEST_P(SharedImageBackingFactoryGLTextureWithGMBTest,
       GpuMemoryBufferImportNative) {
  auto mailbox = Mailbox::Generate();
  gfx::Size size(256, 256);
  gfx::BufferFormat format = gfx::BufferFormat::RGBA_8888;
  auto color_space = gfx::ColorSpace::CreateSRGB();
  uint32_t usage = SHARED_IMAGE_USAGE_GLES2;

  gfx::GpuMemoryBufferHandle handle;
  handle.type = gfx::NATIVE_PIXMAP;
  auto backing = backing_factory_->CreateSharedImage(
      mailbox, kClientId, std::move(handle), format, kNullSurfaceHandle, size,
      color_space, usage);
  ASSERT_TRUE(backing);

  std::unique_ptr<SharedImageRepresentationFactoryRef> ref =
      shared_image_manager_.Register(std::move(backing),
                                     memory_type_tracker_.get());
  scoped_refptr<gl::GLImage> image = GetImageFromMailbox(mailbox);
  ASSERT_EQ(image->GetType(), gl::GLImage::Type::NONE);
  auto* stub_image = static_cast<StubImage*>(image.get());
  EXPECT_TRUE(stub_image->bound());
  int update_counter = stub_image->update_counter();
  ref->Update();
  EXPECT_TRUE(stub_image->bound());
  EXPECT_GT(stub_image->update_counter(), update_counter);
}

TEST_P(SharedImageBackingFactoryGLTextureWithGMBTest,
       GpuMemoryBufferImportSharedMemory) {
  auto mailbox = Mailbox::Generate();
  gfx::Size size(256, 256);
  gfx::BufferFormat format = gfx::BufferFormat::RGBA_8888;
  auto color_space = gfx::ColorSpace::CreateSRGB();
  uint32_t usage = SHARED_IMAGE_USAGE_GLES2;

  size_t shm_size = 0u;
  ASSERT_TRUE(gfx::BufferSizeForBufferFormatChecked(size, format, &shm_size));
  gfx::GpuMemoryBufferHandle handle;
  handle.type = gfx::SHARED_MEMORY_BUFFER;
  handle.region = base::UnsafeSharedMemoryRegion::Create(shm_size);
  ASSERT_TRUE(handle.region.IsValid());
  handle.offset = 0;
  handle.stride = static_cast<int32_t>(
      gfx::RowSizeForBufferFormat(size.width(), format, 0));

  auto backing = backing_factory_->CreateSharedImage(
      mailbox, kClientId, std::move(handle), format, kNullSurfaceHandle, size,
      color_space, usage);
  ASSERT_TRUE(backing);
  std::unique_ptr<SharedImageRepresentationFactoryRef> ref =
      shared_image_manager_.Register(std::move(backing),
                                     memory_type_tracker_.get());
  scoped_refptr<gl::GLImage> image = GetImageFromMailbox(mailbox);
  ASSERT_EQ(image->GetType(), gl::GLImage::Type::MEMORY);
  auto* shm_image = static_cast<gl::GLImageSharedMemory*>(image.get());
  EXPECT_EQ(size, shm_image->GetSize());
  EXPECT_EQ(format, shm_image->format());
}

INSTANTIATE_TEST_CASE_P(Service,
                        SharedImageBackingFactoryGLTextureTest,
                        ::testing::Bool());
INSTANTIATE_TEST_CASE_P(Service,
                        SharedImageBackingFactoryGLTextureWithGMBTest,
                        ::testing::Bool());

}  // anonymous namespace
}  // namespace gpu
