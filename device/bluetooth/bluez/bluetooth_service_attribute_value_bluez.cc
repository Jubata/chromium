// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/bluetooth/bluez/bluetooth_service_attribute_value_bluez.h"

#include <utility>

#include "base/logging.h"
#include "base/memory/ptr_util.h"

namespace bluez {

BluetoothServiceAttributeValueBlueZ::BluetoothServiceAttributeValueBlueZ()
    : type_(NULLTYPE), size_(0), value_(std::make_unique<base::Value>()) {}

BluetoothServiceAttributeValueBlueZ::BluetoothServiceAttributeValueBlueZ(
    Type type,
    size_t size,
    std::unique_ptr<base::Value> value)
    : type_(type), size_(size), value_(std::move(value)) {
  CHECK_NE(type, SEQUENCE);
}

BluetoothServiceAttributeValueBlueZ::BluetoothServiceAttributeValueBlueZ(
    std::unique_ptr<Sequence> sequence)
    : type_(SEQUENCE),
      size_(sequence->size()),
      sequence_(std::move(sequence)) {}

BluetoothServiceAttributeValueBlueZ::BluetoothServiceAttributeValueBlueZ(
    const BluetoothServiceAttributeValueBlueZ& attribute) {
  *this = attribute;
}

BluetoothServiceAttributeValueBlueZ& BluetoothServiceAttributeValueBlueZ::
operator=(const BluetoothServiceAttributeValueBlueZ& attribute) {
  if (this != &attribute) {
    type_ = attribute.type_;
    size_ = attribute.size_;
    if (attribute.type_ == SEQUENCE) {
      value_ = nullptr;
      sequence_ = std::make_unique<Sequence>(*attribute.sequence_);
    } else {
      value_ = attribute.value_->CreateDeepCopy();
      sequence_ = nullptr;
    }
  }
  return *this;
}

BluetoothServiceAttributeValueBlueZ::~BluetoothServiceAttributeValueBlueZ() {}

}  // namespace bluez
