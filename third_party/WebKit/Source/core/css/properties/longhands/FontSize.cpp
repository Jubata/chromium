// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/css/properties/longhands/FontSize.h"

#include "core/css/parser/CSSParserContext.h"
#include "core/css/parser/CSSPropertyParserHelpers.h"
#include "core/css/properties/CSSPropertyFontUtils.h"

namespace blink {
namespace CSSLonghand {

const CSSValue* FontSize::ParseSingleValue(CSSParserTokenRange& range,
                                           const CSSParserContext& context,
                                           const CSSParserLocalContext&) const {
  return CSSPropertyFontUtils::ConsumeFontSize(
      range, context.Mode(), CSSPropertyParserHelpers::UnitlessQuirk::kAllow);
}

}  // namespace CSSLonghand
}  // namespace blink
