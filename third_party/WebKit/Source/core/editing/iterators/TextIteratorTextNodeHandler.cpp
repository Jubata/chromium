// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/editing/iterators/TextIteratorTextNodeHandler.h"

#include <algorithm>
#include "core/dom/FirstLetterPseudoElement.h"
#include "core/editing/EphemeralRange.h"
#include "core/editing/iterators/TextIteratorTextState.h"
#include "core/layout/LayoutTextFragment.h"
#include "core/layout/line/InlineTextBox.h"
#include "core/layout/line/RootInlineBox.h"
#include "core/layout/ng/inline/ng_offset_mapping.h"

namespace blink {

namespace {

// A magic value for infinity, used to indicate that text emission should
// proceed till the end of the text node. Can be removed when we can handle text
// length differences due to text-transform correctly.
const unsigned kMaxOffset = std::numeric_limits<unsigned>::max();

bool ShouldSkipInvisibleTextAt(const Text& text,
                               unsigned offset,
                               bool ignores_visibility) {
  // TODO(xiaochengh): Get style from NGInlineItem or NGPhysicalTextFragment.
  const LayoutObject* layout_object = AssociatedLayoutObjectOf(text, offset);
  if (!layout_object)
    return true;
  if (layout_object->Style()->Display() == EDisplay::kNone)
    return true;
  if (ignores_visibility)
    return false;
  return layout_object->Style()->Visibility() != EVisibility::kVisible;
}

// TODO(xiaochengh): Use a return type with better clarity
std::pair<String, std::pair<unsigned, unsigned>>
ComputeTextAndOffsetsForEmission(const NGOffsetMapping& mapping,
                                 const NGOffsetMappingUnit& unit,
                                 unsigned run_start,
                                 unsigned run_end,
                                 const TextIteratorBehavior& behavior) {
  // TODO(xiaochengh): Handle EmitsOriginalText.
  unsigned text_content_start = unit.ConvertDOMOffsetToTextContent(run_start);
  unsigned text_content_end = unit.ConvertDOMOffsetToTextContent(run_end);
  unsigned length = text_content_end - text_content_start;
  if (behavior.EmitsSpaceForNbsp()) {
    String string = mapping.GetText().Substring(text_content_start, length);
    return {string, {0u, 0u}};
  }
  return {mapping.GetText(), {text_content_start, text_content_end}};
}

}  // namespace

TextIteratorTextNodeHandler::TextIteratorTextNodeHandler(
    const TextIteratorBehavior& behavior,
    TextIteratorTextState* text_state)
    : behavior_(behavior), text_state_(*text_state) {}

bool TextIteratorTextNodeHandler::HandleRemainingTextRuns() {
  if (uses_layout_ng_) {
    HandleTextNodeWithLayoutNG();
    return text_state_.PositionNode();
  }

  if (ShouldProceedToRemainingText())
    ProceedToRemainingText();
  // Handle remembered text box
  if (text_box_) {
    HandleTextBox();
    return text_state_.PositionNode();
  }
  // Handle remembered pre-formatted text node.
  if (!needs_handle_pre_formatted_text_node_)
    return false;
  HandlePreFormattedTextNode();
  return text_state_.PositionNode();
}

void TextIteratorTextNodeHandler::HandleTextNodeWithLayoutNG() {
  DCHECK_LE(offset_, end_offset_);
  DCHECK_LE(end_offset_, text_node_->data().length());

  if (ShouldSkipInvisibleTextAt(*text_node_, offset_,
                                IgnoresStyleVisibility())) {
    offset_ = end_offset_;
    return;
  }

  while (offset_ < end_offset_ && !text_state_.PositionNode()) {
    const EphemeralRange range_to_emit(Position(text_node_, offset_),
                                       Position(text_node_, end_offset_));

    // We may go through multiple mappings, which happens when there is
    // ::first-letter and blockifying style.
    auto* mapping = NGOffsetMapping::GetFor(range_to_emit.StartPosition());
    if (!mapping) {
      offset_ = end_offset_;
      return;
    }

    const unsigned initial_offset = offset_;
    for (const NGOffsetMappingUnit& unit :
         mapping->GetMappingUnitsForDOMRange(range_to_emit)) {
      const unsigned run_start = std::max(offset_, unit.DOMStart());
      const unsigned run_end = std::min(end_offset_, unit.DOMEnd());
      if (run_start >= run_end ||
          unit.ConvertDOMOffsetToTextContent(run_start) ==
              unit.ConvertDOMOffsetToTextContent(run_end)) {
        offset_ = run_end;
        continue;
      }

      auto string_and_offsets = ComputeTextAndOffsetsForEmission(
          *mapping, unit, run_start, run_end, behavior_);
      const String& string = string_and_offsets.first;
      const unsigned text_content_start = string_and_offsets.second.first;
      const unsigned text_content_end = string_and_offsets.second.second;
      text_state_.EmitText(text_node_, run_start, run_end, string,
                           text_content_start, text_content_end);
      offset_ = run_end;
      return;
    }

    // Bail if |offset_| isn't advanced; Otherwise we enter a dead loop.
    // However, this shouldn't happen and should be fixed once reached.
    if (offset_ == initial_offset) {
      NOTREACHED();
      offset_ = end_offset_;
      return;
    }
  }
}

bool TextIteratorTextNodeHandler::ShouldHandleFirstLetter(
    const LayoutText& layout_text) const {
  if (handled_first_letter_)
    return false;
  if (!layout_text.IsTextFragment())
    return false;
  const LayoutTextFragment& text_fragment = ToLayoutTextFragment(layout_text);
  return offset_ < text_fragment.TextStartOffset();
}

static bool HasVisibleTextNode(LayoutText* layout_object) {
  if (layout_object->Style()->Visibility() == EVisibility::kVisible)
    return true;

  if (!layout_object->IsTextFragment())
    return false;

  LayoutTextFragment* fragment = ToLayoutTextFragment(layout_object);
  if (!fragment->IsRemainingTextLayoutObject())
    return false;

  DCHECK(fragment->GetFirstLetterPseudoElement());
  LayoutObject* pseudo_element_layout_object =
      fragment->GetFirstLetterPseudoElement()->GetLayoutObject();
  return pseudo_element_layout_object &&
         pseudo_element_layout_object->Style()->Visibility() ==
             EVisibility::kVisible;
}

void TextIteratorTextNodeHandler::HandlePreFormattedTextNode() {
  // TODO(xiaochengh): Get rid of repeated computation of these fields.
  LayoutText* const layout_object = text_node_->GetLayoutObject();
  const String str = layout_object->GetText();

  needs_handle_pre_formatted_text_node_ = false;

  if (last_text_node_ended_with_collapsed_space_ &&
      HasVisibleTextNode(layout_object)) {
    if (!behavior_.CollapseTrailingSpace() ||
        (offset_ > 0 && str[offset_ - 1] == ' ')) {
      SpliceBuffer(kSpaceCharacter, text_node_, nullptr, offset_, offset_);
      needs_handle_pre_formatted_text_node_ = true;
      return;
    }
  }
  if (ShouldHandleFirstLetter(*layout_object)) {
    HandleTextNodeFirstLetter(ToLayoutTextFragment(layout_object));
    if (first_letter_text_) {
      const String first_letter = first_letter_text_->GetText();
      const unsigned run_start = offset_;
      const bool stops_in_first_letter = end_offset_ <= first_letter.length();
      const unsigned run_end =
          stops_in_first_letter ? end_offset_ : first_letter.length();
      EmitText(text_node_, first_letter_text_, run_start, run_end);
      first_letter_text_ = nullptr;
      text_box_ = nullptr;
      offset_ = run_end;
      if (!stops_in_first_letter)
        needs_handle_pre_formatted_text_node_ = true;
      return;
    }
    // We are here only if the DOM and/or layout trees are broken.
    // For robustness, we should stop processing this node.
    NOTREACHED();
    return;
  }
  if (layout_object->Style()->Visibility() != EVisibility::kVisible &&
      !IgnoresStyleVisibility())
    return;
  DCHECK_GE(offset_, layout_object->TextStartOffset());
  const unsigned run_start = offset_ - layout_object->TextStartOffset();
  const unsigned str_length = str.length();
  const unsigned end = end_offset_ - layout_object->TextStartOffset();
  const unsigned run_end = std::min(str_length, end);

  if (run_start >= run_end)
    return;

  EmitText(text_node_, text_node_->GetLayoutObject(), run_start, run_end);
}

void TextIteratorTextNodeHandler::HandleTextNodeInRange(Text* node,
                                                        unsigned start_offset,
                                                        unsigned end_offset) {
  DCHECK(node);

  // TODO(editing-dev): Stop passing in |start_offset == end_offset|.
  DCHECK_LE(start_offset, end_offset);

  text_node_ = node;
  offset_ = start_offset;
  end_offset_ = end_offset;
  handled_first_letter_ = false;
  first_letter_text_ = nullptr;
  uses_layout_ng_ = false;

  if (NGOffsetMapping::GetFor(Position(node, offset_))) {
    // Restore end offset from magic value.
    if (end_offset_ == kMaxOffset)
      end_offset_ = node->data().length();
    uses_layout_ng_ = true;
    HandleTextNodeWithLayoutNG();
    return;
  }

  LayoutText* layout_object = text_node_->GetLayoutObject();
  String str = layout_object->GetText();

  // Restore end offset from magic value.
  if (end_offset_ == kMaxOffset)
    end_offset_ = layout_object->TextStartOffset() + str.length();

  // handle pre-formatted text
  if (!layout_object->Style()->CollapseWhiteSpace()) {
    HandlePreFormattedTextNode();
    return;
  }

  if (layout_object->FirstTextBox())
    text_box_ = layout_object->FirstTextBox();

  const bool should_handle_first_letter =
      ShouldHandleFirstLetter(*layout_object);
  if (should_handle_first_letter)
    HandleTextNodeFirstLetter(ToLayoutTextFragment(layout_object));

  if (!layout_object->FirstTextBox() && str.length() > 0 &&
      !should_handle_first_letter) {
    if (layout_object->Style()->Visibility() == EVisibility::kVisible ||
        IgnoresStyleVisibility()) {
      last_text_node_ended_with_collapsed_space_ =
          true;  // entire block is collapsed space
    }
    return;
  }

  if (first_letter_text_)
    layout_object = first_letter_text_;

  // Used when text boxes are out of order (Hebrew/Arabic w/ embeded LTR text)
  if (layout_object->ContainsReversedText()) {
    sorted_text_boxes_.clear();
    for (InlineTextBox* text_box : InlineTextBoxesOf(*layout_object)) {
      sorted_text_boxes_.push_back(text_box);
    }
    std::sort(sorted_text_boxes_.begin(), sorted_text_boxes_.end(),
              InlineTextBox::CompareByStart);
    sorted_text_boxes_position_ = 0;
    text_box_ = sorted_text_boxes_.IsEmpty() ? 0 : sorted_text_boxes_[0];
  }

  HandleTextBox();
}

void TextIteratorTextNodeHandler::HandleTextNodeStartFrom(
    Text* node,
    unsigned start_offset) {
  HandleTextNodeInRange(node, start_offset, kMaxOffset);
}

void TextIteratorTextNodeHandler::HandleTextNodeEndAt(Text* node,
                                                      unsigned end_offset) {
  HandleTextNodeInRange(node, 0, end_offset);
}

void TextIteratorTextNodeHandler::HandleTextNodeWhole(Text* node) {
  HandleTextNodeStartFrom(node, 0);
}

// Restore the collapsed space for copy & paste. See http://crbug.com/318925
size_t TextIteratorTextNodeHandler::RestoreCollapsedTrailingSpace(
    InlineTextBox* next_text_box,
    size_t subrun_end) {
  if (next_text_box || !text_box_->Root().NextRootBox() ||
      text_box_->Root().LastChild() != text_box_)
    return subrun_end;

  const LayoutText* layout_object =
      first_letter_text_ ? first_letter_text_ : text_node_->GetLayoutObject();
  const String& text = layout_object->GetText();
  if (text.EndsWith(' ') == 0 || subrun_end != text.length() - 1 ||
      text[subrun_end - 1] == ' ')
    return subrun_end;

  // If there is the leading space in the next line, we don't need to restore
  // the trailing space.
  // Example: <div style="width: 2em;"><b><i>foo </i></b> bar</div>
  InlineBox* first_box_of_next_line =
      text_box_->Root().NextRootBox()->FirstChild();
  if (!first_box_of_next_line)
    return subrun_end + 1;
  Node* first_node_of_next_line =
      first_box_of_next_line->GetLineLayoutItem().GetNode();
  if (!first_node_of_next_line ||
      first_node_of_next_line->nodeValue()[0] != ' ')
    return subrun_end + 1;

  return subrun_end;
}

void TextIteratorTextNodeHandler::HandleTextBox() {
  LayoutText* layout_object =
      first_letter_text_ ? first_letter_text_ : text_node_->GetLayoutObject();
  const unsigned text_start_offset = layout_object->TextStartOffset();

  if (layout_object->Style()->Visibility() != EVisibility::kVisible &&
      !IgnoresStyleVisibility()) {
    text_box_ = nullptr;
  } else {
    String str = layout_object->GetText();
    // Start and end offsets in |str|, i.e., str[start..end - 1] should be
    // emitted (after handling whitespace collapsing).
    const unsigned start = offset_ - layout_object->TextStartOffset();
    const unsigned end = end_offset_ - text_start_offset;
    while (text_box_) {
      const unsigned text_box_start = text_box_->Start();
      const unsigned run_start = std::max(text_box_start, start);

      // Check for collapsed space at the start of this run.
      InlineTextBox* first_text_box =
          layout_object->ContainsReversedText()
              ? (sorted_text_boxes_.IsEmpty() ? 0 : sorted_text_boxes_[0])
              : layout_object->FirstTextBox();
      const bool need_space = last_text_node_ended_with_collapsed_space_ ||
                              (text_box_ == first_text_box &&
                               text_box_start == run_start && run_start > 0);
      if (need_space &&
          !layout_object->Style()->IsCollapsibleWhiteSpace(
              text_state_.LastCharacter()) &&
          text_state_.LastCharacter()) {
        if (run_start > 0 && str[run_start - 1] == ' ') {
          unsigned space_run_start = run_start - 1;
          while (space_run_start > 0 && str[space_run_start - 1] == ' ')
            --space_run_start;
          EmitText(text_node_, layout_object, space_run_start,
                   space_run_start + 1);
        } else {
          SpliceBuffer(kSpaceCharacter, text_node_, nullptr, run_start,
                       run_start);
        }
        return;
      }
      const unsigned text_box_end = text_box_start + text_box_->Len();
      const unsigned run_end = std::min(text_box_end, end);

      // Determine what the next text box will be, but don't advance yet
      InlineTextBox* next_text_box = nullptr;
      if (layout_object->ContainsReversedText()) {
        if (sorted_text_boxes_position_ + 1 < sorted_text_boxes_.size())
          next_text_box = sorted_text_boxes_[sorted_text_boxes_position_ + 1];
      } else {
        next_text_box = text_box_->NextTextBox();
      }

      // FIXME: Based on the outcome of crbug.com/446502 it's possible we can
      //   remove this block. The reason we new it now is because BIDI and
      //   FirstLetter seem to have different ideas of where things can split.
      //   FirstLetter takes the punctuation + first letter, and BIDI will
      //   split out the punctuation and possibly reorder it.
      if (next_text_box &&
          !(next_text_box->GetLineLayoutItem().IsEqual(layout_object))) {
        text_box_ = nullptr;
        return;
      }
      DCHECK(!next_text_box ||
             next_text_box->GetLineLayoutItem().IsEqual(layout_object));

      if (run_start < run_end) {
        // Handle either a single newline character (which becomes a space),
        // or a run of characters that does not include a newline.
        // This effectively translates newlines to spaces without copying the
        // text.
        if (str[run_start] == '\n') {
          // We need to preserve new lines in case of PreLine.
          // See bug crbug.com/317365.
          if (layout_object->Style()->WhiteSpace() == EWhiteSpace::kPreLine) {
            SpliceBuffer('\n', text_node_, nullptr, run_start, run_start);
          } else {
            SpliceBuffer(kSpaceCharacter, text_node_, nullptr, run_start,
                         run_start + 1);
          }
          offset_ = text_start_offset + run_start + 1;
        } else {
          size_t subrun_end = str.find('\n', run_start);
          if (subrun_end == kNotFound || subrun_end > run_end) {
            subrun_end = run_end;
            subrun_end =
                RestoreCollapsedTrailingSpace(next_text_box, subrun_end);
          }

          offset_ = text_start_offset + subrun_end;
          EmitText(text_node_, layout_object, run_start, subrun_end);
        }

        // If we are doing a subrun that doesn't go to the end of the text box,
        // come back again to finish handling this text box; don't advance to
        // the next one.
        if (text_state_.PositionEndOffset() < text_box_end + text_start_offset)
          return;

        if (behavior_.DoesNotEmitSpaceBeyondRangeEnd()) {
          // If the subrun went to the text box end and this end is also the end
          // of the range, do not advance to the next text box and do not
          // generate a space, just stop.
          if (text_box_end == end) {
            text_box_ = nullptr;
            return;
          }
        }

        // Advance and return
        const unsigned next_run_start =
            next_text_box ? next_text_box->Start() : str.length();
        if (next_run_start > run_end) {
          last_text_node_ended_with_collapsed_space_ =
              true;  // collapsed space between runs or at the end
        }

        text_box_ = next_text_box;
        if (layout_object->ContainsReversedText())
          ++sorted_text_boxes_position_;
        return;
      }
      // Advance and continue
      text_box_ = next_text_box;
      if (layout_object->ContainsReversedText())
        ++sorted_text_boxes_position_;
    }
  }

  if (ShouldProceedToRemainingText()) {
    ProceedToRemainingText();
    HandleTextBox();
  }
}

bool TextIteratorTextNodeHandler::ShouldProceedToRemainingText() const {
  if (text_box_ || !remaining_text_box_)
    return false;
  return offset_ < end_offset_;
}

void TextIteratorTextNodeHandler::ProceedToRemainingText() {
  text_box_ = remaining_text_box_;
  remaining_text_box_ = nullptr;
  first_letter_text_ = nullptr;
  offset_ = text_node_->GetLayoutObject()->TextStartOffset();
}

void TextIteratorTextNodeHandler::HandleTextNodeFirstLetter(
    LayoutTextFragment* layout_object) {
  handled_first_letter_ = true;

  if (!layout_object->IsRemainingTextLayoutObject())
    return;

  FirstLetterPseudoElement* first_letter_element =
      layout_object->GetFirstLetterPseudoElement();
  if (!first_letter_element)
    return;

  LayoutObject* pseudo_layout_object = first_letter_element->GetLayoutObject();
  if (pseudo_layout_object->Style()->Visibility() != EVisibility::kVisible &&
      !IgnoresStyleVisibility())
    return;

  LayoutObject* first_letter = pseudo_layout_object->SlowFirstChild();

  sorted_text_boxes_.clear();
  remaining_text_box_ = text_box_;
  CHECK(first_letter && first_letter->IsText());
  first_letter_text_ = ToLayoutText(first_letter);
  text_box_ = first_letter_text_->FirstTextBox();
}

bool TextIteratorTextNodeHandler::FixLeadingWhiteSpaceForReplacedElement(
    Node* parent) {
  // This is a hacky way for white space fixup in legacy layout. With LayoutNG,
  // we can get rid of this function.
  if (uses_layout_ng_)
    return false;

  if (behavior_.CollapseTrailingSpace()) {
    if (text_node_) {
      String str = text_node_->GetLayoutObject()->GetText();
      if (last_text_node_ended_with_collapsed_space_ && offset_ > 0 &&
          str[offset_ - 1] == ' ') {
        SpliceBuffer(kSpaceCharacter, parent, text_node_, 1, 1);
        return true;
      }
    }
  } else if (last_text_node_ended_with_collapsed_space_) {
    SpliceBuffer(kSpaceCharacter, parent, text_node_, 1, 1);
    return true;
  }

  return false;
}

void TextIteratorTextNodeHandler::ResetCollapsedWhiteSpaceFixup() {
  // This is a hacky way for white space fixup in legacy layout. With LayoutNG,
  // we can get rid of this function.
  last_text_node_ended_with_collapsed_space_ = false;
}

void TextIteratorTextNodeHandler::SpliceBuffer(UChar c,
                                               Node* text_node,
                                               Node* offset_base_node,
                                               unsigned text_start_offset,
                                               unsigned text_end_offset) {
  text_state_.SpliceBuffer(c, text_node, offset_base_node, text_start_offset,
                           text_end_offset);
  ResetCollapsedWhiteSpaceFixup();
}

void TextIteratorTextNodeHandler::EmitText(Node* text_node,
                                           LayoutText* layout_object,
                                           unsigned text_start_offset,
                                           unsigned text_end_offset) {
  String string = behavior_.EmitsOriginalText() ? layout_object->OriginalText()
                                                : layout_object->GetText();
  if (behavior_.EmitsSpaceForNbsp())
    string.Replace(kNoBreakSpaceCharacter, kSpaceCharacter);
  text_state_.EmitText(text_node,
                       text_start_offset + layout_object->TextStartOffset(),
                       text_end_offset + layout_object->TextStartOffset(),
                       string, text_start_offset, text_end_offset);
  ResetCollapsedWhiteSpaceFixup();
}

}  // namespace blink
