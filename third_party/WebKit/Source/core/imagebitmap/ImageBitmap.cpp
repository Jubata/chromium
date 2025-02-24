// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/imagebitmap/ImageBitmap.h"

#include <memory>
#include "base/memory/scoped_refptr.h"
#include "core/html/HTMLCanvasElement.h"
#include "core/html/ImageData.h"
#include "core/html/media/HTMLVideoElement.h"
#include "core/offscreencanvas/OffscreenCanvas.h"
#include "platform/CrossThreadFunctional.h"
#include "platform/graphics/CanvasColorParams.h"
#include "platform/graphics/UnacceleratedStaticBitmapImage.h"
#include "platform/graphics/skia/SkiaUtils.h"
#include "platform/image-decoders/ImageDecoder.h"
#include "platform/threading/BackgroundTaskRunner.h"
#include "platform/wtf/CheckedNumeric.h"
#include "platform/wtf/PtrUtil.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColorSpaceXformCanvas.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/core/SkSwizzle.h"
#include "third_party/skia/include/effects/SkColorFilterImageFilter.h"

namespace blink {

constexpr const char* kImageOrientationFlipY = "flipY";
constexpr const char* kImageBitmapOptionNone = "none";
constexpr const char* kImageBitmapOptionDefault = "default";
constexpr const char* kImageBitmapOptionPremultiply = "premultiply";
constexpr const char* kImageBitmapOptionResizeQualityHigh = "high";
constexpr const char* kImageBitmapOptionResizeQualityMedium = "medium";
constexpr const char* kImageBitmapOptionResizeQualityPixelated = "pixelated";
constexpr const char* kSRGBImageBitmapColorSpaceConversion = "srgb";
constexpr const char* kLinearRGBImageBitmapColorSpaceConversion = "linear-rgb";
constexpr const char* kP3ImageBitmapColorSpaceConversion = "p3";
constexpr const char* kRec2020ImageBitmapColorSpaceConversion = "rec2020";

namespace {

// The following two functions are helpers used in cropImage
static inline IntRect NormalizeRect(const IntRect& rect) {
  return IntRect(std::min(rect.X(), rect.MaxX()),
                 std::min(rect.Y(), rect.MaxY()),
                 std::max(rect.Width(), -rect.Width()),
                 std::max(rect.Height(), -rect.Height()));
}

ImageBitmap::ParsedOptions ParseOptions(const ImageBitmapOptions& options,
                                        Optional<IntRect> crop_rect,
                                        IntSize source_size) {
  ImageBitmap::ParsedOptions parsed_options;
  if (options.imageOrientation() == kImageOrientationFlipY) {
    parsed_options.flip_y = true;
  } else {
    parsed_options.flip_y = false;
    DCHECK(options.imageOrientation() == kImageBitmapOptionNone);
  }
  if (options.premultiplyAlpha() == kImageBitmapOptionNone) {
    parsed_options.premultiply_alpha = false;
  } else {
    parsed_options.premultiply_alpha = true;
    DCHECK(options.premultiplyAlpha() == kImageBitmapOptionDefault ||
           options.premultiplyAlpha() == kImageBitmapOptionPremultiply);
  }

  parsed_options.has_color_space_conversion =
      (options.colorSpaceConversion() != kImageBitmapOptionNone);
  parsed_options.color_params.SetCanvasColorSpace(kSRGBCanvasColorSpace);
  if (options.colorSpaceConversion() != kSRGBImageBitmapColorSpaceConversion &&
      options.colorSpaceConversion() != kImageBitmapOptionNone &&
      options.colorSpaceConversion() != kImageBitmapOptionDefault) {
    parsed_options.color_params.SetCanvasPixelFormat(kF16CanvasPixelFormat);
    if (options.colorSpaceConversion() ==
        kLinearRGBImageBitmapColorSpaceConversion) {
      parsed_options.color_params.SetCanvasColorSpace(kSRGBCanvasColorSpace);
    } else if (options.colorSpaceConversion() ==
               kP3ImageBitmapColorSpaceConversion) {
      parsed_options.color_params.SetCanvasColorSpace(kP3CanvasColorSpace);
    } else if (options.colorSpaceConversion() ==
               kRec2020ImageBitmapColorSpaceConversion) {
      parsed_options.color_params.SetCanvasColorSpace(kRec2020CanvasColorSpace);
    } else {
      NOTREACHED()
          << "Invalid ImageBitmap creation attribute colorSpaceConversion: "
          << options.colorSpaceConversion();
    }
  }

  int source_width = source_size.Width();
  int source_height = source_size.Height();
  if (!crop_rect) {
    parsed_options.crop_rect = IntRect(0, 0, source_width, source_height);
  } else {
    parsed_options.crop_rect = NormalizeRect(*crop_rect);
  }
  if (!options.hasResizeWidth() && !options.hasResizeHeight()) {
    parsed_options.resize_width = parsed_options.crop_rect.Width();
    parsed_options.resize_height = parsed_options.crop_rect.Height();
  } else if (options.hasResizeWidth() && options.hasResizeHeight()) {
    parsed_options.resize_width = options.resizeWidth();
    parsed_options.resize_height = options.resizeHeight();
  } else if (options.hasResizeWidth() && !options.hasResizeHeight()) {
    parsed_options.resize_width = options.resizeWidth();
    parsed_options.resize_height = ceil(
        static_cast<float>(options.resizeWidth()) /
        parsed_options.crop_rect.Width() * parsed_options.crop_rect.Height());
  } else {
    parsed_options.resize_height = options.resizeHeight();
    parsed_options.resize_width = ceil(
        static_cast<float>(options.resizeHeight()) /
        parsed_options.crop_rect.Height() * parsed_options.crop_rect.Width());
  }
  if (static_cast<int>(parsed_options.resize_width) ==
          parsed_options.crop_rect.Width() &&
      static_cast<int>(parsed_options.resize_height) ==
          parsed_options.crop_rect.Height()) {
    parsed_options.should_scale_input = false;
    return parsed_options;
  }
  parsed_options.should_scale_input = true;

  if (options.resizeQuality() == kImageBitmapOptionResizeQualityHigh)
    parsed_options.resize_quality = kHigh_SkFilterQuality;
  else if (options.resizeQuality() == kImageBitmapOptionResizeQualityMedium)
    parsed_options.resize_quality = kMedium_SkFilterQuality;
  else if (options.resizeQuality() == kImageBitmapOptionResizeQualityPixelated)
    parsed_options.resize_quality = kNone_SkFilterQuality;
  else
    parsed_options.resize_quality = kLow_SkFilterQuality;
  return parsed_options;
}

// The function dstBufferSizeHasOverflow() is being called at the beginning of
// each ImageBitmap() constructor, which makes sure that doing
// width * height * bytesPerPixel will never overflow unsigned.
bool DstBufferSizeHasOverflow(const ImageBitmap::ParsedOptions& options) {
  CheckedNumeric<unsigned> total_bytes = options.crop_rect.Width();
  total_bytes *= options.crop_rect.Height();
  total_bytes *=
      SkColorTypeBytesPerPixel(options.color_params.GetSkColorType());
  if (!total_bytes.IsValid())
    return true;

  if (!options.should_scale_input)
    return false;
  total_bytes = options.resize_width;
  total_bytes *= options.resize_height;
  total_bytes *=
      SkColorTypeBytesPerPixel(options.color_params.GetSkColorType());
  if (!total_bytes.IsValid())
    return true;

  return false;
}

SkImageInfo GetSkImageInfo(sk_sp<SkImage> skia_image) {
  SkColorType color_type = kN32_SkColorType;
  if (skia_image->colorSpace() && skia_image->colorSpace()->gammaIsLinear())
    color_type = kRGBA_F16_SkColorType;
  return SkImageInfo::Make(skia_image->width(), skia_image->height(),
                           color_type, skia_image->alphaType(),
                           skia_image->refColorSpace());
}

SkImageInfo GetSkImageInfo(const scoped_refptr<StaticBitmapImage>& image) {
  return GetSkImageInfo(image->PaintImageForCurrentFrame().GetSkImage());
}

// This function results in a readback due to using SkImage::readPixels().
scoped_refptr<Uint8Array> CopyImageData(
    const scoped_refptr<StaticBitmapImage>& input,
    const SkImageInfo& info) {
  if (info.isEmpty())
    return nullptr;
  sk_sp<SkImage> sk_image = input->PaintImageForCurrentFrame().GetSkImage();
  if (sk_image->bounds().isEmpty())
    return nullptr;
  unsigned width = static_cast<unsigned>(input->width());
  scoped_refptr<ArrayBuffer> dst_buffer =
      ArrayBuffer::CreateOrNull(width * input->height(), info.bytesPerPixel());
  if (!dst_buffer)
    return nullptr;
  unsigned byte_length = dst_buffer->ByteLength();
  scoped_refptr<Uint8Array> dst_pixels =
      Uint8Array::Create(std::move(dst_buffer), 0, byte_length);
  if (!dst_pixels)
    return nullptr;
  bool read_pixels_successful = sk_image->readPixels(
      info, dst_pixels->Data(), width * info.bytesPerPixel(), 0, 0);
  DCHECK(read_pixels_successful);
  if (!read_pixels_successful)
    return nullptr;
  return dst_pixels;
}

scoped_refptr<Uint8Array> CopyImageData(
    const scoped_refptr<StaticBitmapImage>& input) {
  SkImageInfo info = GetSkImageInfo(input);
  return CopyImageData(std::move(input), info);
}

void freePixels(const void*, void* pixels) {
  static_cast<Uint8Array*>(pixels)->Release();
}

scoped_refptr<StaticBitmapImage> NewImageFromRaster(
    const SkImageInfo& info,
    scoped_refptr<Uint8Array>&& image_pixels) {
  SkPixmap pixmap(info, image_pixels->Data(), info.minRowBytes());

  Uint8Array* pixels = image_pixels.get();
  if (pixels) {
    pixels->AddRef();
    image_pixels = nullptr;
  }

  return StaticBitmapImage::Create(
      SkImage::MakeFromRaster(pixmap, freePixels, pixels));
}

static inline bool ShouldAvoidPremul(
    const ImageBitmap::ParsedOptions& options) {
  return options.source_is_unpremul && !options.premultiply_alpha;
}

scoped_refptr<StaticBitmapImage> FlipImageVertically(
    scoped_refptr<StaticBitmapImage> input,
    const ImageBitmap::ParsedOptions& parsed_options) {
  sk_sp<SkImage> image = input->PaintImageForCurrentFrame().GetSkImage();

  if (ShouldAvoidPremul(parsed_options)) {
    // Unpremul code path may result in a GPU readback if |input| is texture
    // backed since CopyImageData() uses  SkImage::readPixels() to extract the
    // pixels from SkImage.
    scoped_refptr<Uint8Array> image_pixels = CopyImageData(input);
    if (!image_pixels)
      return nullptr;
    SkImageInfo info = GetSkImageInfo(input);
    unsigned image_row_bytes = info.width() * info.bytesPerPixel();
    for (int i = 0; i < info.height() / 2; i++) {
      unsigned top_first_element = i * image_row_bytes;
      unsigned top_last_element = (i + 1) * image_row_bytes;
      unsigned bottom_first_element = (info.height() - 1 - i) * image_row_bytes;
      std::swap_ranges(image_pixels->Data() + top_first_element,
                       image_pixels->Data() + top_last_element,
                       image_pixels->Data() + bottom_first_element);
    }
    return NewImageFromRaster(info, std::move(image_pixels));
  }

  // Since we are allowed to premul the input image if needed, we can use Skia
  // to flip the image by drawing it on a surface.
  sk_sp<SkSurface> surface = SkSurface::MakeRaster(GetSkImageInfo(input));
  if (!surface)
    return nullptr;
  SkCanvas* canvas = surface->getCanvas();
  canvas->scale(1, -1);
  canvas->translate(0, -input->height());
  canvas->drawImage(image.get(), 0, 0);
  return StaticBitmapImage::Create(surface->makeImageSnapshot(),
                                   input->ContextProviderWrapper());
}

scoped_refptr<StaticBitmapImage> GetImageWithAlphaDisposition(
    scoped_refptr<StaticBitmapImage>&& image,
    AlphaDisposition alpha_disposition) {
  SkAlphaType alpha_type = (alpha_disposition == kPremultiplyAlpha)
                               ? kPremul_SkAlphaType
                               : kUnpremul_SkAlphaType;
  sk_sp<SkImage> skia_image = image->PaintImageForCurrentFrame().GetSkImage();
  if (skia_image->alphaType() == alpha_type)
    return image;

  SkImageInfo info = GetSkImageInfo(image.get());
  info = info.makeAlphaType(alpha_type);

  // For premul to unpremul, we have to readback the pixels.
  if (alpha_type == kUnpremul_SkAlphaType ||
      (image->width() * image->height() < 16384)) {
    // Set the color space of the ImageInfo to nullptr to unpremul in gamma
    // encoded space
    scoped_refptr<Uint8Array> dst_pixels =
        CopyImageData(image, info.makeColorSpace(nullptr));
    if (!dst_pixels)
      return nullptr;
    return NewImageFromRaster(info, std::move(dst_pixels));
  }

  // For unpremul to premul, we can either readback the pixels or draw onto a
  // surface. As shown  in
  // https://fiddle.skia.org/c/1ec3c61ed08f7863d43b9f49ab120a0a, drawing on a
  // surface and getting a snapshot is slower if the image is small. Therefore,
  // for small images (< 128x128 pixels), we still do read back.

  // Draw on a surface. Avoid sRGB gamma transfer curve.
  if (SkColorSpace::Equals(info.colorSpace(), SkColorSpace::MakeSRGB().get()))
    info = info.makeColorSpace(nullptr);
  sk_sp<SkSurface> surface = SkSurface::MakeRaster(info);
  if (!surface)
    return nullptr;
  SkPaint paint;
  paint.setBlendMode(SkBlendMode::kSrc);
  surface->getCanvas()->drawImage(skia_image.get(), 0, 0, &paint);
  return StaticBitmapImage::Create(surface->makeImageSnapshot(),
                                   image->ContextProviderWrapper());
}

// Resizes an SkImage using scalePixels(). This code path should not be used if
// source image is not premul and premul is not allowed and the requested filter
// quality is high.
sk_sp<SkImage> ScaleSkImage(sk_sp<SkImage> image,
                            const ImageBitmap::ParsedOptions& parsed_options,
                            AlphaDisposition alpha_disposition) {
  auto image_info = GetSkImageInfo(image);
  DCHECK(image_info.alphaType() == kPremul_SkAlphaType ||
         !ShouldAvoidPremul(parsed_options) ||
         parsed_options.resize_quality != kHigh_SkFilterQuality);
  // Avoid sRGB transfer function by setting the color space to nullptr.
  if (SkColorSpace::Equals(image_info.colorSpace(),
                           SkColorSpace::MakeSRGB().get()))
    image_info = image_info.makeColorSpace(nullptr);

  // Premul if needed
  if (alpha_disposition == kPremultiplyAlpha &&
      image_info.alphaType() == kUnpremul_SkAlphaType) {
    image_info = image_info.makeAlphaType(kPremul_SkAlphaType);
    sk_sp<SkSurface> surface = SkSurface::MakeRaster(image_info);
    if (!surface)
      return nullptr;
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);
    surface->getCanvas()->drawImage(image.get(), 0, 0, &paint);
    image = surface->makeImageSnapshot();
  }

  SkImageInfo resized_info = image_info.makeWH(parsed_options.resize_width,
                                               parsed_options.resize_height);
  scoped_refptr<ArrayBuffer> resized_buffer =
      ArrayBuffer::CreateOrNull(resized_info.computeMinByteSize(), 1);
  if (!resized_buffer)
    return nullptr;
  scoped_refptr<Uint8Array> resized_pixels = Uint8Array::Create(
      std::move(resized_buffer), 0, resized_info.computeMinByteSize());
  if (!resized_pixels)
    return nullptr;
  SkPixmap resized_pixmap(resized_info, resized_pixels->Data(),
                          resized_info.minRowBytes());
  image->scalePixels(resized_pixmap, parsed_options.resize_quality);
  // Tag the resized Pixmap with the correct color space.
  resized_pixmap.setColorSpace(GetSkImageInfo(image).refColorSpace());

  Uint8Array* pixels = resized_pixels.get();
  if (pixels) {
    pixels->AddRef();
    resized_pixels = nullptr;
  }
  return SkImage::MakeFromRaster(resized_pixmap, freePixels, pixels);
}

scoped_refptr<StaticBitmapImage> ScaleImage(
    scoped_refptr<StaticBitmapImage>&& image,
    const ImageBitmap::ParsedOptions& parsed_options) {
  // Use ScaleSkImage() to resize the image unless the image is unpremul and
  // premul code path is not allowed and the filter quality is high.
  auto image_info = GetSkImageInfo(image);
  if (image_info.alphaType() == kPremul_SkAlphaType ||
      !ShouldAvoidPremul(parsed_options) ||
      parsed_options.resize_quality != kHigh_SkFilterQuality) {
    auto sk_image = image->PaintImageForCurrentFrame().GetSkImage();
    AlphaDisposition alpha_disposition = kDontPremultiplyAlpha;
    if (image_info.alphaType() == kUnpremul_SkAlphaType &&
        !ShouldAvoidPremul(parsed_options))
      alpha_disposition = kPremultiplyAlpha;
    auto resized_sk_image =
        ScaleSkImage(sk_image, parsed_options, alpha_disposition);
    return StaticBitmapImage::Create(resized_sk_image,
                                     image->ContextProviderWrapper());
  }

  // If source image is unpremul, premul code path is not allowed, and the
  // filter quality is high, we cannot use SkImage::scalePixels(), and thus
  // ScaleSkImage(), as Skia clamps color channels to alpha in this case.
  // Instead, we scale color channels and alpha channel separately: RGBA ->
  // RGB/255 and A/255,255,255, scale, merge.
  SkScalar set_alpha_255[] = {1, 0, 0, 0, 0,     // copy red channel
                              0, 1, 0, 0, 0,     // copy green channel
                              0, 0, 1, 0, 0,     // copy blue channel
                              0, 0, 0, 0, 255};  // set alpha to 255
  auto color_filter_set_alpha_255 =
      SkColorFilter::MakeMatrixFilterRowMajor255(set_alpha_255);
  auto image_filter_set_alpha_255 = SkColorFilterImageFilter::Make(
      std::move(color_filter_set_alpha_255), nullptr, nullptr);

  SkScalar copy_alpha_to_red[] = {0, 0, 0, 1, 0,     // copy alpha to red
                                  0, 0, 0, 0, 0,     // set green to zero
                                  0, 0, 0, 0, 0,     // set blue to zero
                                  0, 0, 0, 0, 255};  // set alpha to 255
  auto color_filter_copy_alpha_to_red =
      SkColorFilter::MakeMatrixFilterRowMajor255(copy_alpha_to_red);
  auto image_filter_copy_alpha_to_red = SkColorFilterImageFilter::Make(
      std::move(color_filter_copy_alpha_to_red), nullptr, nullptr);

  // separate RGBA to RGB/255 and A,0,0/255
  SkIRect subset;
  SkIPoint offset;
  auto sk_image = image->PaintImageForCurrentFrame().GetSkImage();
  auto rgb_image = sk_image->makeWithFilter(
      image_filter_set_alpha_255.get(), sk_image->bounds(), sk_image->bounds(),
      &subset, &offset);
  auto alpha_image = sk_image->makeWithFilter(
      image_filter_copy_alpha_to_red.get(), sk_image->bounds(),
      sk_image->bounds(), &subset, &offset);

  // resize
  auto resized_rgb_image =
      ScaleSkImage(rgb_image, parsed_options, kDontPremultiplyAlpha);
  auto resized_alpha_image =
      ScaleSkImage(alpha_image, parsed_options, kDontPremultiplyAlpha);

  // Merge two resized rgb and alpha SkImages together.
  // A better solution would be using SkImageFilter and SkBlendMode to merge
  // the images: convert RGB/255 to RGB/0, convert A,0,0/255 to 0,0,0/A, merge
  // using kSrc and kLighten blend modes. Unfortunately, this doesn't work as
  // SkImageFilter clamps color channels to zero when setting alpha to zero.
  // Therefore, we use a pixmap here.
  scoped_refptr<Uint8Array> rgb_data =
      CopyImageData(StaticBitmapImage::Create(resized_rgb_image));
  scoped_refptr<Uint8Array> alpha_data =
      CopyImageData(StaticBitmapImage::Create(resized_alpha_image));
  SkImageInfo resized_image_info = GetSkImageInfo(resized_rgb_image);
  if (resized_image_info.colorType() == kRGBA_F16_SkColorType) {
    uint16_t* rgb_data_iter = static_cast<uint16_t*>((void*)(rgb_data->Data()));
    uint16_t* alpha_data_iter =
        static_cast<uint16_t*>((void*)(alpha_data->Data()));
    for (int i = 0;
         i < resized_image_info.width() * resized_image_info.height(); i++)
      *(rgb_data_iter + i * 4 + 3) = *(alpha_data_iter + i * 4);
  } else {
    uint8_t* rgb_data_iter = static_cast<uint8_t*>(rgb_data->Data());
    uint8_t* alpha_data_iter = static_cast<uint8_t*>(alpha_data->Data());
    int red_channel_locator =
        (kN32_SkColorType == kRGBA_8888_SkColorType) ? 0 : 2;
    for (int i = 0;
         i < resized_image_info.width() * resized_image_info.height(); i++) {
      *(rgb_data_iter + i * 4 + 3) =
          *(alpha_data_iter + i * 4 + red_channel_locator);
    }
  }

  SkImageInfo resized_unpremul_info =
      resized_image_info.makeAlphaType(kUnpremul_SkAlphaType);
  SkPixmap pixmap(resized_unpremul_info, rgb_data->Data(),
                  resized_unpremul_info.minRowBytes());
  Uint8Array* pixels = rgb_data.get();
  if (pixels) {
    pixels->AddRef();
    rgb_data = nullptr;
  }
  return StaticBitmapImage::Create(
      SkImage::MakeFromRaster(pixmap, freePixels, pixels));
}

scoped_refptr<StaticBitmapImage> ApplyColorSpaceConversion(
    scoped_refptr<StaticBitmapImage>&& image,
    ImageBitmap::ParsedOptions& options) {
  SkTransferFunctionBehavior transfer_function_behavior =
      SkTransferFunctionBehavior::kRespect;
  // We normally expect to respect transfer function. However, in two scenarios
  // we have to ignore the transfer function. First, when the source image is
  // unpremul. Second, when the source image is drawn using a
  // SkColorSpaceXformCanvas.
  sk_sp<SkImage> skia_image = image->PaintImageForCurrentFrame().GetSkImage();
  if (!skia_image->colorSpace() ||
      skia_image->alphaType() == kUnpremul_SkAlphaType)
    transfer_function_behavior = SkTransferFunctionBehavior::kIgnore;

  return image->ConvertToColorSpace(
      options.color_params.GetSkColorSpaceForSkSurfaces(),
      transfer_function_behavior);
}

scoped_refptr<StaticBitmapImage> MakeBlankImage(
    const ImageBitmap::ParsedOptions& parsed_options) {
  SkImageInfo info = SkImageInfo::Make(
      parsed_options.crop_rect.Width(), parsed_options.crop_rect.Height(),
      parsed_options.color_params.GetSkColorType(), kPremul_SkAlphaType,
      parsed_options.color_params.GetSkColorSpaceForSkSurfaces());
  if (parsed_options.should_scale_input) {
    info =
        info.makeWH(parsed_options.resize_width, parsed_options.resize_height);
  }
  sk_sp<SkSurface> surface = SkSurface::MakeRaster(info);
  if (!surface)
    return nullptr;
  return StaticBitmapImage::Create(surface->makeImageSnapshot());
}

}  // namespace

sk_sp<SkImage> ImageBitmap::GetSkImageFromDecoder(
    std::unique_ptr<ImageDecoder> decoder) {
  if (!decoder->FrameCount())
    return nullptr;
  ImageFrame* frame = decoder->DecodeFrameBufferAtIndex(0);
  if (!frame || frame->GetStatus() != ImageFrame::kFrameComplete)
    return nullptr;
  DCHECK(!frame->Bitmap().isNull() && !frame->Bitmap().empty());
  return frame->FinalizePixelsAndGetImage();
}

static scoped_refptr<StaticBitmapImage> CropImageAndApplyColorSpaceConversion(
    scoped_refptr<Image>&& image,
    ImageBitmap::ParsedOptions& parsed_options) {
  DCHECK(image);
  IntRect img_rect(IntPoint(), IntSize(image->width(), image->height()));
  const IntRect src_rect = Intersection(img_rect, parsed_options.crop_rect);

  // If cropRect doesn't intersect the source image, return a transparent black
  // image.
  if (src_rect.IsEmpty())
    return MakeBlankImage(parsed_options);

  sk_sp<SkImage> skia_image = image->PaintImageForCurrentFrame().GetSkImage();
  // Attempt to get raw unpremultiplied image data, executed only when
  // skia_image is premultiplied.
  if (!skia_image->isOpaque() && image->Data() &&
      skia_image->alphaType() == kPremul_SkAlphaType) {
    std::unique_ptr<ImageDecoder> decoder(ImageDecoder::Create(
        image->Data(), true,
        parsed_options.premultiply_alpha ? ImageDecoder::kAlphaPremultiplied
                                         : ImageDecoder::kAlphaNotPremultiplied,
        parsed_options.has_color_space_conversion ? ColorBehavior::Tag()
                                                  : ColorBehavior::Ignore()));
    if (!decoder)
      return nullptr;
    skia_image = ImageBitmap::GetSkImageFromDecoder(std::move(decoder));
    if (!skia_image)
      return nullptr;

    // In the case where the source image is lazy-decoded, image_ may not be in
    // a decoded state, we trigger it here.
    SkPixmap pixmap;
    if (!skia_image->isTextureBacked() && !skia_image->peekPixels(&pixmap)) {
      sk_sp<SkSurface> surface =
          SkSurface::MakeRaster(GetSkImageInfo(StaticBitmapImage::Create(
              skia_image, image->ContextProviderWrapper())));
      surface->getCanvas()->drawImage(skia_image.get(), 0, 0);
      skia_image = surface->makeImageSnapshot();
    }
  }

  if (src_rect != img_rect)
    skia_image = skia_image->makeSubset(src_rect);

  scoped_refptr<StaticBitmapImage> result =
      StaticBitmapImage::Create(skia_image, image->ContextProviderWrapper());

  // down-scaling has higher priority than other tasks, up-scaling has lower.
  bool down_scaling =
      parsed_options.should_scale_input &&
      (parsed_options.resize_width * parsed_options.resize_height <
       result->Size().Area());
  bool up_scaling = parsed_options.should_scale_input && !down_scaling;

  // resize if down-scaling
  if (down_scaling) {
    result = ScaleImage(std::move(result), parsed_options);
    if (!result)
      return nullptr;
  }

  // flip if needed
  if (parsed_options.flip_y) {
    result = FlipImageVertically(std::move(result), parsed_options);
    if (!result)
      return nullptr;
  }

  // color convert if needed
  if (parsed_options.has_color_space_conversion) {
    result = ApplyColorSpaceConversion(std::move(result), parsed_options);
    if (!result)
      return nullptr;
  }

  // resize if up-scaling
  if (up_scaling) {
    result = ScaleImage(std::move(result), parsed_options);
    if (!result)
      return nullptr;
  }

  // premultiply / unpremultiply if needed
  result = GetImageWithAlphaDisposition(std::move(result),
                                        parsed_options.premultiply_alpha
                                            ? kPremultiplyAlpha
                                            : kDontPremultiplyAlpha);

  return result;
}

ImageBitmap::ImageBitmap(ImageElementBase* image,
                         Optional<IntRect> crop_rect,
                         Document* document,
                         const ImageBitmapOptions& options) {
  scoped_refptr<Image> input = image->CachedImage()->GetImage();
  ParsedOptions parsed_options =
      ParseOptions(options, crop_rect, image->BitmapSourceSize());
  parsed_options.source_is_unpremul =
      (input->PaintImageForCurrentFrame().GetSkImage()->alphaType() ==
       kUnpremul_SkAlphaType);
  if (DstBufferSizeHasOverflow(parsed_options))
    return;

  image_ =
      CropImageAndApplyColorSpaceConversion(std::move(input), parsed_options);
  if (!image_)
    return;

  image_->SetOriginClean(
      !image->WouldTaintOrigin(document->GetSecurityOrigin()));
}

ImageBitmap::ImageBitmap(HTMLVideoElement* video,
                         Optional<IntRect> crop_rect,
                         Document* document,
                         const ImageBitmapOptions& options) {
  ParsedOptions parsed_options =
      ParseOptions(options, crop_rect, video->BitmapSourceSize());
  if (DstBufferSizeHasOverflow(parsed_options))
    return;

  std::unique_ptr<ImageBuffer> buffer =
      ImageBuffer::Create(IntSize(video->videoWidth(), video->videoHeight()),
                          kDoNotInitializeImagePixels);
  if (!buffer)
    return;

  video->PaintCurrentFrame(
      buffer->Canvas(),
      IntRect(IntPoint(), IntSize(video->videoWidth(), video->videoHeight())),
      nullptr);
  scoped_refptr<StaticBitmapImage> input = buffer->NewImageSnapshot();
  image_ = CropImageAndApplyColorSpaceConversion(input, parsed_options);
  if (!image_)
    return;

  image_->SetOriginClean(
      !video->WouldTaintOrigin(document->GetSecurityOrigin()));
}

ImageBitmap::ImageBitmap(HTMLCanvasElement* canvas,
                         Optional<IntRect> crop_rect,
                         const ImageBitmapOptions& options) {
  SourceImageStatus status;
  scoped_refptr<Image> image_input = canvas->GetSourceImageForCanvas(
      &status, kPreferAcceleration, kSnapshotReasonCreateImageBitmap,
      FloatSize());
  if (status != kNormalSourceImageStatus)
    return;
  DCHECK(image_input->IsStaticBitmapImage());
  scoped_refptr<StaticBitmapImage> input =
      static_cast<StaticBitmapImage*>(image_input.get());

  ParsedOptions parsed_options = ParseOptions(
      options, crop_rect, IntSize(input->width(), input->height()));
  if (DstBufferSizeHasOverflow(parsed_options))
    return;

  image_ =
      CropImageAndApplyColorSpaceConversion(std::move(input), parsed_options);
  if (!image_)
    return;

  image_->SetOriginClean(canvas->OriginClean());
}

ImageBitmap::ImageBitmap(OffscreenCanvas* offscreen_canvas,
                         Optional<IntRect> crop_rect,
                         const ImageBitmapOptions& options) {
  SourceImageStatus status;
  scoped_refptr<Image> raw_input = offscreen_canvas->GetSourceImageForCanvas(
      &status, kPreferNoAcceleration, kSnapshotReasonCreateImageBitmap,
      FloatSize(offscreen_canvas->Size()));
  DCHECK(raw_input->IsStaticBitmapImage());
  scoped_refptr<StaticBitmapImage> input =
      static_cast<StaticBitmapImage*>(raw_input.get());
  raw_input = nullptr;

  if (status != kNormalSourceImageStatus)
    return;

  ParsedOptions parsed_options = ParseOptions(
      options, crop_rect, IntSize(input->width(), input->height()));
  if (DstBufferSizeHasOverflow(parsed_options))
    return;

  image_ =
      CropImageAndApplyColorSpaceConversion(std::move(input), parsed_options);
  if (!image_)
    return;
  image_->SetOriginClean(offscreen_canvas->OriginClean());
}

ImageBitmap::ImageBitmap(const void* pixel_data,
                         uint32_t width,
                         uint32_t height,
                         bool is_image_bitmap_premultiplied,
                         bool is_image_bitmap_origin_clean,
                         const CanvasColorParams& color_params) {
  SkImageInfo info =
      SkImageInfo::Make(width, height, color_params.GetSkColorType(),
                        is_image_bitmap_premultiplied ? kPremul_SkAlphaType
                                                      : kUnpremul_SkAlphaType,
                        color_params.GetSkColorSpaceForSkSurfaces());
  SkPixmap pixmap(info, pixel_data, info.bytesPerPixel() * width);
  image_ = StaticBitmapImage::Create(SkImage::MakeRasterCopy(pixmap));
  if (!image_)
    return;
  image_->SetOriginClean(is_image_bitmap_origin_clean);
}

ImageBitmap::ImageBitmap(ImageData* data,
                         Optional<IntRect> crop_rect,
                         const ImageBitmapOptions& options) {
  ParsedOptions parsed_options =
      ParseOptions(options, crop_rect, data->BitmapSourceSize());
  // ImageData is always unpremul.
  parsed_options.source_is_unpremul = true;
  if (DstBufferSizeHasOverflow(parsed_options))
    return;

  IntRect data_src_rect = IntRect(IntPoint(), data->Size());
  IntRect src_rect = crop_rect
                         ? Intersection(parsed_options.crop_rect, data_src_rect)
                         : data_src_rect;

  // If cropRect doesn't intersect the source image, return a transparent black
  // image.
  if (src_rect.IsEmpty()) {
    image_ = MakeBlankImage(parsed_options);
    return;
  }

  // Copy / color convert the pixels
  scoped_refptr<ArrayBuffer> pixels_buffer = ArrayBuffer::CreateOrNull(
      src_rect.Size().Area(), parsed_options.color_params.BytesPerPixel());
  if (!pixels_buffer)
    return;
  unsigned byte_length = pixels_buffer->ByteLength();
  scoped_refptr<Uint8Array> image_pixels =
      Uint8Array::Create(std::move(pixels_buffer), 0, byte_length);
  if (!image_pixels)
    return;
  if (!data->ImageDataInCanvasColorSettings(
          parsed_options.color_params.ColorSpace(),
          parsed_options.color_params.PixelFormat(), image_pixels->Data(),
          kN32ColorType, &src_rect))
    return;

  // Create Image object
  SkImageInfo info = SkImageInfo::Make(
      src_rect.Width(), src_rect.Height(),
      parsed_options.color_params.GetSkColorType(), kUnpremul_SkAlphaType,
      parsed_options.color_params.GetSkColorSpaceForSkSurfaces());
  image_ = NewImageFromRaster(info, std::move(image_pixels));
  if (!image_)
    return;

  // down-scaling has higher priority than other tasks, up-scaling has lower.
  bool down_scaling =
      parsed_options.should_scale_input &&
      (parsed_options.resize_width * parsed_options.resize_height <
       image_->Size().Area());
  bool up_scaling = parsed_options.should_scale_input && !down_scaling;

  // resize if down-scaling
  if (down_scaling)
    image_ = ScaleImage(std::move(image_), parsed_options);
  if (!image_)
    return;

  // premultiply if needed
  if (parsed_options.premultiply_alpha)
    image_ = GetImageWithAlphaDisposition(std::move(image_), kPremultiplyAlpha);
  if (!image_)
    return;

  // flip if needed
  if (parsed_options.flip_y)
    image_ = FlipImageVertically(std::move(image_), parsed_options);
  if (!image_)
    return;

  // resize if up-scaling
  if (up_scaling)
    image_ = ScaleImage(std::move(image_), parsed_options);
}

ImageBitmap::ImageBitmap(ImageBitmap* bitmap,
                         Optional<IntRect> crop_rect,
                         const ImageBitmapOptions& options) {
  scoped_refptr<StaticBitmapImage> input = bitmap->BitmapImage();
  if (!input)
    return;
  ParsedOptions parsed_options =
      ParseOptions(options, crop_rect, input->Size());
  parsed_options.source_is_unpremul =
      (input->PaintImageForCurrentFrame().GetSkImage()->alphaType() ==
       kUnpremul_SkAlphaType);
  if (DstBufferSizeHasOverflow(parsed_options))
    return;

  image_ =
      CropImageAndApplyColorSpaceConversion(std::move(input), parsed_options);
  if (!image_)
    return;

  image_->SetOriginClean(bitmap->OriginClean());
}

ImageBitmap::ImageBitmap(scoped_refptr<StaticBitmapImage> image,
                         Optional<IntRect> crop_rect,
                         const ImageBitmapOptions& options) {
  bool origin_clean = image->OriginClean();
  ParsedOptions parsed_options =
      ParseOptions(options, crop_rect, image->Size());
  parsed_options.source_is_unpremul =
      (image->PaintImageForCurrentFrame().GetSkImage()->alphaType() ==
       kUnpremul_SkAlphaType);
  if (DstBufferSizeHasOverflow(parsed_options))
    return;

  image_ =
      CropImageAndApplyColorSpaceConversion(std::move(image), parsed_options);
  if (!image_)
    return;

  image_->SetOriginClean(origin_clean);
}

ImageBitmap::ImageBitmap(scoped_refptr<StaticBitmapImage> image) {
  image_ = std::move(image);
}

scoped_refptr<StaticBitmapImage> ImageBitmap::Transfer() {
  DCHECK(!IsNeutered());
  is_neutered_ = true;
  image_->Transfer();
  return std::move(image_);
}

ImageBitmap::~ImageBitmap() {}

ImageBitmap* ImageBitmap::Create(ImageElementBase* image,
                                 Optional<IntRect> crop_rect,
                                 Document* document,
                                 const ImageBitmapOptions& options) {
  return new ImageBitmap(image, crop_rect, document, options);
}

ImageBitmap* ImageBitmap::Create(HTMLVideoElement* video,
                                 Optional<IntRect> crop_rect,
                                 Document* document,
                                 const ImageBitmapOptions& options) {
  return new ImageBitmap(video, crop_rect, document, options);
}

ImageBitmap* ImageBitmap::Create(HTMLCanvasElement* canvas,
                                 Optional<IntRect> crop_rect,
                                 const ImageBitmapOptions& options) {
  return new ImageBitmap(canvas, crop_rect, options);
}

ImageBitmap* ImageBitmap::Create(OffscreenCanvas* offscreen_canvas,
                                 Optional<IntRect> crop_rect,
                                 const ImageBitmapOptions& options) {
  return new ImageBitmap(offscreen_canvas, crop_rect, options);
}

ImageBitmap* ImageBitmap::Create(ImageData* data,
                                 Optional<IntRect> crop_rect,
                                 const ImageBitmapOptions& options) {
  return new ImageBitmap(data, crop_rect, options);
}

ImageBitmap* ImageBitmap::Create(ImageBitmap* bitmap,
                                 Optional<IntRect> crop_rect,
                                 const ImageBitmapOptions& options) {
  return new ImageBitmap(bitmap, crop_rect, options);
}

ImageBitmap* ImageBitmap::Create(scoped_refptr<StaticBitmapImage> image,
                                 Optional<IntRect> crop_rect,
                                 const ImageBitmapOptions& options) {
  return new ImageBitmap(std::move(image), crop_rect, options);
}

ImageBitmap* ImageBitmap::Create(scoped_refptr<StaticBitmapImage> image) {
  return new ImageBitmap(std::move(image));
}

ImageBitmap* ImageBitmap::Create(const void* pixel_data,
                                 uint32_t width,
                                 uint32_t height,
                                 bool is_image_bitmap_premultiplied,
                                 bool is_image_bitmap_origin_clean,
                                 const CanvasColorParams& color_params) {
  return new ImageBitmap(pixel_data, width, height,
                         is_image_bitmap_premultiplied,
                         is_image_bitmap_origin_clean, color_params);
}

void ImageBitmap::ResolvePromiseOnOriginalThread(
    ScriptPromiseResolver* resolver,
    sk_sp<SkImage> skia_image,
    bool origin_clean,
    std::unique_ptr<ParsedOptions> parsed_options) {
  if (!skia_image) {
    resolver->Reject(
        ScriptValue(resolver->GetScriptState(),
                    v8::Null(resolver->GetScriptState()->GetIsolate())));
    return;
  }
  scoped_refptr<StaticBitmapImage> image =
      StaticBitmapImage::Create(std::move(skia_image));
  DCHECK(IsMainThread());
  if (!parsed_options->premultiply_alpha) {
    image =
        GetImageWithAlphaDisposition(std::move(image), kDontPremultiplyAlpha);
  }
  if (!image) {
    resolver->Reject(
        ScriptValue(resolver->GetScriptState(),
                    v8::Null(resolver->GetScriptState()->GetIsolate())));
    return;
  }
  image = ApplyColorSpaceConversion(std::move(image), *(parsed_options.get()));
  if (!image) {
    resolver->Reject(
        ScriptValue(resolver->GetScriptState(),
                    v8::Null(resolver->GetScriptState()->GetIsolate())));
    return;
  }
  ImageBitmap* bitmap = new ImageBitmap(image);
  if (bitmap && bitmap->BitmapImage())
    bitmap->BitmapImage()->SetOriginClean(origin_clean);
  if (bitmap && bitmap->BitmapImage()) {
    resolver->Resolve(bitmap);
  } else {
    resolver->Reject(
        ScriptValue(resolver->GetScriptState(),
                    v8::Null(resolver->GetScriptState()->GetIsolate())));
  }
}

void ImageBitmap::RasterizeImageOnBackgroundThread(
    ScriptPromiseResolver* resolver,
    sk_sp<PaintRecord> paint_record,
    const IntRect& dst_rect,
    bool origin_clean,
    std::unique_ptr<ParsedOptions> parsed_options) {
  DCHECK(!IsMainThread());
  // TODO (zakerinasab): crbug.com/768844
  // For now only SVG is decoded async so it is fine to assume the color space
  // is SRGB. When other sources are decoded async (crbug.com/580202), make sure
  // that proper color space is used in SkImageInfo to avoid clipping the gamut
  // of the image bitmap source.
  SkImageInfo info = SkImageInfo::MakeS32(dst_rect.Width(), dst_rect.Height(),
                                          kPremul_SkAlphaType);
  sk_sp<SkSurface> surface = SkSurface::MakeRaster(info);
  sk_sp<SkImage> skia_image;
  if (surface) {
    paint_record->Playback(surface->getCanvas());
    skia_image = surface->makeImageSnapshot();
  }
  scoped_refptr<WebTaskRunner> task_runner =
      Platform::Current()->MainThread()->GetWebTaskRunner();
  task_runner->PostTask(
      BLINK_FROM_HERE, CrossThreadBind(&ResolvePromiseOnOriginalThread,
                                       WrapCrossThreadPersistent(resolver),
                                       std::move(skia_image), origin_clean,
                                       WTF::Passed(std::move(parsed_options))));
}

ScriptPromise ImageBitmap::CreateAsync(ImageElementBase* image,
                                       Optional<IntRect> crop_rect,
                                       Document* document,
                                       ScriptState* script_state,
                                       const ImageBitmapOptions& options) {
  ScriptPromiseResolver* resolver = ScriptPromiseResolver::Create(script_state);
  ScriptPromise promise = resolver->Promise();

  scoped_refptr<Image> input = image->CachedImage()->GetImage();
  ParsedOptions parsed_options =
      ParseOptions(options, crop_rect, image->BitmapSourceSize());
  if (DstBufferSizeHasOverflow(parsed_options)) {
    resolver->Reject(
        ScriptValue(resolver->GetScriptState(),
                    v8::Null(resolver->GetScriptState()->GetIsolate())));
    return promise;
  }

  IntRect input_rect(IntPoint(), input->Size());
  const IntRect src_rect = Intersection(input_rect, parsed_options.crop_rect);

  // In the case when |crop_rect| doesn't intersect the source image, we return
  // a transparent black image, respecting the color_params but ignoring
  // poremultiply_alpha.
  if (src_rect.IsEmpty()) {
    ImageBitmap* bitmap = new ImageBitmap(MakeBlankImage(parsed_options));
    if (bitmap && bitmap->BitmapImage()) {
      bitmap->BitmapImage()->SetOriginClean(
          !image->WouldTaintOrigin(document->GetSecurityOrigin()));
    }
    if (bitmap && bitmap->BitmapImage()) {
      resolver->Resolve(bitmap);
    } else {
      resolver->Reject(
          ScriptValue(resolver->GetScriptState(),
                      v8::Null(resolver->GetScriptState()->GetIsolate())));
    }
    return promise;
  }

  IntRect draw_src_rect(parsed_options.crop_rect);
  IntRect draw_dst_rect(0, 0, parsed_options.resize_width,
                        parsed_options.resize_height);
  sk_sp<PaintRecord> paint_record =
      input->PaintRecordForContainer(NullURL(), input->Size(), draw_src_rect,
                                     draw_dst_rect, parsed_options.flip_y);
  std::unique_ptr<ParsedOptions> passed_parsed_options =
      std::make_unique<ParsedOptions>(parsed_options);
  BackgroundTaskRunner::PostOnBackgroundThread(
      BLINK_FROM_HERE,
      CrossThreadBind(&RasterizeImageOnBackgroundThread,
                      WrapCrossThreadPersistent(resolver),
                      std::move(paint_record), draw_dst_rect,
                      !image->WouldTaintOrigin(document->GetSecurityOrigin()),
                      WTF::Passed(std::move(passed_parsed_options))));
  return promise;
}

void ImageBitmap::close() {
  if (!image_ || is_neutered_)
    return;
  image_ = nullptr;
  is_neutered_ = true;
}

// static
ImageBitmap* ImageBitmap::Take(ScriptPromiseResolver*, sk_sp<SkImage> image) {
  return ImageBitmap::Create(StaticBitmapImage::Create(std::move(image)));
}

CanvasColorParams ImageBitmap::GetCanvasColorParams() {
  return CanvasColorParams(GetSkImageInfo(image_));
}

scoped_refptr<Uint8Array> ImageBitmap::CopyBitmapData(
    AlphaDisposition alpha_op,
    DataU8ColorType u8_color_type) {
  SkImageInfo info = GetSkImageInfo(image_);
  auto color_type = info.colorType();
  if (color_type == kN32_SkColorType && u8_color_type == kRGBAColorType)
    color_type = kRGBA_8888_SkColorType;
  info =
      SkImageInfo::Make(width(), height(), color_type,
                        (alpha_op == kPremultiplyAlpha) ? kPremul_SkAlphaType
                                                        : kUnpremul_SkAlphaType,
                        info.refColorSpace());
  return CopyImageData(image_, info);
}

scoped_refptr<Uint8Array> ImageBitmap::CopyBitmapData() {
  return CopyImageData(image_);
}

unsigned long ImageBitmap::width() const {
  if (!image_)
    return 0;
  DCHECK_GT(image_->width(), 0);
  return image_->width();
}

unsigned long ImageBitmap::height() const {
  if (!image_)
    return 0;
  DCHECK_GT(image_->height(), 0);
  return image_->height();
}

bool ImageBitmap::IsAccelerated() const {
  return image_ && (image_->IsTextureBacked() || image_->HasMailbox());
}

IntSize ImageBitmap::Size() const {
  if (!image_)
    return IntSize();
  DCHECK_GT(image_->width(), 0);
  DCHECK_GT(image_->height(), 0);
  return IntSize(image_->width(), image_->height());
}

ScriptPromise ImageBitmap::CreateImageBitmap(
    ScriptState* script_state,
    EventTarget& event_target,
    Optional<IntRect> crop_rect,
    const ImageBitmapOptions& options) {
  return ImageBitmapSource::FulfillImageBitmap(
      script_state, Create(this, crop_rect, options));
}

scoped_refptr<Image> ImageBitmap::GetSourceImageForCanvas(
    SourceImageStatus* status,
    AccelerationHint,
    SnapshotReason,
    const FloatSize&) {
  *status = kNormalSourceImageStatus;
  if (!image_)
    return nullptr;
  if (image_->IsPremultiplied())
    return image_;
  // Skia does not support drawing unpremul SkImage on SkCanvas.
  // Premultiply and return.
  return GetImageWithAlphaDisposition(std::move(image_), kPremultiplyAlpha);
}

void ImageBitmap::AdjustDrawRects(FloatRect* src_rect,
                                  FloatRect* dst_rect) const {}

FloatSize ImageBitmap::ElementSize(const FloatSize&) const {
  return FloatSize(width(), height());
}

}  // namespace blink
