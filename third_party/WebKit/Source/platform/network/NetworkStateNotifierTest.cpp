/*
 * Copyright (c) 2014, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "platform/network/NetworkStateNotifier.h"

#include "platform/scheduler/test/fake_web_task_runner.h"
#include "platform/testing/UnitTestHelpers.h"
#include "platform/wtf/Functional.h"
#include "platform/wtf/Optional.h"
#include "platform/wtf/Time.h"
#include "public/platform/Platform.h"
#include "public/platform/WebConnectionType.h"
#include "public/platform/WebEffectiveConnectionType.h"
#include "public/platform/WebThread.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace blink {

using scheduler::FakeWebTaskRunner;

namespace {
const double kNoneMaxBandwidthMbps = 0.0;
const double kBluetoothMaxBandwidthMbps = 1.0;
const double kEthernetMaxBandwidthMbps = 2.0;
const Optional<TimeDelta> kEthernetHttpRtt(TimeDelta::FromMilliseconds(50));
const Optional<TimeDelta> kEthernetTransportRtt(
    TimeDelta::FromMilliseconds(25));
const Optional<double> kEthernetThroughputMbps(75.0);
const Optional<TimeDelta> kUnknownRtt;
const Optional<double> kUnknownThroughputMbps;

enum class SaveData {
  kOff = 0,
  kOn = 1,
};

}  // namespace

class StateObserver : public NetworkStateNotifier::NetworkStateObserver {
 public:
  StateObserver()
      : observed_type_(kWebConnectionTypeNone),
        observed_max_bandwidth_mbps_(0.0),
        observed_effective_type_(WebEffectiveConnectionType::kTypeUnknown),
        observed_http_rtt_(kUnknownRtt),
        observed_transport_rtt_(kUnknownRtt),
        observed_downlink_throughput_mbps_(kUnknownThroughputMbps),
        observed_on_line_state_(false),
        observed_save_data_(SaveData::kOff),
        callback_count_(0) {}

  virtual void ConnectionChange(
      WebConnectionType type,
      double max_bandwidth_mbps,
      WebEffectiveConnectionType effective_type,
      const Optional<TimeDelta>& http_rtt,
      const Optional<TimeDelta>& transport_rtt,
      const Optional<double>& downlink_throughput_mbps,
      bool save_data) {
    observed_type_ = type;
    observed_max_bandwidth_mbps_ = max_bandwidth_mbps;
    observed_effective_type_ = effective_type;
    observed_http_rtt_ = http_rtt;
    observed_transport_rtt_ = transport_rtt;
    observed_downlink_throughput_mbps_ = downlink_throughput_mbps;
    observed_save_data_ = save_data ? SaveData::kOn : SaveData::kOff;
    callback_count_ += 1;

    if (closure_)
      std::move(closure_).Run();
  }

  virtual void OnLineStateChange(bool on_line) {
    observed_on_line_state_ = on_line;
    callback_count_ += 1;

    if (closure_)
      std::move(closure_).Run();
  }

  WebConnectionType ObservedType() const { return observed_type_; }
  double ObservedMaxBandwidth() const { return observed_max_bandwidth_mbps_; }
  WebEffectiveConnectionType ObservedEffectiveType() const {
    return observed_effective_type_;
  }
  Optional<TimeDelta> ObservedHttpRtt() const { return observed_http_rtt_; }
  Optional<TimeDelta> ObservedTransportRtt() const {
    return observed_transport_rtt_;
  }
  Optional<double> ObservedDownlinkThroughputMbps() const {
    return observed_downlink_throughput_mbps_;
  }
  bool ObservedOnLineState() const { return observed_on_line_state_; }
  SaveData ObservedSaveData() const { return observed_save_data_; }
  int CallbackCount() const { return callback_count_; }

  void AddObserverOnNotification(NetworkStateNotifier* notifier,
                                 StateObserver* observer_to_add,
                                 scoped_refptr<WebTaskRunner> task_runner) {
    closure_ = base::BindOnce(
        [](StateObserver* observer, NetworkStateNotifier* notifier,
           StateObserver* observer_to_add,
           scoped_refptr<WebTaskRunner> task_runner) {
          observer->added_handle_ =
              notifier->AddConnectionObserver(observer_to_add, task_runner);
        },
        base::Unretained(this), base::Unretained(notifier),
        base::Unretained(observer_to_add), task_runner);
  }

  void RemoveObserverOnNotification(
      std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle>
          handle) {
    closure_ = base::BindOnce(
        [](std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle>
               handle) {},
        std::move(handle));
  }

 private:
  base::OnceClosure closure_;
  WebConnectionType observed_type_;
  double observed_max_bandwidth_mbps_;
  WebEffectiveConnectionType observed_effective_type_;
  Optional<TimeDelta> observed_http_rtt_;
  Optional<TimeDelta> observed_transport_rtt_;
  Optional<double> observed_downlink_throughput_mbps_;
  bool observed_on_line_state_;
  SaveData observed_save_data_;
  int callback_count_;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle>
      added_handle_;
};

class NetworkStateNotifierTest : public ::testing::Test {
 public:
  NetworkStateNotifierTest()
      : task_runner_(base::AdoptRef(new FakeWebTaskRunner())),
        task_runner2_(base::AdoptRef(new FakeWebTaskRunner())) {
    // Initialize connection, so that future calls to setWebConnection issue
    // notifications.
    notifier_.SetWebConnection(kWebConnectionTypeUnknown, 0.0);
    notifier_.SetOnLine(false);
  }

  WebTaskRunner* GetTaskRunner() { return task_runner_.get(); }
  WebTaskRunner* GetTaskRunner2() { return task_runner2_.get(); }

  void TearDown() override {
    RunPendingTasks();
    task_runner_ = nullptr;
    task_runner2_ = nullptr;
  }

 protected:
  void RunPendingTasks() {
    task_runner_->RunUntilIdle();
    task_runner2_->RunUntilIdle();
  }

  void SetConnection(WebConnectionType type,
                     double max_bandwidth_mbps,
                     WebEffectiveConnectionType effective_type,
                     const Optional<TimeDelta>& http_rtt,
                     const Optional<TimeDelta>& transport_rtt,
                     const Optional<double>& downlink_throughput_mbps,
                     SaveData save_data) {
    notifier_.SetWebConnection(type, max_bandwidth_mbps);
    notifier_.SetNetworkQuality(
        effective_type,
        http_rtt.has_value() ? http_rtt.value()
                             : base::TimeDelta::FromMilliseconds(-1),
        transport_rtt.has_value() ? transport_rtt.value()
                                  : base::TimeDelta::FromMilliseconds(-1),
        downlink_throughput_mbps.has_value()
            ? downlink_throughput_mbps.value() * 1000
            : -1);
    notifier_.SetSaveDataEnabled(save_data == SaveData::kOn);
    RunPendingTasks();
  }
  void SetOnLine(bool on_line) {
    notifier_.SetOnLine(on_line);
    RunPendingTasks();
  }

  bool VerifyObservations(const StateObserver& observer,
                          WebConnectionType type,
                          double max_bandwidth_mbps,
                          WebEffectiveConnectionType effective_type,
                          const Optional<TimeDelta>& http_rtt,
                          const Optional<TimeDelta>& transport_rtt,
                          const Optional<double>& downlink_throughput_mbps,
                          SaveData save_data) const {
    EXPECT_EQ(type, observer.ObservedType());
    EXPECT_EQ(max_bandwidth_mbps, observer.ObservedMaxBandwidth());
    EXPECT_EQ(effective_type, observer.ObservedEffectiveType());
    EXPECT_EQ(http_rtt, observer.ObservedHttpRtt());
    EXPECT_EQ(transport_rtt, observer.ObservedTransportRtt());
    EXPECT_EQ(downlink_throughput_mbps,
              observer.ObservedDownlinkThroughputMbps());
    EXPECT_EQ(save_data, observer.ObservedSaveData());

    return observer.ObservedType() == type &&
           observer.ObservedMaxBandwidth() == max_bandwidth_mbps &&
           observer.ObservedEffectiveType() == effective_type &&
           observer.ObservedHttpRtt() == http_rtt &&
           observer.ObservedTransportRtt() == transport_rtt &&
           observer.ObservedDownlinkThroughputMbps() ==
               downlink_throughput_mbps &&
           observer.ObservedSaveData() == save_data;
  }

  scoped_refptr<FakeWebTaskRunner> task_runner_;
  scoped_refptr<FakeWebTaskRunner> task_runner2_;
  NetworkStateNotifier notifier_;
};

TEST_F(NetworkStateNotifierTest, AddObserver) {
  StateObserver observer;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle =
      notifier_.AddConnectionObserver(&observer, GetTaskRunner());
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeNone, kNoneMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));

  // Change max. bandwidth and the network quality estimates.
  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
      kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff));
  EXPECT_EQ(observer.CallbackCount(), 2);

  // Only change the connection type.
  SetConnection(kWebConnectionTypeEthernet, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeEthernet, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
      kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff));
  EXPECT_EQ(observer.CallbackCount(), 3);

  // Only change the max. bandwidth.
  SetConnection(kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
      kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff));
  EXPECT_EQ(observer.CallbackCount(), 4);

  // Only change the transport RTT.
  SetConnection(kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt.value() * 2, kEthernetThroughputMbps,
                SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
      kEthernetTransportRtt.value() * 2, kEthernetThroughputMbps,
      SaveData::kOff));
  EXPECT_EQ(observer.CallbackCount(), 5);

  // Only change the effective connection type.
  SetConnection(kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
                WebEffectiveConnectionType::kType4G, kEthernetHttpRtt,
                kEthernetTransportRtt.value() * 2, kEthernetThroughputMbps,
                SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
      WebEffectiveConnectionType::kType4G, kEthernetHttpRtt,
      kEthernetTransportRtt.value() * 2, kEthernetThroughputMbps,
      SaveData::kOff));
  EXPECT_EQ(observer.CallbackCount(), 6);

  // Only change the save data.
  SetConnection(kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
                WebEffectiveConnectionType::kType4G, kEthernetHttpRtt,
                kEthernetTransportRtt.value() * 2, kEthernetThroughputMbps,
                SaveData::kOn);
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
      WebEffectiveConnectionType::kType4G, kEthernetHttpRtt,
      kEthernetTransportRtt.value() * 2, kEthernetThroughputMbps,
      SaveData::kOn));
  EXPECT_EQ(observer.CallbackCount(), 7);
}

TEST_F(NetworkStateNotifierTest, RemoveObserver) {
  StateObserver observer1, observer2;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle1 =
      notifier_.AddConnectionObserver(&observer1, GetTaskRunner());
  handle1 = nullptr;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle2 =
      notifier_.AddConnectionObserver(&observer2, GetTaskRunner());

  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff);

  EXPECT_TRUE(VerifyObservations(
      observer1, kWebConnectionTypeNone, kNoneMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(VerifyObservations(
      observer2, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
      kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff));
}

TEST_F(NetworkStateNotifierTest, RemoveSoleObserver) {
  StateObserver observer1;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle =
      notifier_.AddConnectionObserver(&observer1, GetTaskRunner());
  handle = nullptr;

  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer1, kWebConnectionTypeNone, kNoneMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
}

TEST_F(NetworkStateNotifierTest, AddObserverWhileNotifying) {
  StateObserver observer1, observer2;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle =
      notifier_.AddConnectionObserver(&observer1, GetTaskRunner());
  observer1.AddObserverOnNotification(&notifier_, &observer2, GetTaskRunner());

  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer1, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(VerifyObservations(
      observer2, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
}

TEST_F(NetworkStateNotifierTest, RemoveSoleObserverWhileNotifying) {
  StateObserver observer1;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle =
      notifier_.AddConnectionObserver(&observer1, GetTaskRunner());
  observer1.RemoveObserverOnNotification(std::move(handle));

  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer1, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));

  SetConnection(kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer1, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
}

TEST_F(NetworkStateNotifierTest, RemoveCurrentObserverWhileNotifying) {
  StateObserver observer1, observer2;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle1 =
      notifier_.AddConnectionObserver(&observer1, GetTaskRunner());
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle2 =
      notifier_.AddConnectionObserver(&observer2, GetTaskRunner());
  observer1.RemoveObserverOnNotification(std::move(handle1));

  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer1, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(VerifyObservations(
      observer2, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));

  SetConnection(kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer1, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(VerifyObservations(
      observer2, kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
}

TEST_F(NetworkStateNotifierTest, RemovePastObserverWhileNotifying) {
  StateObserver observer1, observer2;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle1 =
      notifier_.AddConnectionObserver(&observer1, GetTaskRunner());
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle2 =
      notifier_.AddConnectionObserver(&observer2, GetTaskRunner());
  observer2.RemoveObserverOnNotification(std::move(handle1));

  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOff);
  EXPECT_EQ(observer1.ObservedType(), kWebConnectionTypeBluetooth);
  EXPECT_EQ(observer2.ObservedType(), kWebConnectionTypeBluetooth);

  SetConnection(kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer1, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(VerifyObservations(
      observer2, kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
}

TEST_F(NetworkStateNotifierTest, RemoveFutureObserverWhileNotifying) {
  StateObserver observer1, observer2, observer3;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle1 =
      notifier_.AddConnectionObserver(&observer1, GetTaskRunner());
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle2 =
      notifier_.AddConnectionObserver(&observer2, GetTaskRunner());
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle3 =
      notifier_.AddConnectionObserver(&observer3, GetTaskRunner());
  observer1.RemoveObserverOnNotification(std::move(handle2));

  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer1, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(VerifyObservations(
      observer2, kWebConnectionTypeNone, kNoneMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(VerifyObservations(
      observer3, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
}

TEST_F(NetworkStateNotifierTest, MultipleContextsAddObserver) {
  StateObserver observer1, observer2;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle1 =
      notifier_.AddConnectionObserver(&observer1, GetTaskRunner());
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle2 =
      notifier_.AddConnectionObserver(&observer2, GetTaskRunner2());

  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer1, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
      kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(VerifyObservations(
      observer2, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
      kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff));
}

TEST_F(NetworkStateNotifierTest, RemoveContext) {
  StateObserver observer1, observer2;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle1 =
      notifier_.AddConnectionObserver(&observer1, GetTaskRunner());
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle2 =
      notifier_.AddConnectionObserver(&observer2, GetTaskRunner2());
  handle2 = nullptr;

  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer1, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
      kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(VerifyObservations(
      observer2, kWebConnectionTypeNone, kNoneMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
}

TEST_F(NetworkStateNotifierTest, RemoveAllContexts) {
  StateObserver observer1, observer2;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle1 =
      notifier_.AddConnectionObserver(&observer1, GetTaskRunner());
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle2 =
      notifier_.AddConnectionObserver(&observer2, GetTaskRunner2());
  handle1 = nullptr;
  handle2 = nullptr;

  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer1, kWebConnectionTypeNone, kNoneMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(VerifyObservations(
      observer2, kWebConnectionTypeNone, kNoneMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
}

TEST_F(NetworkStateNotifierTest, SetNetworkConnectionInfoOverride) {
  StateObserver observer;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle =
      notifier_.AddConnectionObserver(&observer, GetTaskRunner());

  notifier_.SetOnLine(true);
  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(notifier_.OnLine());
  EXPECT_EQ(kWebConnectionTypeBluetooth, notifier_.ConnectionType());
  EXPECT_EQ(kBluetoothMaxBandwidthMbps, notifier_.MaxBandwidth());

  notifier_.SetNetworkConnectionInfoOverride(true, kWebConnectionTypeEthernet,
                                             kEthernetMaxBandwidthMbps);
  RunPendingTasks();
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(notifier_.OnLine());
  EXPECT_EQ(kWebConnectionTypeEthernet, notifier_.ConnectionType());
  EXPECT_EQ(kEthernetMaxBandwidthMbps, notifier_.MaxBandwidth());

  // When override is active, calls to setOnLine and setConnection are temporary
  // ignored.
  notifier_.SetOnLine(false);
  SetConnection(kWebConnectionTypeNone, kNoneMaxBandwidthMbps,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOff);
  RunPendingTasks();
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(notifier_.OnLine());
  EXPECT_EQ(kWebConnectionTypeEthernet, notifier_.ConnectionType());
  EXPECT_EQ(kEthernetMaxBandwidthMbps, notifier_.MaxBandwidth());

  notifier_.ClearOverride();
  RunPendingTasks();
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeNone, kNoneMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_FALSE(notifier_.OnLine());
  EXPECT_EQ(kWebConnectionTypeNone, notifier_.ConnectionType());
  EXPECT_EQ(kNoneMaxBandwidthMbps, notifier_.MaxBandwidth());
}

TEST_F(NetworkStateNotifierTest, SetNetworkQualityInfoOverride) {
  StateObserver observer;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle =
      notifier_.AddConnectionObserver(&observer, GetTaskRunner());

  notifier_.SetOnLine(true);
  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(notifier_.OnLine());
  EXPECT_EQ(kWebConnectionTypeBluetooth, notifier_.ConnectionType());
  EXPECT_EQ(kBluetoothMaxBandwidthMbps, notifier_.MaxBandwidth());

  notifier_.SetNetworkQualityInfoOverride(
      WebEffectiveConnectionType::kType3G,
      kEthernetHttpRtt.value().InMilliseconds(),
      kEthernetThroughputMbps.value());
  RunPendingTasks();
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeOther,
      NetworkStateNotifier::NetworkState::kInvalidMaxBandwidth,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt, kUnknownRtt,
      kEthernetThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(notifier_.OnLine());
  EXPECT_EQ(kWebConnectionTypeOther, notifier_.ConnectionType());
  EXPECT_EQ(-1, notifier_.MaxBandwidth());
  EXPECT_EQ(WebEffectiveConnectionType::kType3G, notifier_.EffectiveType());
  EXPECT_EQ(kEthernetHttpRtt, notifier_.HttpRtt());
  EXPECT_EQ(kEthernetThroughputMbps, notifier_.DownlinkThroughputMbps());

  // When override is active, calls to SetConnection are temporary ignored.
  notifier_.SetOnLine(false);
  SetConnection(kWebConnectionTypeNone, kNoneMaxBandwidthMbps,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOff);
  RunPendingTasks();
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeOther,
      NetworkStateNotifier::NetworkState::kInvalidMaxBandwidth,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt, kUnknownRtt,
      kEthernetThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(notifier_.OnLine());
  EXPECT_EQ(kWebConnectionTypeOther, notifier_.ConnectionType());
  EXPECT_EQ(-1, notifier_.MaxBandwidth());
  EXPECT_EQ(WebEffectiveConnectionType::kType3G, notifier_.EffectiveType());
  EXPECT_EQ(kEthernetHttpRtt, notifier_.HttpRtt());
  EXPECT_EQ(kEthernetThroughputMbps, notifier_.DownlinkThroughputMbps());

  // Override the network connection info as well.
  notifier_.SetNetworkConnectionInfoOverride(true, kWebConnectionTypeEthernet,
                                             kEthernetMaxBandwidthMbps);
  RunPendingTasks();
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt, kUnknownRtt,
      kEthernetThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(notifier_.OnLine());
  EXPECT_EQ(kWebConnectionTypeEthernet, notifier_.ConnectionType());
  EXPECT_EQ(kEthernetMaxBandwidthMbps, notifier_.MaxBandwidth());
  EXPECT_EQ(WebEffectiveConnectionType::kType3G, notifier_.EffectiveType());
  EXPECT_EQ(kEthernetHttpRtt, notifier_.HttpRtt());
  EXPECT_EQ(kEthernetThroughputMbps, notifier_.DownlinkThroughputMbps());

  // Clearing the override should cause the network state to be changed and
  // notified to observers.
  notifier_.ClearOverride();
  RunPendingTasks();
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeNone, kNoneMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_FALSE(notifier_.OnLine());
  EXPECT_EQ(kWebConnectionTypeNone, notifier_.ConnectionType());
  EXPECT_EQ(kNoneMaxBandwidthMbps, notifier_.MaxBandwidth());
  EXPECT_EQ(WebEffectiveConnectionType::kTypeUnknown,
            notifier_.EffectiveType());
  EXPECT_EQ(kUnknownRtt, notifier_.TransportRtt());
  EXPECT_EQ(kUnknownThroughputMbps, notifier_.DownlinkThroughputMbps());
}

TEST_F(NetworkStateNotifierTest, SaveDataOverride) {
  StateObserver observer;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle =
      notifier_.AddConnectionObserver(&observer, GetTaskRunner());

  notifier_.SetOnLine(true);
  // Set save-data attribute to false.
  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt, kUnknownRtt,
      kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_TRUE(notifier_.OnLine());
  EXPECT_EQ(kWebConnectionTypeBluetooth, notifier_.ConnectionType());
  EXPECT_EQ(kBluetoothMaxBandwidthMbps, notifier_.MaxBandwidth());
  EXPECT_FALSE(notifier_.SaveDataEnabled());

  // Set save-data attribute to true.
  notifier_.SetSaveDataEnabledOverride(true);
  RunPendingTasks();
  EXPECT_TRUE(VerifyObservations(observer, kWebConnectionTypeOther, -1,
                                 WebEffectiveConnectionType::kTypeUnknown,
                                 kUnknownRtt, kUnknownRtt,
                                 kUnknownThroughputMbps, SaveData::kOn));
  EXPECT_TRUE(notifier_.OnLine());
  EXPECT_EQ(kWebConnectionTypeOther, notifier_.ConnectionType());
  EXPECT_EQ(-1, notifier_.MaxBandwidth());
  EXPECT_TRUE(notifier_.SaveDataEnabled());

  // When override is active, calls to SetConnection are temporary ignored.
  // save_data is set to false in SetConnection() but would be temporarily
  // ignored.
  notifier_.SetOnLine(false);
  SetConnection(kWebConnectionTypeNone, -1,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOff);
  RunPendingTasks();
  EXPECT_TRUE(VerifyObservations(observer, kWebConnectionTypeOther, -1,
                                 WebEffectiveConnectionType::kTypeUnknown,
                                 kUnknownRtt, kUnknownRtt,
                                 kUnknownThroughputMbps, SaveData::kOn));
  EXPECT_TRUE(notifier_.OnLine());
  EXPECT_EQ(kWebConnectionTypeOther, notifier_.ConnectionType());
  EXPECT_EQ(-1, notifier_.MaxBandwidth());
  EXPECT_TRUE(notifier_.SaveDataEnabled());

  // CLearing the override should cause the network state to be changed and
  // notified to observers.
  notifier_.ClearOverride();
  RunPendingTasks();
  EXPECT_TRUE(VerifyObservations(observer, kWebConnectionTypeNone, -1,
                                 WebEffectiveConnectionType::kTypeUnknown,
                                 kUnknownRtt, kUnknownRtt,
                                 kUnknownThroughputMbps, SaveData::kOff));
  EXPECT_FALSE(notifier_.OnLine());
  EXPECT_EQ(kWebConnectionTypeNone, notifier_.ConnectionType());
  EXPECT_EQ(-1, notifier_.MaxBandwidth());
  EXPECT_FALSE(notifier_.SaveDataEnabled());

  // Set save-data attribute to true.
  SetConnection(kWebConnectionTypeNone, -1,
                WebEffectiveConnectionType::kTypeUnknown, kUnknownRtt,
                kUnknownRtt, kUnknownThroughputMbps, SaveData::kOn);
  RunPendingTasks();
  EXPECT_TRUE(VerifyObservations(observer, kWebConnectionTypeNone, -1,
                                 WebEffectiveConnectionType::kTypeUnknown,
                                 kUnknownRtt, kUnknownRtt,
                                 kUnknownThroughputMbps, SaveData::kOn));
  EXPECT_FALSE(notifier_.OnLine());
  EXPECT_EQ(kWebConnectionTypeNone, notifier_.ConnectionType());
  EXPECT_EQ(-1, notifier_.MaxBandwidth());
  EXPECT_TRUE(notifier_.SaveDataEnabled());
}

TEST_F(NetworkStateNotifierTest, NoExtraNotifications) {
  StateObserver observer;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle =
      notifier_.AddConnectionObserver(&observer, GetTaskRunner());

  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
      kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff));
  EXPECT_EQ(observer.CallbackCount(), 2);

  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff);
  EXPECT_EQ(observer.CallbackCount(), 2);

  SetConnection(kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
                WebEffectiveConnectionType::kType4G,
                kEthernetHttpRtt.value() * 2, kEthernetTransportRtt.value() * 2,
                kEthernetThroughputMbps.value() * 2, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
      WebEffectiveConnectionType::kType4G, kEthernetHttpRtt.value() * 2,
      kEthernetTransportRtt.value() * 2, kEthernetThroughputMbps.value() * 2,
      SaveData::kOff));
  EXPECT_EQ(observer.CallbackCount(), 4);

  SetConnection(kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
                WebEffectiveConnectionType::kType4G,
                kEthernetHttpRtt.value() * 2, kEthernetTransportRtt.value() * 2,
                kEthernetThroughputMbps.value() * 2, SaveData::kOff);
  EXPECT_EQ(observer.CallbackCount(), 4);

  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff);
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
      kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff));
  EXPECT_EQ(observer.CallbackCount(), 6);

  // Changing the Save-Data attribute should trigger one callback.
  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOn);
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
      kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOn));
  EXPECT_EQ(observer.CallbackCount(), 7);

  SetConnection(kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOn);
  EXPECT_TRUE(VerifyObservations(
      observer, kWebConnectionTypeBluetooth, kBluetoothMaxBandwidthMbps,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
      kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOn));
  EXPECT_EQ(observer.CallbackCount(), 7);
}

TEST_F(NetworkStateNotifierTest, NoNotificationOnInitialization) {
  NetworkStateNotifier notifier;
  StateObserver observer;

  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle1 =
      notifier.AddConnectionObserver(&observer, GetTaskRunner());
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle2 =
      notifier.AddOnLineObserver(&observer, GetTaskRunner());
  RunPendingTasks();
  EXPECT_EQ(observer.CallbackCount(), 0);

  notifier.SetWebConnection(kWebConnectionTypeBluetooth,
                            kBluetoothMaxBandwidthMbps);
  notifier.SetOnLine(true);
  RunPendingTasks();
  EXPECT_EQ(observer.CallbackCount(), 0);

  notifier.SetOnLine(true);
  notifier.SetWebConnection(kWebConnectionTypeBluetooth,
                            kBluetoothMaxBandwidthMbps);
  RunPendingTasks();
  EXPECT_EQ(observer.CallbackCount(), 0);

  notifier.SetWebConnection(kWebConnectionTypeEthernet,
                            kEthernetMaxBandwidthMbps);
  RunPendingTasks();
  EXPECT_EQ(observer.CallbackCount(), 1);
  EXPECT_EQ(observer.ObservedType(), kWebConnectionTypeEthernet);
  EXPECT_EQ(observer.ObservedMaxBandwidth(), kEthernetMaxBandwidthMbps);

  notifier.SetOnLine(false);
  RunPendingTasks();
  EXPECT_EQ(observer.CallbackCount(), 2);
  EXPECT_FALSE(observer.ObservedOnLineState());
}

TEST_F(NetworkStateNotifierTest, OnLineNotification) {
  StateObserver observer;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle =
      notifier_.AddOnLineObserver(&observer, GetTaskRunner());

  SetOnLine(true);
  RunPendingTasks();
  EXPECT_TRUE(observer.ObservedOnLineState());
  EXPECT_EQ(observer.CallbackCount(), 1);

  SetOnLine(false);
  RunPendingTasks();
  EXPECT_FALSE(observer.ObservedOnLineState());
  EXPECT_EQ(observer.CallbackCount(), 2);
}

TEST_F(NetworkStateNotifierTest, MultipleObservers) {
  StateObserver observer1;
  StateObserver observer2;

  // Observer1 observes online state, Observer2 observes both.
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle1 =
      notifier_.AddOnLineObserver(&observer1, GetTaskRunner());
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle2 =
      notifier_.AddConnectionObserver(&observer2, GetTaskRunner());
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle3 =
      notifier_.AddOnLineObserver(&observer2, GetTaskRunner());

  notifier_.SetOnLine(true);
  RunPendingTasks();
  EXPECT_TRUE(observer1.ObservedOnLineState());
  EXPECT_TRUE(observer2.ObservedOnLineState());
  EXPECT_EQ(observer1.CallbackCount(), 1);
  EXPECT_EQ(observer2.CallbackCount(), 1);

  notifier_.SetOnLine(false);
  RunPendingTasks();
  EXPECT_FALSE(observer1.ObservedOnLineState());
  EXPECT_FALSE(observer2.ObservedOnLineState());
  EXPECT_EQ(observer1.CallbackCount(), 2);
  EXPECT_EQ(observer2.CallbackCount(), 2);

  notifier_.SetOnLine(true);
  SetConnection(kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
                WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
                kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff);

  EXPECT_TRUE(observer1.ObservedOnLineState());
  EXPECT_TRUE(observer2.ObservedOnLineState());
  EXPECT_TRUE(VerifyObservations(
      observer2, kWebConnectionTypeEthernet, kEthernetMaxBandwidthMbps,
      WebEffectiveConnectionType::kType3G, kEthernetHttpRtt,
      kEthernetTransportRtt, kEthernetThroughputMbps, SaveData::kOff));
  EXPECT_EQ(observer1.CallbackCount(), 3);
  EXPECT_EQ(observer2.CallbackCount(), 5);
}

}  // namespace blink
