/*
 * Copyright (C) 2008 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "modules/accessibility/AXImageMapLink.h"

#include "SkMatrix44.h"
#include "core/dom/AccessibleNode.h"
#include "core/dom/ElementTraversal.h"
#include "modules/accessibility/AXLayoutObject.h"
#include "modules/accessibility/AXObjectCacheImpl.h"
#include "platform/graphics/Path.h"

namespace blink {

using namespace HTMLNames;

AXImageMapLink::AXImageMapLink(HTMLAreaElement* area,
                               AXObjectCacheImpl& ax_object_cache)
    : AXNodeObject(area, ax_object_cache) {}

AXImageMapLink::~AXImageMapLink() {}

AXImageMapLink* AXImageMapLink::Create(HTMLAreaElement* area,
                                       AXObjectCacheImpl& ax_object_cache) {
  return new AXImageMapLink(area, ax_object_cache);
}

HTMLMapElement* AXImageMapLink::MapElement() const {
  HTMLAreaElement* area = AreaElement();
  if (!area)
    return nullptr;
  return Traversal<HTMLMapElement>::FirstAncestor(*area);
}

AXObject* AXImageMapLink::ComputeParent() const {
  DCHECK(!IsDetached());
  if (parent_)
    return parent_;

  if (!MapElement())
    return nullptr;

  return AXObjectCache().GetOrCreate(MapElement()->GetLayoutObject());
}

AccessibilityRole AXImageMapLink::RoleValue() const {
  const AtomicString& aria_role =
      GetAOMPropertyOrARIAAttribute(AOMStringProperty::kRole);
  if (!aria_role.IsEmpty())
    return AXObject::AriaRoleToWebCoreRole(aria_role);

  return kLinkRole;
}

bool AXImageMapLink::ComputeAccessibilityIsIgnored(
    IgnoredReasons* ignored_reasons) const {
  return AccessibilityIsIgnoredByDefault(ignored_reasons);
}

Element* AXImageMapLink::ActionElement() const {
  return AnchorElement();
}

Element* AXImageMapLink::AnchorElement() const {
  return GetNode() ? ToElement(GetNode()) : nullptr;
}

KURL AXImageMapLink::Url() const {
  if (!AreaElement())
    return KURL();

  return AreaElement()->Href();
}

void AXImageMapLink::GetRelativeBounds(
    AXObject** out_container,
    FloatRect& out_bounds_in_container,
    SkMatrix44& out_container_transform) const {
  *out_container = nullptr;
  out_bounds_in_container = FloatRect();
  out_container_transform.setIdentity();

  HTMLAreaElement* area = AreaElement();
  HTMLMapElement* map = MapElement();
  if (!area || !map)
    return;

  LayoutObject* layout_object;
  if (parent_ && parent_->IsAXLayoutObject())
    layout_object = ToAXLayoutObject(parent_)->GetLayoutObject();
  else
    layout_object = map->GetLayoutObject();

  if (!layout_object)
    return;

  out_bounds_in_container = area->GetPath(layout_object).BoundingRect();
  *out_container = AXObjectCache().GetOrCreate(layout_object);
}

void AXImageMapLink::Trace(blink::Visitor* visitor) {
  AXNodeObject::Trace(visitor);
}

}  // namespace blink
