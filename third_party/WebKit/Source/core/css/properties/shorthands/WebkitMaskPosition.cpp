// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/css/properties/shorthands/WebkitMaskPosition.h"

#include "core/css/parser/CSSParserContext.h"
#include "core/css/parser/CSSPropertyParserHelpers.h"
#include "core/css/properties/CSSPropertyBackgroundUtils.h"

namespace blink {
namespace CSSShorthand {

bool WebkitMaskPosition::ParseShorthand(
    bool important,
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&,
    HeapVector<CSSPropertyValue, 256>& properties) const {
  CSSValue* result_x = nullptr;
  CSSValue* result_y = nullptr;

  if (!CSSPropertyBackgroundUtils::ConsumeBackgroundPosition(
          range, context, CSSPropertyParserHelpers::UnitlessQuirk::kAllow,
          result_x, result_y) ||
      !range.AtEnd())
    return false;

  CSSPropertyParserHelpers::AddProperty(
      CSSPropertyWebkitMaskPositionX, CSSPropertyWebkitMaskPosition, *result_x,
      important, CSSPropertyParserHelpers::IsImplicitProperty::kNotImplicit,
      properties);

  CSSPropertyParserHelpers::AddProperty(
      CSSPropertyWebkitMaskPositionY, CSSPropertyWebkitMaskPosition, *result_y,
      important, CSSPropertyParserHelpers::IsImplicitProperty::kNotImplicit,
      properties);
  return true;
}

}  // namespace CSSShorthand
}  // namespace blink
