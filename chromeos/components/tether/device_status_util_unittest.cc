// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/components/tether/device_status_util.h"

#include <memory>

#include "base/bind.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "chromeos/components/tether/fake_tether_host_fetcher.h"
#include "chromeos/components/tether/proto_test_util.h"
#include "components/cryptauth/remote_device.h"
#include "components/cryptauth/remote_device_test_util.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromeos {

namespace tether {

class DeviceStatusUtilTest : public testing::Test {
 public:
  DeviceStatusUtilTest() {}

 private:
  DISALLOW_COPY_AND_ASSIGN(DeviceStatusUtilTest);
};

TEST_F(DeviceStatusUtilTest, TestNotPresent) {
  DeviceStatus status = CreateTestDeviceStatus(
      proto_test_util::kDoNotSetStringField /* cell_provider_name */,
      proto_test_util::kDoNotSetIntField /* battery_percentage */,
      proto_test_util::kDoNotSetIntField /* connection_strength */);

  std::string carrier;
  int32_t battery_percentage;
  int32_t signal_strength;

  NormalizeDeviceStatus(status, &carrier, &battery_percentage,
                        &signal_strength);

  EXPECT_EQ("unknown-carrier", carrier);
  EXPECT_EQ(100, battery_percentage);
  EXPECT_EQ(100, signal_strength);
}

TEST_F(DeviceStatusUtilTest, TestEmptyCellProvider) {
  DeviceStatus status = CreateTestDeviceStatus(
      "" /* cell_provider_name */,
      proto_test_util::kDoNotSetIntField /* battery_percentage */,
      proto_test_util::kDoNotSetIntField /* connection_strength */);

  std::string carrier;
  int32_t battery_percentage;
  int32_t signal_strength;

  NormalizeDeviceStatus(status, &carrier, &battery_percentage,
                        &signal_strength);

  EXPECT_EQ("unknown-carrier", carrier);
  EXPECT_EQ(100, battery_percentage);
  EXPECT_EQ(100, signal_strength);
}

TEST_F(DeviceStatusUtilTest, TestBelowMinValue) {
  DeviceStatus status = CreateTestDeviceStatus(
      "cellProvider" /* cell_provider_name */, -1 /* battery_percentage */,
      -1 /* connection_strength */);

  std::string carrier;
  int32_t battery_percentage;
  int32_t signal_strength;

  NormalizeDeviceStatus(status, &carrier, &battery_percentage,
                        &signal_strength);

  EXPECT_EQ("cellProvider", carrier);
  EXPECT_EQ(0, battery_percentage);
  EXPECT_EQ(0, signal_strength);
}

TEST_F(DeviceStatusUtilTest, TestAboveMaxValue) {
  DeviceStatus status = CreateTestDeviceStatus(
      "cellProvider" /* cell_provider_name */, 101 /* battery_percentage */,
      5 /* connection_strength */);

  std::string carrier;
  int32_t battery_percentage;
  int32_t signal_strength;

  NormalizeDeviceStatus(status, &carrier, &battery_percentage,
                        &signal_strength);

  EXPECT_EQ("cellProvider", carrier);
  EXPECT_EQ(100, battery_percentage);
  EXPECT_EQ(100, signal_strength);
}

TEST_F(DeviceStatusUtilTest, TestValidValues) {
  DeviceStatus status = CreateTestDeviceStatus(
      "cellProvider" /* cell_provider_name */, 50 /* battery_percentage */,
      2 /* connection_strength */);

  std::string carrier;
  int32_t battery_percentage;
  int32_t signal_strength;

  NormalizeDeviceStatus(status, &carrier, &battery_percentage,
                        &signal_strength);

  EXPECT_EQ("cellProvider", carrier);
  EXPECT_EQ(50, battery_percentage);
  EXPECT_EQ(50, signal_strength);
}

}  // namespace tether

}  // namespace cryptauth
