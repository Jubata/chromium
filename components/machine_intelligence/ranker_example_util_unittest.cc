// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/machine_intelligence/ranker_example_util.h"

#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace machine_intelligence {

using ::testing::ElementsAreArray;

class RankerExampleUtilTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto& features = *example_.mutable_features();
    features[bool_name_].set_bool_value(bool_value_);
    features[int32_name_].set_int32_value(int32_value_);
    features[float_name_].set_float_value(float_value_);
    features[one_hot_name_].set_string_value(one_hot_value_);
  }

  RankerExample example_;
  const std::string bool_name_ = "bool_feature";
  const bool bool_value_ = true;
  const std::string int32_name_ = "int32_feature";
  const int int32_value_ = 2;
  const std::string float_name_ = "float_feature";
  const float float_value_ = 3.0f;
  const std::string one_hot_name_ = "one_hot_feature";
  const std::string elem1_ = "elem1";
  const std::string elem2_ = "elem2";
  const std::string one_hot_value_ = elem1_;
  const float epsilon_ = 0.00000001f;
};

TEST_F(RankerExampleUtilTest, CheckFeature) {
  EXPECT_TRUE(SafeGetFeature(bool_name_, example_, nullptr));
  EXPECT_TRUE(SafeGetFeature(int32_name_, example_, nullptr));
  EXPECT_TRUE(SafeGetFeature(float_name_, example_, nullptr));
  EXPECT_TRUE(SafeGetFeature(one_hot_name_, example_, nullptr));
  EXPECT_FALSE(SafeGetFeature("", example_, nullptr));
  EXPECT_FALSE(SafeGetFeature("foo", example_, nullptr));
}

TEST_F(RankerExampleUtilTest, SafeGetFeature) {
  Feature feature;

  EXPECT_TRUE(SafeGetFeature(bool_name_, example_, &feature));
  EXPECT_TRUE(feature.bool_value());
  feature.Clear();

  EXPECT_TRUE(SafeGetFeature(int32_name_, example_, &feature));
  EXPECT_EQ(int32_value_, feature.int32_value());
  feature.Clear();

  EXPECT_TRUE(SafeGetFeature(float_name_, example_, &feature));
  EXPECT_NEAR(float_value_, feature.float_value(), epsilon_);
  feature.Clear();

  EXPECT_TRUE(SafeGetFeature(one_hot_name_, example_, &feature));
  EXPECT_EQ(one_hot_value_, feature.string_value());
  feature.Clear();

  EXPECT_FALSE(SafeGetFeature("", example_, &feature));
  EXPECT_FALSE(SafeGetFeature("foo", example_, &feature));
}

TEST_F(RankerExampleUtilTest, GetFeatureValueAsFloat) {
  float value;

  EXPECT_TRUE(GetFeatureValueAsFloat(bool_name_, example_, &value));
  EXPECT_NEAR(1.0f, value, epsilon_);

  EXPECT_TRUE(GetFeatureValueAsFloat(int32_name_, example_, &value));
  EXPECT_NEAR(2.0f, value, epsilon_);

  EXPECT_TRUE(GetFeatureValueAsFloat(float_name_, example_, &value));
  EXPECT_NEAR(3.0f, value, epsilon_);

  EXPECT_FALSE(GetFeatureValueAsFloat(one_hot_name_, example_, &value));
  // Value remains unchanged if GetFeatureValueAsFloat returns false.
  EXPECT_NEAR(3.0f, value, epsilon_);

  EXPECT_FALSE(GetFeatureValueAsFloat("", example_, &value));
  EXPECT_FALSE(GetFeatureValueAsFloat("foo", example_, &value));
}

TEST_F(RankerExampleUtilTest, GetOneHotValue) {
  std::string value;

  EXPECT_FALSE(GetOneHotValue(bool_name_, example_, &value));

  EXPECT_FALSE(GetOneHotValue(int32_name_, example_, &value));

  EXPECT_FALSE(GetOneHotValue(float_name_, example_, &value));

  EXPECT_TRUE(GetOneHotValue(one_hot_name_, example_, &value));
  EXPECT_EQ(one_hot_value_, value);

  EXPECT_FALSE(GetOneHotValue("", example_, &value));
  EXPECT_FALSE(GetOneHotValue("foo", example_, &value));
}

}  // namespace machine_intelligence
