/*
 * Copyright (C) 2004, 2005, 2006 Apple Computer, Inc.  All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "core/editing/VisibleSelection.h"

#include "bindings/core/v8/ExceptionState.h"
#include "core/dom/Document.h"
#include "core/dom/Element.h"
#include "core/dom/Range.h"
#include "core/editing/EditingUtilities.h"
#include "core/editing/EphemeralRange.h"
#include "core/editing/SelectionAdjuster.h"
#include "core/editing/SelectionTemplate.h"
#include "core/editing/VisiblePosition.h"
#include "core/editing/iterators/CharacterIterator.h"
#include "platform/geometry/LayoutPoint.h"
#include "platform/wtf/Assertions.h"
#include "platform/wtf/text/CString.h"
#include "platform/wtf/text/CharacterNames.h"
#include "platform/wtf/text/StringBuilder.h"

namespace blink {

template <typename Strategy>
VisibleSelectionTemplate<Strategy>::VisibleSelectionTemplate()
    : affinity_(TextAffinity::kDownstream),
      base_is_first_(true),
      is_directional_(false) {}

template <typename Strategy>
VisibleSelectionTemplate<Strategy>::VisibleSelectionTemplate(
    const SelectionTemplate<Strategy>& selection)
    : base_(selection.Base()),
      extent_(selection.Extent()),
      affinity_(selection.Affinity()),
      base_is_first_(selection.IsBaseFirst()),
      is_directional_(selection.IsDirectional()) {}

template <typename Strategy>
VisibleSelectionTemplate<Strategy> VisibleSelectionTemplate<Strategy>::Create(
    const SelectionTemplate<Strategy>& selection) {
  return CreateWithGranularity(selection, TextGranularity::kCharacter);
}

VisibleSelection CreateVisibleSelection(const SelectionInDOMTree& selection) {
  return VisibleSelection::Create(selection);
}

VisibleSelectionInFlatTree CreateVisibleSelection(
    const SelectionInFlatTree& selection) {
  return VisibleSelectionInFlatTree::Create(selection);
}

// TODO(editing-dev): We should move |ComputeVisibleSelection()| to here to
// avoid forward declaration.
template <typename Strategy>
static SelectionTemplate<Strategy> ComputeVisibleSelection(
    const SelectionTemplate<Strategy>&,
    TextGranularity);

template <typename Strategy>
VisibleSelectionTemplate<Strategy>
VisibleSelectionTemplate<Strategy>::CreateWithGranularity(
    const SelectionTemplate<Strategy>& selection,
    TextGranularity granularity) {
  return VisibleSelectionTemplate(
      ComputeVisibleSelection(selection, granularity));
}

VisibleSelection CreateVisibleSelectionWithGranularity(
    const SelectionInDOMTree& selection,
    TextGranularity granularity) {
  return VisibleSelection::CreateWithGranularity(selection, granularity);
}

VisibleSelectionInFlatTree CreateVisibleSelectionWithGranularity(
    const SelectionInFlatTree& selection,
    TextGranularity granularity) {
  return VisibleSelectionInFlatTree::CreateWithGranularity(selection,
                                                           granularity);
}

template <typename Strategy>
static SelectionType ComputeSelectionType(
    const PositionTemplate<Strategy>& start,
    const PositionTemplate<Strategy>& end) {
  if (start.IsNull()) {
    DCHECK(end.IsNull());
    return kNoSelection;
  }
  DCHECK(!NeedsLayoutTreeUpdate(start)) << start << ' ' << end;
  if (start == end)
    return kCaretSelection;
  if (MostBackwardCaretPosition(start) == MostBackwardCaretPosition(end))
    return kCaretSelection;
  return kRangeSelection;
}

template <typename Strategy>
VisibleSelectionTemplate<Strategy>::VisibleSelectionTemplate(
    const VisibleSelectionTemplate<Strategy>& other)
    : base_(other.base_),
      extent_(other.extent_),
      affinity_(other.affinity_),
      base_is_first_(other.base_is_first_),
      is_directional_(other.is_directional_) {}

template <typename Strategy>
VisibleSelectionTemplate<Strategy>& VisibleSelectionTemplate<Strategy>::
operator=(const VisibleSelectionTemplate<Strategy>& other) {
  base_ = other.base_;
  extent_ = other.extent_;
  affinity_ = other.affinity_;
  base_is_first_ = other.base_is_first_;
  is_directional_ = other.is_directional_;
  return *this;
}

template <typename Strategy>
SelectionTemplate<Strategy> VisibleSelectionTemplate<Strategy>::AsSelection()
    const {
  if (base_.IsNull()) {
    return typename SelectionTemplate<Strategy>::Builder()
        .SetIsDirectional(is_directional_)
        .Build();
  }
  return typename SelectionTemplate<Strategy>::Builder()
      .SetBaseAndExtent(base_, extent_)
      .SetAffinity(affinity_)
      .SetIsDirectional(is_directional_)
      .Build();
}

template <typename Strategy>
bool VisibleSelectionTemplate<Strategy>::IsCaret() const {
  return base_.IsNotNull() && base_ == extent_;
}

template <typename Strategy>
bool VisibleSelectionTemplate<Strategy>::IsNone() const {
  return base_.IsNull();
}

template <typename Strategy>
bool VisibleSelectionTemplate<Strategy>::IsRange() const {
  return base_ != extent_;
}

template <typename Strategy>
PositionTemplate<Strategy> VisibleSelectionTemplate<Strategy>::Start() const {
  return base_is_first_ ? base_ : extent_;
}

template <typename Strategy>
PositionTemplate<Strategy> VisibleSelectionTemplate<Strategy>::End() const {
  return base_is_first_ ? extent_ : base_;
}

EphemeralRange FirstEphemeralRangeOf(const VisibleSelection& selection) {
  if (selection.IsNone())
    return EphemeralRange();
  Position start = selection.Start().ParentAnchoredEquivalent();
  Position end = selection.End().ParentAnchoredEquivalent();
  return EphemeralRange(start, end);
}

template <typename Strategy>
EphemeralRangeTemplate<Strategy>
VisibleSelectionTemplate<Strategy>::ToNormalizedEphemeralRange() const {
  if (IsNone())
    return EphemeralRangeTemplate<Strategy>();

  // Make sure we have an updated layout since this function is called
  // in the course of running edit commands which modify the DOM.
  // Failing to ensure this can result in equivalentXXXPosition calls returning
  // incorrect results.
  DCHECK(!NeedsLayoutTreeUpdate(Start())) << *this;

  if (IsCaret()) {
    // If the selection is a caret, move the range start upstream. This
    // helps us match the conventions of text editors tested, which make
    // style determinations based on the character before the caret, if any.
    const PositionTemplate<Strategy> start =
        MostBackwardCaretPosition(Start()).ParentAnchoredEquivalent();
    return EphemeralRangeTemplate<Strategy>(start, start);
  }
  // If the selection is a range, select the minimum range that encompasses
  // the selection. Again, this is to match the conventions of text editors
  // tested, which make style determinations based on the first character of
  // the selection. For instance, this operation helps to make sure that the
  // "X" selected below is the only thing selected. The range should not be
  // allowed to "leak" out to the end of the previous text node, or to the
  // beginning of the next text node, each of which has a different style.
  //
  // On a treasure map, <b>X</b> marks the spot.
  //                       ^ selected
  //
  DCHECK(IsRange());
  return NormalizeRange(EphemeralRangeTemplate<Strategy>(Start(), End()));
}

// TODO(editing-dev): We should move |AdjustSelectionWithTrailingWhitespace()|
// to "SelectionController.cpp" as file local function.
SelectionInFlatTree AdjustSelectionWithTrailingWhitespace(
    const SelectionInFlatTree& selection) {
  if (selection.IsNone())
    return selection;
  if (!selection.IsRange())
    return selection;
  const bool base_is_first =
      selection.Base() == selection.ComputeStartPosition();
  const PositionInFlatTree& end =
      base_is_first ? selection.Extent() : selection.Base();
  DCHECK_EQ(end, selection.ComputeEndPosition());
  const PositionInFlatTree& new_end = SkipWhitespace(end);
  if (end == new_end)
    return selection;
  if (base_is_first) {
    return SelectionInFlatTree::Builder(selection)
        .SetBaseAndExtent(selection.Base(), new_end)
        .Build();
  }
  return SelectionInFlatTree::Builder(selection)
      .SetBaseAndExtent(new_end, selection.Extent())
      .Build();
}

template <typename Strategy>
static SelectionTemplate<Strategy> CanonicalizeSelection(
    const SelectionTemplate<Strategy>& selection) {
  if (selection.IsNone())
    return SelectionTemplate<Strategy>();
  const PositionTemplate<Strategy>& base =
      CreateVisiblePosition(selection.Base(), selection.Affinity())
          .DeepEquivalent();
  if (selection.IsCaret()) {
    if (base.IsNull())
      return SelectionTemplate<Strategy>();
    return
        typename SelectionTemplate<Strategy>::Builder().Collapse(base).Build();
  }
  const PositionTemplate<Strategy>& extent =
      CreateVisiblePosition(selection.Extent(), selection.Affinity())
          .DeepEquivalent();
  if (base.IsNotNull() && extent.IsNotNull()) {
    return typename SelectionTemplate<Strategy>::Builder()
        .SetBaseAndExtent(base, extent)
        .Build();
  }
  if (base.IsNotNull()) {
    return
        typename SelectionTemplate<Strategy>::Builder().Collapse(base).Build();
  }
  if (extent.IsNotNull()) {
    return typename SelectionTemplate<Strategy>::Builder()
        .Collapse(extent)
        .Build();
  }
  return SelectionTemplate<Strategy>();
}

// TODO(editing-dev): Once we move all static functions into anonymous
// namespace, we should get rid of this forward declaration.
template <typename Strategy>
static EphemeralRangeTemplate<Strategy>
AdjustSelectionToAvoidCrossingEditingBoundaries(
    const EphemeralRangeTemplate<Strategy>&,
    const PositionTemplate<Strategy>& base);

// TODO(editing-dev): Move this to SelectionAdjuster.
template <typename Strategy>
static SelectionTemplate<Strategy>
AdjustSelectionToAvoidCrossingShadowBoundaries(
    const SelectionTemplate<Strategy>& granularity_adjusted_selection) {
  const EphemeralRangeTemplate<Strategy> expanded_range(
      granularity_adjusted_selection.ComputeStartPosition(),
      granularity_adjusted_selection.ComputeEndPosition());

  const EphemeralRangeTemplate<Strategy> shadow_adjusted_range =
      granularity_adjusted_selection.IsBaseFirst()
          ? EphemeralRangeTemplate<Strategy>(
                expanded_range.StartPosition(),
                SelectionAdjuster::
                    AdjustSelectionEndToAvoidCrossingShadowBoundaries(
                        expanded_range))
          : EphemeralRangeTemplate<Strategy>(
                SelectionAdjuster::
                    AdjustSelectionStartToAvoidCrossingShadowBoundaries(
                        expanded_range),
                expanded_range.EndPosition());
  typename SelectionTemplate<Strategy>::Builder builder;
  return granularity_adjusted_selection.IsBaseFirst()
             ? builder.SetAsForwardSelection(shadow_adjusted_range).Build()
             : builder.SetAsBackwardSelection(shadow_adjusted_range).Build();
}

// TODO(editing-dev): Move this to SelectionAdjuster.
template <typename Strategy>
static SelectionTemplate<Strategy>
AdjustSelectionToAvoidCrossingEditingBoundaries(
    const SelectionTemplate<Strategy>& shadow_adjusted_selection) {
  // TODO(editing-dev): Refactor w/o EphemeralRange.
  const EphemeralRangeTemplate<Strategy> shadow_adjusted_range(
      shadow_adjusted_selection.ComputeStartPosition(),
      shadow_adjusted_selection.ComputeEndPosition());
  const EphemeralRangeTemplate<Strategy> editing_adjusted_range =
      AdjustSelectionToAvoidCrossingEditingBoundaries(
          shadow_adjusted_range, shadow_adjusted_selection.Base());
  typename SelectionTemplate<Strategy>::Builder builder;
  if (editing_adjusted_range.IsCollapsed())
    return builder.Collapse(editing_adjusted_range.StartPosition()).Build();
  return shadow_adjusted_selection.IsBaseFirst()
             ? builder.SetAsForwardSelection(editing_adjusted_range).Build()
             : builder.SetAsBackwardSelection(editing_adjusted_range).Build();
}

template <typename Strategy>
static SelectionTemplate<Strategy> ComputeVisibleSelection(
    const SelectionTemplate<Strategy>& passed_selection,
    TextGranularity granularity) {
  DCHECK(!NeedsLayoutTreeUpdate(passed_selection.Base()));
  DCHECK(!NeedsLayoutTreeUpdate(passed_selection.Extent()));

  const SelectionTemplate<Strategy>& canonicalized_selection =
      CanonicalizeSelection(passed_selection);

  if (canonicalized_selection.IsNone())
    return SelectionTemplate<Strategy>();

  const SelectionTemplate<Strategy>& granularity_adjusted_selection =
      SelectionAdjuster::AdjustSelectionRespectingGranularity(
          canonicalized_selection, granularity);
  const SelectionTemplate<Strategy>& shadow_adjusted_selection =
      AdjustSelectionToAvoidCrossingShadowBoundaries(
          granularity_adjusted_selection);
  const SelectionTemplate<Strategy>& editing_adjusted_selection =
      AdjustSelectionToAvoidCrossingEditingBoundaries(
          shadow_adjusted_selection);
  const EphemeralRangeTemplate<Strategy> editing_adjusted_range(
      editing_adjusted_selection.ComputeStartPosition(),
      editing_adjusted_selection.ComputeEndPosition());
  // TODO(editing-dev): Implement
  // const SelectionTemplate<Strategy>& adjusted_selection =
  // AdjustSelectionType(editing_adjusted_range);
  const SelectionType selection_type =
      ComputeSelectionType(editing_adjusted_range.StartPosition(),
                           editing_adjusted_range.EndPosition());
  DCHECK_NE(selection_type, kNoSelection);

  // "Constrain" the selection to be the smallest equivalent range of
  // nodes. This is a somewhat arbitrary choice, but experience shows that
  // it is useful to make to make the selection "canonical" (if only for
  // purposes of comparing selections). This is an ideal point of the code
  // to do this operation, since all selection changes that result in a
  // RANGE come through here before anyone uses it.
  // TODO(yosin) Canonicalizing is good, but haven't we already done it
  // (when we set these two positions to |VisiblePosition|
  // |DeepEquivalent()|s above)?
  const EphemeralRangeTemplate<Strategy> range =
      selection_type == kRangeSelection
          ? EphemeralRangeTemplate<Strategy>(
                MostForwardCaretPosition(
                    editing_adjusted_range.StartPosition()),
                MostBackwardCaretPosition(editing_adjusted_range.EndPosition()))
          : editing_adjusted_range;
  if (selection_type == kCaretSelection) {
    return typename SelectionTemplate<Strategy>::Builder()
        .Collapse(PositionWithAffinityTemplate<Strategy>(
            range.StartPosition(), passed_selection.Affinity()))
        .SetIsDirectional(passed_selection.IsDirectional())
        .Build();
  }
  if (canonicalized_selection.IsBaseFirst()) {
    return typename SelectionTemplate<Strategy>::Builder()
        .SetIsDirectional(passed_selection.IsDirectional())
        .SetAsForwardSelection(range)
        .Build();
  }
  return typename SelectionTemplate<Strategy>::Builder()
      .SetIsDirectional(passed_selection.IsDirectional())
      .SetAsBackwardSelection(range)
      .Build();
}

template <typename Strategy>
bool VisibleSelectionTemplate<Strategy>::IsValidFor(
    const Document& document) const {
  if (IsNone())
    return true;
  return base_.IsValidFor(document) && extent_.IsValidFor(document);
}

// TODO(yosin) This function breaks the invariant of this class.
// But because we use VisibleSelection to store values in editing commands for
// use when undoing the command, we need to be able to create a selection that
// while currently invalid, will be valid once the changes are undone. This is a
// design problem. To fix it we either need to change the invariants of
// |VisibleSelection| or create a new class for editing to use that can
// manipulate selections that are not currently valid.
template <typename Strategy>
VisibleSelectionTemplate<Strategy>
VisibleSelectionTemplate<Strategy>::CreateWithoutValidationDeprecated(
    const PositionTemplate<Strategy>& base,
    const PositionTemplate<Strategy>& extent,
    TextAffinity affinity) {
  DCHECK(base.IsNotNull());
  DCHECK(extent.IsNotNull());

  VisibleSelectionTemplate<Strategy> visible_selection;
  visible_selection.base_ = base;
  visible_selection.extent_ = extent;
  visible_selection.base_is_first_ = base.CompareTo(extent) <= 0;
  if (base == extent) {
    visible_selection.affinity_ = affinity;
    return visible_selection;
  }
  // Since |affinity_| for non-|CaretSelection| is always |kDownstream|,
  // we should keep this invariant. Note: This function can be called with
  // |affinity_| is |kUpstream|.
  visible_selection.affinity_ = TextAffinity::kDownstream;
  return visible_selection;
}

static Element* LowestEditableAncestor(Node* node) {
  while (node) {
    if (HasEditableStyle(*node))
      return RootEditableElement(*node);
    if (IsHTMLBodyElement(*node))
      break;
    node = node->parentNode();
  }

  return nullptr;
}

// Returns true if |position| is editable or its lowest editable root is not
// |base_editable_ancestor|.
template <typename Strategy>
static bool ShouldContinueSearchEditingBoundary(
    const PositionTemplate<Strategy>& position,
    Element* base_editable_ancestor) {
  if (position.IsNull())
    return false;
  if (IsEditablePosition(position))
    return true;
  return LowestEditableAncestor(position.ComputeContainerNode()) !=
         base_editable_ancestor;
}

template <typename Strategy>
static bool ShouldAdjustPositionToAvoidCrossingEditingBoundaries(
    const PositionTemplate<Strategy>& position,
    const ContainerNode* editable_root,
    const Element* base_editable_ancestor) {
  if (editable_root)
    return true;
  Element* const editable_ancestor =
      LowestEditableAncestor(position.ComputeContainerNode());
  return editable_ancestor != base_editable_ancestor;
}

// The selection ends in editable content or non-editable content inside a
// different editable ancestor, move backward until non-editable content inside
// the same lowest editable ancestor is reached.
template <typename Strategy>
PositionTemplate<Strategy> AdjustSelectionEndToAvoidCrossingEditingBoundaries(
    const PositionTemplate<Strategy>& end,
    ContainerNode* end_root,
    Element* base_editable_ancestor) {
  if (ShouldAdjustPositionToAvoidCrossingEditingBoundaries(
          end, end_root, base_editable_ancestor)) {
    PositionTemplate<Strategy> position =
        PreviousVisuallyDistinctCandidate(end);
    Element* shadow_ancestor = end_root ? end_root->OwnerShadowHost() : nullptr;
    if (position.IsNull() && shadow_ancestor)
      position = PositionTemplate<Strategy>::AfterNode(*shadow_ancestor);
    while (
        ShouldContinueSearchEditingBoundary(position, base_editable_ancestor)) {
      Element* root = RootEditableElementOf(position);
      shadow_ancestor = root ? root->OwnerShadowHost() : nullptr;
      position = IsAtomicNode(position.ComputeContainerNode())
                     ? PositionTemplate<Strategy>::InParentBeforeNode(
                           *position.ComputeContainerNode())
                     : PreviousVisuallyDistinctCandidate(position);
      if (position.IsNull() && shadow_ancestor)
        position = PositionTemplate<Strategy>::AfterNode(*shadow_ancestor);
    }
    return CreateVisiblePosition(position).DeepEquivalent();
  }
  return end;
}

// The selection starts in editable content or non-editable content inside a
// different editable ancestor, move forward until non-editable content inside
// the same lowest editable ancestor is reached.
template <typename Strategy>
PositionTemplate<Strategy> AdjustSelectionStartToAvoidCrossingEditingBoundaries(
    const PositionTemplate<Strategy>& start,
    ContainerNode* start_root,
    Element* base_editable_ancestor) {
  if (ShouldAdjustPositionToAvoidCrossingEditingBoundaries(
          start, start_root, base_editable_ancestor)) {
    PositionTemplate<Strategy> position = NextVisuallyDistinctCandidate(start);
    Element* shadow_ancestor =
        start_root ? start_root->OwnerShadowHost() : nullptr;
    if (position.IsNull() && shadow_ancestor)
      position = PositionTemplate<Strategy>::BeforeNode(*shadow_ancestor);
    while (
        ShouldContinueSearchEditingBoundary(position, base_editable_ancestor)) {
      Element* root = RootEditableElementOf(position);
      shadow_ancestor = root ? root->OwnerShadowHost() : nullptr;
      position = IsAtomicNode(position.ComputeContainerNode())
                     ? PositionTemplate<Strategy>::InParentAfterNode(
                           *position.ComputeContainerNode())
                     : NextVisuallyDistinctCandidate(position);
      if (position.IsNull() && shadow_ancestor)
        position = PositionTemplate<Strategy>::BeforeNode(*shadow_ancestor);
    }
    return CreateVisiblePosition(position).DeepEquivalent();
  }
  return start;
}

template <typename Strategy>
static EphemeralRangeTemplate<Strategy>
AdjustSelectionToAvoidCrossingEditingBoundaries(
    const EphemeralRangeTemplate<Strategy>& range,
    const PositionTemplate<Strategy>& base) {
  DCHECK(base.IsNotNull());
  DCHECK(range.IsNotNull());

  ContainerNode* base_root = HighestEditableRoot(base);
  ContainerNode* start_root = HighestEditableRoot(range.StartPosition());
  ContainerNode* end_root = HighestEditableRoot(range.EndPosition());

  Element* base_editable_ancestor =
      LowestEditableAncestor(base.ComputeContainerNode());

  // The base, start and end are all in the same region.  No adjustment
  // necessary.
  if (base_root == start_root && base_root == end_root)
    return range;

  // The selection is based in editable content.
  if (base_root) {
    // If the start is outside the base's editable root, cap it at the start of
    // that root.
    // If the start is in non-editable content that is inside the base's
    // editable root, put it at the first editable position after start inside
    // the base's editable root.
    PositionTemplate<Strategy> start = range.StartPosition();
    if (start_root != base_root) {
      const VisiblePositionTemplate<Strategy> first =
          FirstEditableVisiblePositionAfterPositionInRoot(start, *base_root);
      start = first.DeepEquivalent();
      if (start.IsNull()) {
        NOTREACHED();
        return {};
      }
    }
    // If the end is outside the base's editable root, cap it at the end of that
    // root.
    // If the end is in non-editable content that is inside the base's root, put
    // it at the last editable position before the end inside the base's root.
    PositionTemplate<Strategy> end = range.EndPosition();
    if (end_root != base_root) {
      const VisiblePositionTemplate<Strategy> last =
          LastEditableVisiblePositionBeforePositionInRoot(end, *base_root);
      end = last.DeepEquivalent();
      if (end.IsNull())
        end = start;
    }
    return {start, end};
  } else {
    // The selection is based in non-editable content.
    // FIXME: Non-editable pieces inside editable content should be atomic, in
    // the same way that editable pieces in non-editable content are atomic.
    const PositionTemplate<Strategy>& end =
        AdjustSelectionEndToAvoidCrossingEditingBoundaries(
            range.EndPosition(), end_root, base_editable_ancestor);
    if (end.IsNull()) {
      // The selection crosses an Editing boundary.  This is a
      // programmer error in the editing code.  Happy debugging!
      NOTREACHED();
      return {};
    }

    const PositionTemplate<Strategy>& start =
        AdjustSelectionStartToAvoidCrossingEditingBoundaries(
            range.StartPosition(), start_root, base_editable_ancestor);
    if (start.IsNull()) {
      // The selection crosses an Editing boundary.  This is a
      // programmer error in the editing code.  Happy debugging!
      NOTREACHED();
      return {};
    }
    return {start, end};
  }
}

template <typename Strategy>
bool VisibleSelectionTemplate<Strategy>::IsContentEditable() const {
  return IsEditablePosition(Start());
}

template <typename Strategy>
Element* VisibleSelectionTemplate<Strategy>::RootEditableElement() const {
  return RootEditableElementOf(Start());
}

template <typename Strategy>
static bool EqualSelectionsAlgorithm(
    const VisibleSelectionTemplate<Strategy>& selection1,
    const VisibleSelectionTemplate<Strategy>& selection2) {
  if (selection1.Affinity() != selection2.Affinity() ||
      selection1.IsDirectional() != selection2.IsDirectional())
    return false;

  if (selection1.IsNone())
    return selection2.IsNone();

  const VisibleSelectionTemplate<Strategy> selection_wrapper1(selection1);
  const VisibleSelectionTemplate<Strategy> selection_wrapper2(selection2);

  return selection_wrapper1.Base() == selection_wrapper2.Base() &&
         selection_wrapper1.Extent() == selection_wrapper2.Extent();
}

template <typename Strategy>
bool VisibleSelectionTemplate<Strategy>::operator==(
    const VisibleSelectionTemplate<Strategy>& other) const {
  return EqualSelectionsAlgorithm<Strategy>(*this, other);
}

template <typename Strategy>
VisiblePositionTemplate<Strategy>
VisibleSelectionTemplate<Strategy>::VisibleStart() const {
  return CreateVisiblePosition(
      Start(), IsRange() ? TextAffinity::kDownstream : Affinity());
}

template <typename Strategy>
VisiblePositionTemplate<Strategy>
VisibleSelectionTemplate<Strategy>::VisibleEnd() const {
  return CreateVisiblePosition(
      End(), IsRange() ? TextAffinity::kUpstream : Affinity());
}

template <typename Strategy>
VisiblePositionTemplate<Strategy>
VisibleSelectionTemplate<Strategy>::VisibleBase() const {
  return CreateVisiblePosition(
      base_, IsRange() ? (IsBaseFirst() ? TextAffinity::kUpstream
                                        : TextAffinity::kDownstream)
                       : Affinity());
}

template <typename Strategy>
VisiblePositionTemplate<Strategy>
VisibleSelectionTemplate<Strategy>::VisibleExtent() const {
  return CreateVisiblePosition(
      extent_, IsRange() ? (IsBaseFirst() ? TextAffinity::kDownstream
                                          : TextAffinity::kUpstream)
                         : Affinity());
}

template <typename Strategy>
void VisibleSelectionTemplate<Strategy>::Trace(blink::Visitor* visitor) {
  visitor->Trace(base_);
  visitor->Trace(extent_);
}

#ifndef NDEBUG

template <typename Strategy>
void VisibleSelectionTemplate<Strategy>::ShowTreeForThis() const {
  if (!Start().AnchorNode())
    return;
  LOG(INFO) << "\n"
            << Start()
                   .AnchorNode()
                   ->ToMarkedTreeString(Start().AnchorNode(), "S",
                                        End().AnchorNode(), "E")
                   .Utf8()
                   .data()
            << "start: " << Start().ToAnchorTypeAndOffsetString().Utf8().data()
            << "\n"
            << "end: " << End().ToAnchorTypeAndOffsetString().Utf8().data();
}

#endif

template <typename Strategy>
void VisibleSelectionTemplate<Strategy>::PrintTo(
    const VisibleSelectionTemplate<Strategy>& selection,
    std::ostream* ostream) {
  if (selection.IsNone()) {
    *ostream << "VisibleSelection()";
    return;
  }
  *ostream << "VisibleSelection(base: " << selection.Base()
           << " extent:" << selection.Extent()
           << " start: " << selection.Start() << " end: " << selection.End()
           << ' ' << selection.Affinity() << ' '
           << (selection.IsDirectional() ? "Directional" : "NonDirectional")
           << ')';
}

template class CORE_TEMPLATE_EXPORT VisibleSelectionTemplate<EditingStrategy>;
template class CORE_TEMPLATE_EXPORT
    VisibleSelectionTemplate<EditingInFlatTreeStrategy>;

std::ostream& operator<<(std::ostream& ostream,
                         const VisibleSelection& selection) {
  VisibleSelection::PrintTo(selection, &ostream);
  return ostream;
}

std::ostream& operator<<(std::ostream& ostream,
                         const VisibleSelectionInFlatTree& selection) {
  VisibleSelectionInFlatTree::PrintTo(selection, &ostream);
  return ostream;
}

}  // namespace blink

#ifndef NDEBUG

void showTree(const blink::VisibleSelection& sel) {
  sel.ShowTreeForThis();
}

void showTree(const blink::VisibleSelection* sel) {
  if (sel)
    sel->ShowTreeForThis();
}

void showTree(const blink::VisibleSelectionInFlatTree& sel) {
  sel.ShowTreeForThis();
}

void showTree(const blink::VisibleSelectionInFlatTree* sel) {
  if (sel)
    sel->ShowTreeForThis();
}
#endif
