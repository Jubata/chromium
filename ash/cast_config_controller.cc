// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/cast_config_controller.h"

#include <utility>
#include <vector>

namespace ash {

CastConfigController::CastConfigController() : binding_(this) {}

CastConfigController::~CastConfigController() {}

bool CastConfigController::Connected() {
  return client_.is_bound();
}

void CastConfigController::AddObserver(CastConfigControllerObserver* observer) {
  observers_.AddObserver(observer);
}

void CastConfigController::RemoveObserver(
    CastConfigControllerObserver* observer) {
  observers_.RemoveObserver(observer);
}

void CastConfigController::BindRequest(mojom::CastConfigRequest request) {
  binding_.Bind(std::move(request));
}

void CastConfigController::SetClient(
    mojom::CastConfigClientAssociatedPtrInfo client) {
  client_.Bind(std::move(client));

  // If we added observers before we were connected to, run them now.
  if (observers_.might_have_observers())
    client_->RequestDeviceRefresh();
}

void CastConfigController::OnDevicesUpdated(
    std::vector<mojom::SinkAndRoutePtr> devices) {
  for (auto& observer : observers_) {
    std::vector<mojom::SinkAndRoutePtr> devices_copy;
    for (auto& item : devices)
      devices_copy.push_back(item.Clone());
    observer.OnDevicesUpdated(std::move(devices_copy));
  }
}

void CastConfigController::RequestDeviceRefresh() {
  if (client_)
    client_->RequestDeviceRefresh();
}

void CastConfigController::CastToSink(ash::mojom::CastSinkPtr sink) {
  if (client_)
    client_->CastToSink(std::move(sink));
}

void CastConfigController::StopCasting(ash::mojom::CastRoutePtr route) {
  if (client_)
    client_->StopCasting(std::move(route));
}

}  // namespace ash
