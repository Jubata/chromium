// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/css/properties/longhands/MaxInlineSizeOrMaxLogicalWidth.h"

#include "core/css/properties/CSSPropertyLengthUtils.h"

namespace blink {
namespace CSSLonghand {

const CSSValue* MaxInlineSizeOrMaxLogicalWidth::ParseSingleValue(
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&) const {
  return CSSPropertyLengthUtils::ConsumeMaxWidthOrHeight(range, context);
}

}  // namespace CSSLonghand
}  // namespace blink
