// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/app_list/model/search/tokenized_string_char_iterator.h"

#include "base/i18n/char_iterator.h"
#include "base/logging.h"
#include "third_party/icu/source/common/unicode/utf16.h"

namespace app_list {

TokenizedStringCharIterator::State::State() : token_index(0u), char_index(0) {}

TokenizedStringCharIterator::State::State(size_t token_index, int char_index)
    : token_index(token_index), char_index(char_index) {}

TokenizedStringCharIterator::TokenizedStringCharIterator(
    const TokenizedString& tokenized)
    : tokens_(tokenized.tokens()),
      mappings_(tokenized.mappings()),
      current_token_(0) {
  CreateTokenCharIterator();
}

TokenizedStringCharIterator::~TokenizedStringCharIterator() {}

bool TokenizedStringCharIterator::NextChar() {
  if (current_token_iter_) {
    current_token_iter_->Advance();
    if (!current_token_iter_->end())
      return true;
  }

  return NextToken();
}

bool TokenizedStringCharIterator::NextToken() {
  if (current_token_ < tokens_.size()) {
    ++current_token_;
    CreateTokenCharIterator();
  }

  return !!current_token_iter_;
}

int32_t TokenizedStringCharIterator::Get() const {
  return current_token_iter_ ? current_token_iter_->get() : 0;
}

int32_t TokenizedStringCharIterator::GetArrayPos() const {
  DCHECK(current_token_iter_);
  return mappings_[current_token_].start() + current_token_iter_->array_pos();
}

size_t TokenizedStringCharIterator::GetCharSize() const {
  return current_token_iter_ ? U16_LENGTH(Get()) : 0;
}

bool TokenizedStringCharIterator::IsFirstCharOfToken() const {
  return current_token_iter_ && current_token_iter_->char_pos() == 0;
}

TokenizedStringCharIterator::State TokenizedStringCharIterator::GetState()
    const {
  return State(current_token_,
               current_token_iter_ ? current_token_iter_->char_pos() : 0);
}

void TokenizedStringCharIterator::SetState(const State& state) {
  current_token_ = state.token_index;
  CreateTokenCharIterator();
  if (current_token_iter_) {
    while (current_token_iter_->char_pos() < state.char_index)
      current_token_iter_->Advance();
  }
}

void TokenizedStringCharIterator::CreateTokenCharIterator() {
  if (current_token_ == tokens_.size()) {
    current_token_iter_.reset();
    return;
  }

  current_token_iter_.reset(
      new base::i18n::UTF16CharIterator(&tokens_[current_token_]));
}

}  // namespace app_list
