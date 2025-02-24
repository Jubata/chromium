/*
 * (C) 1999-2003 Lars Knoll (knoll@kde.org)
 * (C) 2002-2003 Dirk Mueller (mueller@kde.org)
 * Copyright (C) 2002, 2005, 2006, 2008, 2012 Apple Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "core/css/CSSFontFaceRule.h"

#include "core/css/CSSPropertyValueSet.h"
#include "core/css/PropertySetCSSStyleDeclaration.h"
#include "core/css/StyleRule.h"
#include "platform/wtf/text/StringBuilder.h"

namespace blink {

CSSFontFaceRule::CSSFontFaceRule(StyleRuleFontFace* font_face_rule,
                                 CSSStyleSheet* parent)
    : CSSRule(parent), font_face_rule_(font_face_rule) {}

CSSFontFaceRule::~CSSFontFaceRule() {}

CSSStyleDeclaration* CSSFontFaceRule::style() const {
  if (!properties_cssom_wrapper_)
    properties_cssom_wrapper_ = StyleRuleCSSStyleDeclaration::Create(
        font_face_rule_->MutableProperties(),
        const_cast<CSSFontFaceRule*>(this));
  return properties_cssom_wrapper_.Get();
}

String CSSFontFaceRule::cssText() const {
  StringBuilder result;
  result.Append("@font-face { ");
  String descs = font_face_rule_->Properties().AsText();
  result.Append(descs);
  if (!descs.IsEmpty())
    result.Append(' ');
  result.Append('}');
  return result.ToString();
}

void CSSFontFaceRule::Reattach(StyleRuleBase* rule) {
  DCHECK(rule);
  font_face_rule_ = ToStyleRuleFontFace(rule);
  if (properties_cssom_wrapper_)
    properties_cssom_wrapper_->Reattach(font_face_rule_->MutableProperties());
}

void CSSFontFaceRule::Trace(blink::Visitor* visitor) {
  visitor->Trace(font_face_rule_);
  visitor->Trace(properties_cssom_wrapper_);
  CSSRule::Trace(visitor);
}

}  // namespace blink
