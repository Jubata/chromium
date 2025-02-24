// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/machine_intelligence/fake_ranker_model_loader.h"

namespace machine_intelligence {

namespace testing {

FakeRankerModelLoader::FakeRankerModelLoader(
    ValidateModelCallback validate_model_cb,
    OnModelAvailableCallback on_model_available_cb,
    std::unique_ptr<RankerModel> ranker_model)
    : ranker_model_(std::move(ranker_model)),
      validate_model_cb_(std::move(validate_model_cb)),
      on_model_available_cb_(std::move(on_model_available_cb)) {}

FakeRankerModelLoader::~FakeRankerModelLoader() {}

void FakeRankerModelLoader::NotifyOfRankerActivity() {
  if (validate_model_cb_.Run(*ranker_model_.get()) == RankerModelStatus::OK) {
    on_model_available_cb_.Run(std::move(ranker_model_));
  }
}

}  // namespace testing

}  // namespace machine_intelligence
