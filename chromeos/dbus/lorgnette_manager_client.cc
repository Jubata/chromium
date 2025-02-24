// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/dbus/lorgnette_manager_client.h"

#include <memory>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback.h"
#include "base/files/scoped_file.h"
#include "base/location.h"
#include "base/macros.h"
#include "base/optional.h"
#include "base/task_scheduler/post_task.h"
#include "chromeos/dbus/pipe_reader.h"
#include "dbus/bus.h"
#include "dbus/message.h"
#include "dbus/object_path.h"
#include "dbus/object_proxy.h"
#include "third_party/cros_system_api/dbus/service_constants.h"

namespace chromeos {

// The LorgnetteManagerClient implementation used in production.
class LorgnetteManagerClientImpl : public LorgnetteManagerClient {
 public:
  LorgnetteManagerClientImpl() :
      lorgnette_daemon_proxy_(NULL), weak_ptr_factory_(this) {}

  ~LorgnetteManagerClientImpl() override {}

  void ListScanners(const ListScannersCallback& callback) override {
    dbus::MethodCall method_call(lorgnette::kManagerServiceInterface,
                                 lorgnette::kListScannersMethod);
    lorgnette_daemon_proxy_->CallMethod(
        &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
        base::BindOnce(&LorgnetteManagerClientImpl::OnListScanners,
                       weak_ptr_factory_.GetWeakPtr(), callback));
  }

  // LorgnetteManagerClient override.
  void ScanImageToString(
      std::string device_name,
      const ScanProperties& properties,
      const ScanImageToStringCallback& callback) override {
    auto scan_data_reader = std::make_unique<ScanDataReader>();
    base::ScopedFD fd = scan_data_reader->Start();

    // Issue the dbus request to scan an image.
    dbus::MethodCall method_call(lorgnette::kManagerServiceInterface,
                                 lorgnette::kScanImageMethod);
    dbus::MessageWriter writer(&method_call);
    writer.AppendString(device_name);
    writer.AppendFileDescriptor(fd.get());

    dbus::MessageWriter option_writer(NULL);
    dbus::MessageWriter element_writer(NULL);
    writer.OpenArray("{sv}", &option_writer);
    if (!properties.mode.empty()) {
      option_writer.OpenDictEntry(&element_writer);
      element_writer.AppendString(lorgnette::kScanPropertyMode);
      element_writer.AppendVariantOfString(properties.mode);
      option_writer.CloseContainer(&element_writer);
    }
    if (properties.resolution_dpi) {
      option_writer.OpenDictEntry(&element_writer);
      element_writer.AppendString(lorgnette::kScanPropertyResolution);
      element_writer.AppendVariantOfUint32(properties.resolution_dpi);
      option_writer.CloseContainer(&element_writer);
    }
    writer.CloseContainer(&option_writer);

    lorgnette_daemon_proxy_->CallMethod(
        &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
        base::BindOnce(&LorgnetteManagerClientImpl::OnScanImageComplete,
                       weak_ptr_factory_.GetWeakPtr(), callback,
                       std::move(scan_data_reader)));
  }

 protected:
  void Init(dbus::Bus* bus) override {
    lorgnette_daemon_proxy_ =
        bus->GetObjectProxy(lorgnette::kManagerServiceName,
                            dbus::ObjectPath(lorgnette::kManagerServicePath));
  }

 private:
  // Reads scan data on a blocking sequence.
  class ScanDataReader {
   public:
    // In case of success, std::string holds the read data. Otherwise,
    // nullopt.
    using CompletionCallback =
        base::OnceCallback<void(base::Optional<std::string> data)>;

    ScanDataReader() : weak_ptr_factory_(this) {}

    // Creates a pipe to read the scan data from the D-Bus service.
    // Returns a write-side FD.
    base::ScopedFD Start() {
      DCHECK(!pipe_reader_.get());
      DCHECK(!data_.has_value());
      pipe_reader_ = std::make_unique<chromeos::PipeReader>(
          base::CreateTaskRunnerWithTraits(
              {base::MayBlock(),
               base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN}));

      return pipe_reader_->StartIO(base::BindOnce(
          &ScanDataReader::OnDataRead, weak_ptr_factory_.GetWeakPtr()));
    }

    // Waits for the data read completion. If it is already done, |callback|
    // will be called synchronously.
    void Wait(CompletionCallback callback) {
      DCHECK(callback_.is_null());
      callback_ = std::move(callback);
      MaybeCompleted();
    }

   private:
    // Called when a |pipe_reader_| completes reading scan data to a string.
    void OnDataRead(base::Optional<std::string> data) {
      DCHECK(!data_read_);
      data_read_ = true;
      data_ = std::move(data);
      pipe_reader_.reset();
      MaybeCompleted();
    }

    void MaybeCompleted() {
      // If data reading is not yet completed, or D-Bus call does not yet
      // return, wait for the other.
      if (!data_read_ || callback_.is_null())
        return;

      std::move(callback_).Run(std::move(data_));
    }

    std::unique_ptr<chromeos::PipeReader> pipe_reader_;

    // Set to true on data read completion.
    bool data_read_ = false;

    // Available only when |data_read_| is true.
    base::Optional<std::string> data_;

    CompletionCallback callback_;

    base::WeakPtrFactory<ScanDataReader> weak_ptr_factory_;
    DISALLOW_COPY_AND_ASSIGN(ScanDataReader);
  };

  // Called when ListScanners completes.
  void OnListScanners(const ListScannersCallback& callback,
                      dbus::Response* response) {
    ScannerTable scanners;
    dbus::MessageReader table_reader(NULL);
    if (!response || !dbus::MessageReader(response).PopArray(&table_reader)) {
      callback.Run(false, scanners);
      return;
    }

    bool decode_failure = false;
    while (table_reader.HasMoreData()) {
      std::string device_name;
      dbus::MessageReader device_entry_reader(NULL);
      dbus::MessageReader device_element_reader(NULL);
      if (!table_reader.PopDictEntry(&device_entry_reader) ||
          !device_entry_reader.PopString(&device_name) ||
          !device_entry_reader.PopArray(&device_element_reader)) {
        decode_failure = true;
        break;
      }

      ScannerTableEntry scanner_entry;
      while (device_element_reader.HasMoreData()) {
        dbus::MessageReader device_attribute_reader(NULL);
        std::string attribute;
        std::string value;
        if (!device_element_reader.PopDictEntry(&device_attribute_reader) ||
            !device_attribute_reader.PopString(&attribute) ||
            !device_attribute_reader.PopString(&value)) {
          decode_failure = true;
          break;
        }
        scanner_entry[attribute] = value;
      }

      if (decode_failure)
          break;

      scanners[device_name] = scanner_entry;
    }

    if (decode_failure) {
      LOG(ERROR) << "Failed to decode response from ListScanners";
      callback.Run(false, scanners);
    } else {
      callback.Run(true, scanners);
    }
  }

  // Called when a response for ScanImage() is received.
  void OnScanImageComplete(const ScanImageToStringCallback& callback,
                           std::unique_ptr<ScanDataReader> scan_data_reader,
                           dbus::Response* response) {
    if (!response) {
      LOG(ERROR) << "Failed to scan image";
      // Do not touch |scan_data_reader|, so that RAII deletes it and
      // cancels the inflight operation.
      callback.Run(false, std::string());
      return;
    }
    auto* reader = scan_data_reader.get();
    reader->Wait(base::BindOnce(
        &LorgnetteManagerClientImpl::OnScanDataCompleted,
        weak_ptr_factory_.GetWeakPtr(), callback, std::move(scan_data_reader)));
  }

  // Called when scan data read is completed.
  void OnScanDataCompleted(const ScanImageToStringCallback& callback,
                           std::unique_ptr<ScanDataReader> scan_data_reader,
                           base::Optional<std::string> data) {
    callback.Run(data.has_value(), data.value_or(std::string()));
  }

  dbus::ObjectProxy* lorgnette_daemon_proxy_;
  base::WeakPtrFactory<LorgnetteManagerClientImpl> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(LorgnetteManagerClientImpl);
};

LorgnetteManagerClient::LorgnetteManagerClient() {
}

LorgnetteManagerClient::~LorgnetteManagerClient() {
}

// static
LorgnetteManagerClient* LorgnetteManagerClient::Create() {
  return new LorgnetteManagerClientImpl();
}

}  // namespace chromeos
