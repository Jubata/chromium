// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/animation/TransitionKeyframe.h"

#include "bindings/core/v8/V8ObjectBuilder.h"
#include "core/animation/CompositorAnimations.h"
#include "core/animation/InterpolationType.h"
#include "core/animation/PairwiseInterpolationValue.h"
#include "core/animation/TransitionInterpolation.h"

namespace blink {

void TransitionKeyframe::SetCompositorValue(
    scoped_refptr<AnimatableValue> compositor_value) {
  DCHECK_EQ(property_.GetCSSProperty().IsCompositableProperty(),
            static_cast<bool>(compositor_value.get()));
  compositor_value_ = std::move(compositor_value);
}

PropertyHandleSet TransitionKeyframe::Properties() const {
  PropertyHandleSet result;
  result.insert(property_);
  return result;
}

void TransitionKeyframe::AddKeyframePropertiesToV8Object(
    V8ObjectBuilder& object_builder) const {
  Keyframe::AddKeyframePropertiesToV8Object(object_builder);
  // TODO(crbug.com/777971): Add in the property/value for TransitionKeyframe.
}

scoped_refptr<Keyframe::PropertySpecificKeyframe>
TransitionKeyframe::CreatePropertySpecificKeyframe(
    const PropertyHandle& property,
    double offset) const {
  DCHECK(property == property_);
  DCHECK(offset == offset_);
  return PropertySpecificKeyframe::Create(Offset(), &Easing(), Composite(),
                                          value_->Clone(), compositor_value_);
}

scoped_refptr<Interpolation>
TransitionKeyframe::PropertySpecificKeyframe::CreateInterpolation(
    const PropertyHandle& property,
    const Keyframe::PropertySpecificKeyframe& other_super_class) const {
  const PropertySpecificKeyframe& other =
      ToTransitionPropertySpecificKeyframe(other_super_class);
  DCHECK(value_->GetType() == other.value_->GetType());
  return TransitionInterpolation::Create(
      property, value_->GetType(), value_->Value().Clone(),
      other.value_->Value().Clone(), compositor_value_,
      other.compositor_value_);
}

}  // namespace blink
