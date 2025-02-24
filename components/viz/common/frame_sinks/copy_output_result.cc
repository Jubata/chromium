// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/common/frame_sinks/copy_output_result.h"

#include "base/logging.h"
#include "third_party/libyuv/include/libyuv.h"

namespace viz {

CopyOutputResult::CopyOutputResult(Format format, const gfx::Rect& rect)
    : format_(format), rect_(rect) {
  DCHECK(format_ == Format::RGBA_BITMAP || format_ == Format::RGBA_TEXTURE ||
         format_ == Format::I420_PLANES);
}

CopyOutputResult::~CopyOutputResult() = default;

bool CopyOutputResult::IsEmpty() const {
  if (rect_.IsEmpty())
    return true;
  switch (format_) {
    case Format::RGBA_BITMAP:
    case Format::I420_PLANES:
      return false;
    case Format::RGBA_TEXTURE:
      if (auto* mailbox = GetTextureMailbox())
        return !mailbox->IsTexture();
      else
        return true;
  }
  NOTREACHED();
  return true;
}

const SkBitmap& CopyOutputResult::AsSkBitmap() const {
  return cached_bitmap_;
}

const TextureMailbox* CopyOutputResult::GetTextureMailbox() const {
  return nullptr;
}

std::unique_ptr<SingleReleaseCallback>
CopyOutputResult::TakeTextureOwnership() {
  return nullptr;
}

bool CopyOutputResult::ReadI420Planes(uint8_t* y_out,
                                      int y_out_stride,
                                      uint8_t* u_out,
                                      int u_out_stride,
                                      uint8_t* v_out,
                                      int v_out_stride) const {
  const SkBitmap& bitmap = AsSkBitmap();
  if (!bitmap.readyToDraw())
    return false;
  const uint8_t* pixels = static_cast<uint8_t*>(bitmap.getPixels());
  // TODO(crbug/758057): The conversion below ignores color space completely.
  if (bitmap.colorType() == kBGRA_8888_SkColorType) {
    return 0 == libyuv::ARGBToI420(pixels, bitmap.rowBytes(), y_out,
                                   y_out_stride, u_out, u_out_stride, v_out,
                                   v_out_stride, bitmap.width(),
                                   bitmap.height());
  } else if (bitmap.colorType() == kRGBA_8888_SkColorType) {
    return 0 == libyuv::ABGRToI420(pixels, bitmap.rowBytes(), y_out,
                                   y_out_stride, u_out, u_out_stride, v_out,
                                   v_out_stride, bitmap.width(),
                                   bitmap.height());
  }

  // Other SkBitmap color types could be supported, but are currently never
  // being used.
  NOTIMPLEMENTED();
  return false;
}

CopyOutputSkBitmapResult::CopyOutputSkBitmapResult(const gfx::Rect& rect,
                                                   const SkBitmap& bitmap)
    : CopyOutputSkBitmapResult(Format::RGBA_BITMAP, rect, bitmap) {}

CopyOutputSkBitmapResult::CopyOutputSkBitmapResult(
    CopyOutputResult::Format format,
    const gfx::Rect& rect,
    const SkBitmap& bitmap)
    : CopyOutputResult(format, rect) {
  DCHECK(format == Format::RGBA_BITMAP || format == Format::I420_PLANES);
  if (!rect.IsEmpty()) {
    // Hold a reference to the |bitmap|'s pixels, for AsSkBitmap().
    *(cached_bitmap()) = bitmap;
  }
}

const SkBitmap& CopyOutputSkBitmapResult::AsSkBitmap() const {
  SkBitmap* const bitmap = cached_bitmap();

  if (rect().IsEmpty())
    return *bitmap;  // Return "null" bitmap for empty result.

  const SkImageInfo image_info = SkImageInfo::MakeN32Premul(
      rect().width(), rect().height(), bitmap->refColorSpace());
  if (bitmap->info() == image_info && bitmap->readyToDraw())
    return *bitmap;  // Return bitmap in expected format.

  // The bitmap is not in the "native optimized" format. Convert it once for
  // this and all future calls of this method.
  SkBitmap replacement;
  replacement.allocPixels(image_info);
  replacement.eraseColor(SK_ColorBLACK);
  SkPixmap src_pixmap;
  if (bitmap->peekPixels(&src_pixmap)) {
    // Note: writePixels() can fail, but then the replacement bitmap will be
    // left with part/all solid black due to the eraseColor() call above.
    replacement.writePixels(src_pixmap);
  }
  *bitmap = replacement;

  return *bitmap;
}

CopyOutputSkBitmapResult::~CopyOutputSkBitmapResult() = default;

CopyOutputTextureResult::CopyOutputTextureResult(
    const gfx::Rect& rect,
    const TextureMailbox& texture_mailbox,
    std::unique_ptr<SingleReleaseCallback> release_callback)
    : CopyOutputResult(Format::RGBA_TEXTURE, rect),
      texture_mailbox_(texture_mailbox),
      release_callback_(std::move(release_callback)) {
  DCHECK(rect.IsEmpty() || texture_mailbox_.IsTexture());
  DCHECK(release_callback_ || !texture_mailbox_.IsTexture());
}

CopyOutputTextureResult::~CopyOutputTextureResult() {
  if (release_callback_)
    release_callback_->Run(gpu::SyncToken(), false);
}

const TextureMailbox* CopyOutputTextureResult::GetTextureMailbox() const {
  return &texture_mailbox_;
}

std::unique_ptr<SingleReleaseCallback>
CopyOutputTextureResult::TakeTextureOwnership() {
  texture_mailbox_ = TextureMailbox();
  return std::move(release_callback_);
}

}  // namespace viz
