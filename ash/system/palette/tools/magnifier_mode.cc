// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/system/palette/tools/magnifier_mode.h"

#include "ash/magnifier/partial_magnification_controller.h"
#include "ash/resources/vector_icons/vector_icons.h"
#include "ash/shell.h"
#include "ash/strings/grit/ash_strings.h"
#include "ash/system/palette/palette_ids.h"
#include "ui/base/l10n/l10n_util.h"

namespace ash {

MagnifierMode::MagnifierMode(Delegate* delegate)
    : CommonPaletteTool(delegate) {}

MagnifierMode::~MagnifierMode() {}

PaletteGroup MagnifierMode::GetGroup() const {
  return PaletteGroup::MODE;
}

PaletteToolId MagnifierMode::GetToolId() const {
  return PaletteToolId::MAGNIFY;
}

const gfx::VectorIcon& MagnifierMode::GetActiveTrayIcon() const {
  return kPaletteTrayIconMagnifyIcon;
}

void MagnifierMode::OnEnable() {
  CommonPaletteTool::OnEnable();
  Shell::Get()->partial_magnification_controller()->SetEnabled(true);
  delegate()->HidePalette();
}

void MagnifierMode::OnDisable() {
  CommonPaletteTool::OnDisable();
  Shell::Get()->partial_magnification_controller()->SetEnabled(false);
}

views::View* MagnifierMode::CreateView() {
  return CreateDefaultView(
      l10n_util::GetStringUTF16(IDS_ASH_STYLUS_TOOLS_MAGNIFIER_MODE));
}

const gfx::VectorIcon& MagnifierMode::GetPaletteIcon() const {
  return kPaletteModeMagnifyIcon;
}

}  // namespace ash
