/*
 * Copyright (C) 2003, 2006, 2009 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "platform/geometry/IntRect.h"

#include "platform/geometry/FloatRect.h"
#include "platform/geometry/LayoutRect.h"
#include "platform/wtf/Vector.h"
#include "platform/wtf/text/WTFString.h"
#include "third_party/skia/include/core/SkRect.h"
#include "ui/gfx/geometry/rect.h"

#include <algorithm>

namespace blink {

IntRect::IntRect(const FloatRect& r)
    : location_(clampTo<int>(r.X()), clampTo<int>(r.Y())),
      size_(clampTo<int>(r.Width()), clampTo<int>(r.Height())) {}

IntRect::IntRect(const LayoutRect& r)
    : location_(r.X().ToInt(), r.Y().ToInt()),
      size_(r.Width().ToInt(), r.Height().ToInt()) {}

void IntRect::ShiftXEdgeTo(int edge) {
  int delta = edge - X();
  SetX(edge);
  SetWidth(std::max(0, Width() - delta));
}

void IntRect::ShiftMaxXEdgeTo(int edge) {
  int delta = edge - MaxX();
  SetWidth(std::max(0, Width() + delta));
}

void IntRect::ShiftYEdgeTo(int edge) {
  int delta = edge - Y();
  SetY(edge);
  SetHeight(std::max(0, Height() - delta));
}

void IntRect::ShiftMaxYEdgeTo(int edge) {
  int delta = edge - MaxY();
  SetHeight(std::max(0, Height() + delta));
}

bool IntRect::Intersects(const IntRect& other) const {
  // Checking emptiness handles negative widths as well as zero.
  return !IsEmpty() && !other.IsEmpty() && X() < other.MaxX() &&
         other.X() < MaxX() && Y() < other.MaxY() && other.Y() < MaxY();
}

bool IntRect::Contains(const IntRect& other) const {
  return X() <= other.X() && MaxX() >= other.MaxX() && Y() <= other.Y() &&
         MaxY() >= other.MaxY();
}

void IntRect::Intersect(const IntRect& other) {
  int left = std::max(X(), other.X());
  int top = std::max(Y(), other.Y());
  int right = std::min(MaxX(), other.MaxX());
  int bottom = std::min(MaxY(), other.MaxY());

  // Return a clean empty rectangle for non-intersecting cases.
  if (left >= right || top >= bottom) {
    left = 0;
    top = 0;
    right = 0;
    bottom = 0;
  }

  location_.SetX(left);
  location_.SetY(top);
  size_.SetWidth(right - left);
  size_.SetHeight(bottom - top);
}

void IntRect::Unite(const IntRect& other) {
  // Handle empty special cases first.
  if (other.IsEmpty())
    return;
  if (IsEmpty()) {
    *this = other;
    return;
  }

  UniteEvenIfEmpty(other);
}

void IntRect::UniteIfNonZero(const IntRect& other) {
  // Handle empty special cases first.
  if (!other.Width() && !other.Height())
    return;
  if (!Width() && !Height()) {
    *this = other;
    return;
  }

  UniteEvenIfEmpty(other);
}

void IntRect::UniteEvenIfEmpty(const IntRect& other) {
  int left = std::min(X(), other.X());
  int top = std::min(Y(), other.Y());
  int right = std::max(MaxX(), other.MaxX());
  int bottom = std::max(MaxY(), other.MaxY());

  location_.SetX(left);
  location_.SetY(top);
  size_.SetWidth(right - left);
  size_.SetHeight(bottom - top);
}

void IntRect::Scale(float s) {
  location_.SetX((int)(X() * s));
  location_.SetY((int)(Y() * s));
  size_.SetWidth((int)(Width() * s));
  size_.SetHeight((int)(Height() * s));
}

static inline int DistanceToInterval(int pos, int start, int end) {
  if (pos < start)
    return start - pos;
  if (pos > end)
    return end - pos;
  return 0;
}

IntSize IntRect::DifferenceToPoint(const IntPoint& point) const {
  int xdistance = DistanceToInterval(point.X(), X(), MaxX());
  int ydistance = DistanceToInterval(point.Y(), Y(), MaxY());
  return IntSize(xdistance, ydistance);
}

IntRect::operator SkIRect() const {
  SkIRect rect = {X(), Y(), MaxX(), MaxY()};
  return rect;
}

IntRect::operator SkRect() const {
  SkRect rect;
  rect.set(SkIntToScalar(X()), SkIntToScalar(Y()), SkIntToScalar(MaxX()),
           SkIntToScalar(MaxY()));
  return rect;
}

IntRect::operator gfx::Rect() const {
  return gfx::Rect(X(), Y(), Width(), Height());
}

IntRect UnionRect(const Vector<IntRect>& rects) {
  IntRect result;

  size_t count = rects.size();
  for (size_t i = 0; i < count; ++i)
    result.Unite(rects[i]);

  return result;
}

IntRect UnionRectEvenIfEmpty(const Vector<IntRect>& rects) {
  size_t count = rects.size();
  if (!count)
    return IntRect();

  IntRect result = rects[0];
  for (size_t i = 1; i < count; ++i)
    result.UniteEvenIfEmpty(rects[i]);

  return result;
}

std::ostream& operator<<(std::ostream& ostream, const IntRect& rect) {
  return ostream << rect.ToString();
}

String IntRect::ToString() const {
  return String::Format("%s %s", Location().ToString().Ascii().data(),
                        Size().ToString().Ascii().data());
}

}  // namespace blink
