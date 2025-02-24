// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/libgtkui/nav_button_provider_gtk3.h"

#include <gtk/gtk.h>

#include "chrome/browser/ui/libgtkui/gtk3_background_painter.h"
#include "chrome/browser/ui/libgtkui/gtk_util.h"
#include "ui/base/glib/scoped_gobject.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/image/image_skia_source.h"
#include "ui/views/resources/grit/views_resources.h"
#include "ui/views/widget/widget.h"

namespace libgtkui {

namespace {

// gtkheaderbar.c uses GTK_ICON_SIZE_MENU, which is 16px.
const int kIconSize = 16;

// Specified in GtkHeaderBar spec.
const int kHeaderSpacing = 6;

const char* ButtonStyleClassFromButtonType(
    chrome::FrameButtonDisplayType type) {
  switch (type) {
    case chrome::FrameButtonDisplayType::kMinimize:
      return "minimize";
    case chrome::FrameButtonDisplayType::kMaximize:
    case chrome::FrameButtonDisplayType::kRestore:
      return "maximize";
    case chrome::FrameButtonDisplayType::kClose:
      return "close";
    default:
      NOTREACHED();
      return "";
  }
}

GtkStateFlags GtkStateFlagsFromButtonState(views::Button::ButtonState state) {
  switch (state) {
    case views::Button::STATE_NORMAL:
      return GTK_STATE_FLAG_NORMAL;
    case views::Button::STATE_HOVERED:
      return GTK_STATE_FLAG_PRELIGHT;
    case views::Button::STATE_PRESSED:
      return static_cast<GtkStateFlags>(GTK_STATE_FLAG_PRELIGHT |
                                        GTK_STATE_FLAG_ACTIVE);
    case views::Button::STATE_DISABLED:
      return GTK_STATE_FLAG_INSENSITIVE;
    default:
      NOTREACHED();
      return GTK_STATE_FLAG_NORMAL;
  }
}

const char* IconNameFromButtonType(chrome::FrameButtonDisplayType type) {
  switch (type) {
    case chrome::FrameButtonDisplayType::kMinimize:
      return "window-minimize-symbolic";
    case chrome::FrameButtonDisplayType::kMaximize:
      return "window-maximize-symbolic";
    case chrome::FrameButtonDisplayType::kRestore:
      return "window-restore-symbolic";
    case chrome::FrameButtonDisplayType::kClose:
      return "window-close-symbolic";
    default:
      NOTREACHED();
      return "";
  }
}

gfx::Insets InsetsFromGtkBorder(const GtkBorder& border) {
  return gfx::Insets(border.top, border.left, border.bottom, border.right);
}

gfx::Insets PaddingFromStyleContext(GtkStyleContext* context,
                                    GtkStateFlags state) {
  GtkBorder padding;
  gtk_style_context_get_padding(context, state, &padding);
  return InsetsFromGtkBorder(padding);
}

gfx::Insets BorderFromStyleContext(GtkStyleContext* context,
                                   GtkStateFlags state) {
  GtkBorder border;
  gtk_style_context_get_border(context, state, &border);
  return InsetsFromGtkBorder(border);
}

gfx::Insets MarginFromStyleContext(GtkStyleContext* context,
                                   GtkStateFlags state) {
  GtkBorder margin;
  gtk_style_context_get_margin(context, state, &margin);
  return InsetsFromGtkBorder(margin);
}

ScopedGObject<GdkPixbuf> LoadNavButtonIcon(chrome::FrameButtonDisplayType type,
                                           GtkStyleContext* button_context,
                                           int scale) {
  const char* icon_name = IconNameFromButtonType(type);
  ScopedGObject<GtkIconInfo> icon_info(gtk_icon_theme_lookup_icon_for_scale(
      gtk_icon_theme_get_default(), icon_name, kIconSize, scale,
      static_cast<GtkIconLookupFlags>(GTK_ICON_LOOKUP_USE_BUILTIN |
                                      GTK_ICON_LOOKUP_GENERIC_FALLBACK)));
  return ScopedGObject<GdkPixbuf>(gtk_icon_info_load_symbolic_for_context(
      icon_info, button_context, nullptr, nullptr));
}

gfx::Size GetMinimumWidgetSize(gfx::Size content_size,
                               GtkStyleContext* content_context,
                               GtkStyleContext* widget_context,
                               GtkStateFlags state) {
  gfx::Rect widget_rect = gfx::Rect(content_size);
  if (content_context)
    widget_rect.Inset(-MarginFromStyleContext(content_context, state));
  if (GtkVersionCheck(3, 20)) {
    int min_width, min_height;
    gtk_style_context_get(widget_context, state, "min-width", &min_width,
                          "min-height", &min_height, NULL);
    widget_rect.set_width(std::max(widget_rect.width(), min_width));
    widget_rect.set_height(std::max(widget_rect.height(), min_height));
  }
  widget_rect.Inset(-PaddingFromStyleContext(widget_context, state));
  widget_rect.Inset(-BorderFromStyleContext(widget_context, state));
  return widget_rect.size();
}

void CalculateUnscaledButtonSize(chrome::FrameButtonDisplayType type,
                                 gfx::Size* button_size,
                                 gfx::Insets* button_margin) {
  // views::ImageButton expects the images for each state to be of the
  // same size, but GTK can, in general, use a differnetly-sized
  // button for each state.  For this reason, render buttons for all
  // states at the size of a GTK_STATE_FLAG_NORMAL button.
  auto button_context = GetStyleContextFromCss(
      "GtkHeaderBar#headerbar.header-bar.titlebar "
      "GtkButton#button.titlebutton." +
      std::string(ButtonStyleClassFromButtonType(type)));

  ScopedGObject<GdkPixbuf> icon_pixbuf =
      LoadNavButtonIcon(type, button_context, 1);

  gfx::Size icon_size(gdk_pixbuf_get_width(icon_pixbuf),
                      gdk_pixbuf_get_height(icon_pixbuf));
  auto image_context =
      AppendCssNodeToStyleContext(button_context, "GtkImage#image");
  gfx::Size image_size = GetMinimumWidgetSize(icon_size, nullptr, image_context,
                                              GTK_STATE_FLAG_NORMAL);

  *button_size = GetMinimumWidgetSize(image_size, image_context, button_context,
                                      GTK_STATE_FLAG_NORMAL);
  *button_margin =
      MarginFromStyleContext(button_context, GTK_STATE_FLAG_NORMAL);
}

ScopedStyleContext CreateHeaderContext() {
  return GetStyleContextFromCss("GtkHeaderBar#headerbar.header-bar.titlebar");
}

ScopedStyleContext CreateAvatarButtonContext(GtkStyleContext* header_context) {
  return AppendCssNodeToStyleContext(
      header_context, GtkVersionCheck(3, 20)
                          ? "GtkButton#button.image-button.toggle"
                          : "GtkToggleButton#button.image-button");
}

class NavButtonImageSource : public gfx::ImageSkiaSource {
 public:
  NavButtonImageSource(chrome::FrameButtonDisplayType type,
                       views::Button::ButtonState state,
                       bool active,
                       gfx::Size button_size)
      : type_(type),
        state_(state),
        active_(active),
        button_size_(button_size) {}

  ~NavButtonImageSource() override {}

  gfx::ImageSkiaRep GetImageForScale(float scale) override {
    // gfx::ImageSkia kindly caches the result of this function, so
    // RenderNavButton() is called at most once for each needed scale
    // factor.  Additionally, buttons in the HOVERED or PRESSED states
    // are not actually rendered until they are needed.
    auto button_context = GetStyleContextFromCss(
        "GtkHeaderBar#headerbar.header-bar.titlebar "
        "GtkButton#button.titlebutton");
    gtk_style_context_add_class(button_context,
                                ButtonStyleClassFromButtonType(type_));
    GtkStateFlags button_state = GtkStateFlagsFromButtonState(state_);
    if (!active_) {
      button_state =
          static_cast<GtkStateFlags>(button_state | GTK_STATE_FLAG_BACKDROP);
    }
    gtk_style_context_set_state(button_context, button_state);

    // Gtk doesn't support fractional scale factors, but chrome does.
    // Rendering the button background and border at a fractional
    // scale factor is easy, since we can adjust the cairo context
    // transform.  But the icon is loaded from a pixbuf, so we pick
    // the next-highest integer scale and manually downsize.
    int pixbuf_scale = scale == static_cast<int>(scale) ? scale : scale + 1;
    ScopedGObject<GdkPixbuf> icon_pixbuf =
        LoadNavButtonIcon(type_, button_context, pixbuf_scale);

    gfx::Size icon_size(gdk_pixbuf_get_width(icon_pixbuf),
                        gdk_pixbuf_get_height(icon_pixbuf));

    SkBitmap bitmap;
    bitmap.allocN32Pixels(scale * button_size_.width(),
                          scale * button_size_.height());
    bitmap.eraseColor(0);

    CairoSurface surface(bitmap);
    cairo_t* cr = surface.cairo();

    cairo_save(cr);
    cairo_scale(cr, scale, scale);
    if (GtkVersionCheck(3, 11, 3) ||
        (button_state & (GTK_STATE_FLAG_PRELIGHT | GTK_STATE_FLAG_ACTIVE))) {
      gtk_render_background(button_context, cr, 0, 0, button_size_.width(),
                            button_size_.height());
      gtk_render_frame(button_context, cr, 0, 0, button_size_.width(),
                       button_size_.height());
    }
    cairo_restore(cr);
    cairo_save(cr);
    float pixbuf_extra_scale = scale / pixbuf_scale;
    cairo_scale(cr, pixbuf_extra_scale, pixbuf_extra_scale);
    gtk_render_icon(
        button_context, cr, icon_pixbuf,
        ((pixbuf_scale * button_size_.width() - icon_size.width()) / 2),
        ((pixbuf_scale * button_size_.height() - icon_size.height()) / 2));
    cairo_restore(cr);

    return gfx::ImageSkiaRep(bitmap, scale);
  }

  bool HasRepresentationAtAllScales() const override { return true; }

 private:
  chrome::FrameButtonDisplayType type_;
  views::Button::ButtonState state_;
  bool active_;
  gfx::Size button_size_;
};

}  // namespace

NavButtonProviderGtk3::NavButtonProviderGtk3() {}

NavButtonProviderGtk3::~NavButtonProviderGtk3() {}

void NavButtonProviderGtk3::RedrawImages(int top_area_height,
                                         bool maximized,
                                         bool active) {
  auto header_context = CreateHeaderContext();

  GtkBorder header_padding;
  gtk_style_context_get_padding(header_context, GTK_STATE_FLAG_NORMAL,
                                &header_padding);

  double scale = 1.0f;
  std::map<chrome::FrameButtonDisplayType, gfx::Size> button_sizes;
  std::map<chrome::FrameButtonDisplayType, gfx::Insets> button_margins;
  std::vector<chrome::FrameButtonDisplayType> display_types{
      chrome::FrameButtonDisplayType::kMinimize,
      maximized ? chrome::FrameButtonDisplayType::kRestore
                : chrome::FrameButtonDisplayType::kMaximize,
      chrome::FrameButtonDisplayType::kClose,
  };
  for (auto type : display_types) {
    CalculateUnscaledButtonSize(type, &button_sizes[type],
                                &button_margins[type]);
    int button_unconstrained_height = button_sizes[type].height() +
                                      button_margins[type].top() +
                                      button_margins[type].bottom();

    int needed_height = header_padding.top + button_unconstrained_height +
                        header_padding.bottom;

    if (needed_height > top_area_height)
      scale =
          std::min(scale, static_cast<double>(top_area_height) / needed_height);
  }

  top_area_spacing_ = InsetsFromGtkBorder(header_padding);
  top_area_spacing_ =
      gfx::Insets(std::round(scale * top_area_spacing_.top()),
                  std::round(scale * top_area_spacing_.left()),
                  std::round(scale * top_area_spacing_.bottom()),
                  std::round(scale * top_area_spacing_.right()));

  inter_button_spacing_ = std::round(scale * kHeaderSpacing);

  for (auto type : display_types) {
    double button_height =
        scale * (button_sizes[type].height() + button_margins[type].top() +
                 button_margins[type].bottom());
    double available_height =
        top_area_height - scale * (header_padding.top + header_padding.bottom);
    double scaled_button_offset = (available_height - button_height) / 2;

    gfx::Size size = button_sizes[type];
    size = gfx::Size(std::round(scale * size.width()),
                     std::round(scale * size.height()));
    gfx::Insets margin = button_margins[type];
    margin =
        gfx::Insets(std::round(scale * (header_padding.top + margin.top()) +
                               scaled_button_offset),
                    std::round(scale * margin.left()), 0,
                    std::round(scale * margin.right()));

    button_margins_[type] = margin;

    for (size_t state = 0; state < views::Button::STATE_COUNT; state++) {
      button_images_[type][state] = gfx::ImageSkia(
          std::make_unique<NavButtonImageSource>(
              type, static_cast<views::Button::ButtonState>(state), active,
              size),
          size);
    }
  }
}

gfx::ImageSkia NavButtonProviderGtk3::GetImage(
    chrome::FrameButtonDisplayType type,
    views::Button::ButtonState state) const {
  auto it = button_images_.find(type);
  DCHECK(it != button_images_.end());
  return it->second[state];
}

gfx::Insets NavButtonProviderGtk3::GetNavButtonMargin(
    chrome::FrameButtonDisplayType type) const {
  auto it = button_margins_.find(type);
  DCHECK(it != button_margins_.end());
  return it->second;
}

gfx::Insets NavButtonProviderGtk3::GetTopAreaSpacing() const {
  return top_area_spacing_;
}

int NavButtonProviderGtk3::GetInterNavButtonSpacing() const {
  return inter_button_spacing_;
}

std::unique_ptr<views::Background>
NavButtonProviderGtk3::CreateAvatarButtonBackground(
    const views::Button* avatar_button) const {
  auto header_context = CreateHeaderContext();
  auto button_context = CreateAvatarButtonContext(header_context);
  return std::make_unique<Gtk3BackgroundPainter>(avatar_button,
                                                 std::move(button_context));
}

void NavButtonProviderGtk3::CalculateCaptionButtonLayout(
    const gfx::Size& content_size,
    int top_area_height,
    gfx::Size* caption_button_size,
    gfx::Insets* caption_button_spacing) const {
  auto header_context = CreateHeaderContext();
  gfx::InsetsF header_padding =
      PaddingFromStyleContext(header_context, GTK_STATE_FLAG_NORMAL);

  auto button_context = CreateAvatarButtonContext(header_context);
  gfx::InsetsF button_padding =
      PaddingFromStyleContext(button_context, GTK_STATE_FLAG_NORMAL);
  gfx::InsetsF button_border =
      BorderFromStyleContext(button_context, GTK_STATE_FLAG_NORMAL);
  gfx::InsetsF button_margin =
      MarginFromStyleContext(button_context, GTK_STATE_FLAG_NORMAL);

  float content_width = content_size.width();
  float content_height = content_size.height();
  if (GtkVersionCheck(3, 20)) {
    int min_width, min_height;
    gtk_style_context_get(button_context, GTK_STATE_FLAG_NORMAL, "min-width",
                          &min_width, "min-height", &min_height, NULL);
    content_width = std::max(content_width, static_cast<float>(min_width));
    content_height = std::max(content_height, static_cast<float>(min_height));
  }

  gfx::InsetsF scalable_insets =
      header_padding + button_padding + button_border + button_margin;
  float scalable_height =
      scalable_insets.top() + scalable_insets.bottom() + content_height;

  float scale = scalable_height > top_area_height && scalable_height != 0
                    ? top_area_height / scalable_height
                    : 1.0f;
  header_padding = header_padding.Scale(scale);
  button_padding = button_padding.Scale(scale);
  button_border = button_border.Scale(scale);
  button_margin = button_margin.Scale(scale);
  // Don't scale |content_width| down if the button is wide.
  if (content_width <= content_height)
    content_width *= scale;
  content_height *= scale;

  float button_height = content_height + button_border.top() +
                        button_border.bottom() + button_padding.top() +
                        button_padding.bottom();
  float button_height_with_margin =
      button_height + button_margin.top() + button_margin.bottom();
  float shiftable_region_start = header_padding.top();
  float shiftable_region_end = top_area_height - header_padding.bottom();
  float button_offset_in_shiftable_region =
      (shiftable_region_end - shiftable_region_start -
       button_height_with_margin) /
      2;

  *caption_button_size = gfx::Size(
      std::round(content_width + button_border.left() + button_border.right() +
                 button_padding.left() + button_padding.right()),
      std::round(button_height));
  *caption_button_spacing = gfx::Insets(
      std::round(shiftable_region_start + button_margin.top() +
                 button_offset_in_shiftable_region),
      std::round(button_margin.left()), 0, std::round(button_margin.right()));
}

}  // namespace libgtkui
