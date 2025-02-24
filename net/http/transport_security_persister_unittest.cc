// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/transport_security_persister.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_task_runner_handle.h"
#include "net/http/transport_security_state.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

namespace {

const char kReportUri[] = "http://www.example.test/report";

class TransportSecurityPersisterTest : public testing::Test {
 public:
  TransportSecurityPersisterTest() {
  }

  ~TransportSecurityPersisterTest() override {
    EXPECT_TRUE(base::MessageLoopForIO::IsCurrent());
    base::RunLoop().RunUntilIdle();
  }

  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    ASSERT_TRUE(base::MessageLoopForIO::IsCurrent());
    persister_ = std::make_unique<TransportSecurityPersister>(
        &state_, temp_dir_.GetPath(), base::ThreadTaskRunnerHandle::Get());
  }

 protected:
  base::ScopedTempDir temp_dir_;
  TransportSecurityState state_;
  std::unique_ptr<TransportSecurityPersister> persister_;
};

// Tests that LoadEntries() clears existing non-static entries.
TEST_F(TransportSecurityPersisterTest, LoadEntriesClearsExistingState) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  std::string output;
  bool dirty;

  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;
  TransportSecurityState::ExpectCTState expect_ct_state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  static const char kYahooDomain[] = "yahoo.com";

  EXPECT_FALSE(state_.GetDynamicSTSState(kYahooDomain, &sts_state));
  EXPECT_FALSE(state_.GetDynamicPKPState(kYahooDomain, &pkp_state));

  state_.AddHSTS(kYahooDomain, expiry, false /* include subdomains */);
  HashValue spki(HASH_VALUE_SHA256);
  memset(spki.data(), 0, spki.size());
  HashValueVector dynamic_spki_hashes;
  dynamic_spki_hashes.push_back(spki);
  state_.AddHPKP(kYahooDomain, expiry, false, dynamic_spki_hashes, GURL());
  state_.AddExpectCT(kYahooDomain, expiry, true /* enforce */, GURL());

  EXPECT_TRUE(state_.GetDynamicSTSState(kYahooDomain, &sts_state));
  EXPECT_TRUE(state_.GetDynamicPKPState(kYahooDomain, &pkp_state));
  EXPECT_TRUE(state_.GetDynamicExpectCTState(kYahooDomain, &expect_ct_state));

  EXPECT_TRUE(persister_->LoadEntries("{}", &dirty));
  EXPECT_FALSE(dirty);

  EXPECT_FALSE(state_.GetDynamicSTSState(kYahooDomain, &sts_state));
  EXPECT_FALSE(state_.GetDynamicPKPState(kYahooDomain, &pkp_state));
  EXPECT_FALSE(state_.GetDynamicExpectCTState(kYahooDomain, &expect_ct_state));
}

TEST_F(TransportSecurityPersisterTest, SerializeData1) {
  std::string output;
  bool dirty;

  EXPECT_TRUE(persister_->SerializeData(&output));
  EXPECT_TRUE(persister_->LoadEntries(output, &dirty));
  EXPECT_FALSE(dirty);
}

TEST_F(TransportSecurityPersisterTest, SerializeData2) {
  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  static const char kYahooDomain[] = "yahoo.com";

  EXPECT_FALSE(
      state_.GetStaticDomainState(kYahooDomain, &sts_state, &pkp_state));
  EXPECT_FALSE(state_.GetDynamicSTSState(kYahooDomain, &sts_state));
  EXPECT_FALSE(state_.GetDynamicPKPState(kYahooDomain, &pkp_state));

  bool include_subdomains = true;
  state_.AddHSTS(kYahooDomain, expiry, include_subdomains);

  std::string output;
  bool dirty;
  EXPECT_TRUE(persister_->SerializeData(&output));
  EXPECT_TRUE(persister_->LoadEntries(output, &dirty));

  EXPECT_TRUE(state_.GetDynamicSTSState(kYahooDomain, &sts_state));
  EXPECT_EQ(sts_state.upgrade_mode,
            TransportSecurityState::STSState::MODE_FORCE_HTTPS);
  EXPECT_TRUE(state_.GetDynamicSTSState("foo.yahoo.com", &sts_state));
  EXPECT_EQ(sts_state.upgrade_mode,
            TransportSecurityState::STSState::MODE_FORCE_HTTPS);
  EXPECT_TRUE(state_.GetDynamicSTSState("foo.bar.yahoo.com", &sts_state));
  EXPECT_EQ(sts_state.upgrade_mode,
            TransportSecurityState::STSState::MODE_FORCE_HTTPS);
  EXPECT_TRUE(state_.GetDynamicSTSState("foo.bar.baz.yahoo.com", &sts_state));
  EXPECT_EQ(sts_state.upgrade_mode,
            TransportSecurityState::STSState::MODE_FORCE_HTTPS);
  EXPECT_FALSE(state_.GetStaticDomainState("com", &sts_state, &pkp_state));
}

TEST_F(TransportSecurityPersisterTest, SerializeData3) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  const GURL report_uri(kReportUri);
  // Add an entry.
  HashValue fp1(HASH_VALUE_SHA256);
  memset(fp1.data(), 0, fp1.size());
  HashValue fp2(HASH_VALUE_SHA256);
  memset(fp2.data(), 1, fp2.size());
  base::Time expiry =
      base::Time::Now() + base::TimeDelta::FromSeconds(1000);
  HashValueVector dynamic_spki_hashes;
  dynamic_spki_hashes.push_back(fp1);
  dynamic_spki_hashes.push_back(fp2);
  bool include_subdomains = false;
  state_.AddHSTS("www.example.com", expiry, include_subdomains);
  state_.AddHPKP("www.example.com", expiry, include_subdomains,
                 dynamic_spki_hashes, report_uri);
  state_.AddExpectCT("www.example.com", expiry, true /* enforce */, GURL());

  // Add another entry.
  memset(fp1.data(), 2, fp1.size());
  memset(fp2.data(), 3, fp2.size());
  expiry =
      base::Time::Now() + base::TimeDelta::FromSeconds(3000);
  dynamic_spki_hashes.push_back(fp1);
  dynamic_spki_hashes.push_back(fp2);
  state_.AddHSTS("www.example.net", expiry, include_subdomains);
  state_.AddHPKP("www.example.net", expiry, include_subdomains,
                 dynamic_spki_hashes, report_uri);
  state_.AddExpectCT("www.example.net", expiry, false /* enforce */,
                     report_uri);

  // Save a copy of everything.
  std::set<std::string> sts_saved;
  TransportSecurityState::STSStateIterator sts_iter(state_);
  while (sts_iter.HasNext()) {
    sts_saved.insert(sts_iter.hostname());
    sts_iter.Advance();
  }

  std::set<std::string> pkp_saved;
  TransportSecurityState::PKPStateIterator pkp_iter(state_);
  while (pkp_iter.HasNext()) {
    pkp_saved.insert(pkp_iter.hostname());
    pkp_iter.Advance();
  }

  std::set<std::string> expect_ct_saved;
  TransportSecurityState::ExpectCTStateIterator expect_ct_iter(state_);
  while (expect_ct_iter.HasNext()) {
    expect_ct_saved.insert(expect_ct_iter.hostname());
    expect_ct_iter.Advance();
  }

  std::string serialized;
  EXPECT_TRUE(persister_->SerializeData(&serialized));

  // Persist the data to the file. For the test to be fast and not flaky, we
  // just do it directly rather than call persister_->StateIsDirty. (That uses
  // ImportantFileWriter, which has an asynchronous commit interval rather
  // than block.) Use a different basename just for cleanliness.
  base::FilePath path =
      temp_dir_.GetPath().AppendASCII("TransportSecurityPersisterTest");
  EXPECT_EQ(static_cast<int>(serialized.size()),
            base::WriteFile(path, serialized.c_str(), serialized.size()));

  // Read the data back.
  std::string persisted;
  EXPECT_TRUE(base::ReadFileToString(path, &persisted));
  EXPECT_EQ(persisted, serialized);
  bool dirty;
  EXPECT_TRUE(persister_->LoadEntries(persisted, &dirty));
  EXPECT_FALSE(dirty);

  // Check that states are the same as saved.
  size_t count = 0;
  TransportSecurityState::STSStateIterator sts_iter2(state_);
  while (sts_iter2.HasNext()) {
    count++;
    sts_iter2.Advance();
  }
  EXPECT_EQ(count, sts_saved.size());

  count = 0;
  TransportSecurityState::PKPStateIterator pkp_iter2(state_);
  while (pkp_iter2.HasNext()) {
    count++;
    pkp_iter2.Advance();
  }
  EXPECT_EQ(count, pkp_saved.size());

  count = 0;
  TransportSecurityState::ExpectCTStateIterator expect_ct_iter2(state_);
  while (expect_ct_iter2.HasNext()) {
    count++;
    expect_ct_iter2.Advance();
  }
  EXPECT_EQ(count, expect_ct_saved.size());
}

TEST_F(TransportSecurityPersisterTest, SerializeDataOld) {
  // This is an old-style piece of transport state JSON, which has no creation
  // date.
  std::string output =
      "{ "
      "\"NiyD+3J1r6z1wjl2n1ALBu94Zj9OsEAMo0kCN8js0Uk=\": {"
      "\"expiry\": 1266815027.983453, "
      "\"include_subdomains\": false, "
      "\"mode\": \"strict\" "
      "}"
      "}";
  bool dirty;
  EXPECT_TRUE(persister_->LoadEntries(output, &dirty));
  EXPECT_TRUE(dirty);
}

TEST_F(TransportSecurityPersisterTest, PublicKeyPins) {
  const GURL report_uri(kReportUri);
  TransportSecurityState::PKPState pkp_state;
  static const char kTestDomain[] = "example.com";

  EXPECT_FALSE(state_.GetDynamicPKPState(kTestDomain, &pkp_state));
  HashValueVector hashes;
  std::string failure_log;
  EXPECT_FALSE(pkp_state.CheckPublicKeyPins(hashes, &failure_log));

  HashValue sha256(HASH_VALUE_SHA256);
  memset(sha256.data(), '1', sha256.size());
  pkp_state.spki_hashes.push_back(sha256);

  EXPECT_FALSE(pkp_state.CheckPublicKeyPins(hashes, &failure_log));

  hashes.push_back(sha256);
  EXPECT_TRUE(pkp_state.CheckPublicKeyPins(hashes, &failure_log));

  hashes[0].data()[0] = '2';
  EXPECT_FALSE(pkp_state.CheckPublicKeyPins(hashes, &failure_log));

  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  bool include_subdomains = false;
  state_.AddHSTS(kTestDomain, expiry, include_subdomains);
  state_.AddHPKP(kTestDomain, expiry, include_subdomains, pkp_state.spki_hashes,
                 report_uri);
  std::string serialized;
  EXPECT_TRUE(persister_->SerializeData(&serialized));
  bool dirty;
  EXPECT_TRUE(persister_->LoadEntries(serialized, &dirty));

  TransportSecurityState::PKPState new_pkp_state;
  EXPECT_TRUE(state_.GetDynamicPKPState(kTestDomain, &new_pkp_state));
  EXPECT_EQ(1u, new_pkp_state.spki_hashes.size());
  EXPECT_EQ(sha256.tag, new_pkp_state.spki_hashes[0].tag);
  EXPECT_EQ(0, memcmp(new_pkp_state.spki_hashes[0].data(), sha256.data(),
                      sha256.size()));
  EXPECT_EQ(report_uri, new_pkp_state.report_uri);
}

// Tests that dynamic Expect-CT state is serialized and deserialized correctly.
TEST_F(TransportSecurityPersisterTest, ExpectCT) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  const GURL report_uri(kReportUri);
  TransportSecurityState::ExpectCTState expect_ct_state;
  static const char kTestDomain[] = "example.test";

  EXPECT_FALSE(state_.GetDynamicExpectCTState(kTestDomain, &expect_ct_state));

  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  state_.AddExpectCT(kTestDomain, expiry, true /* enforce */, GURL());
  std::string serialized;
  EXPECT_TRUE(persister_->SerializeData(&serialized));
  bool dirty;
  // LoadEntries() clears existing dynamic data before loading entries from
  // |serialized|.
  EXPECT_TRUE(persister_->LoadEntries(serialized, &dirty));

  TransportSecurityState::ExpectCTState new_expect_ct_state;
  EXPECT_TRUE(
      state_.GetDynamicExpectCTState(kTestDomain, &new_expect_ct_state));
  EXPECT_TRUE(new_expect_ct_state.enforce);
  EXPECT_TRUE(new_expect_ct_state.report_uri.is_empty());
  EXPECT_EQ(expiry, new_expect_ct_state.expiry);

  // Update the state for the domain and check that it is
  // serialized/deserialized correctly.
  state_.AddExpectCT(kTestDomain, expiry, false /* enforce */, report_uri);
  EXPECT_TRUE(persister_->SerializeData(&serialized));
  EXPECT_TRUE(persister_->LoadEntries(serialized, &dirty));
  EXPECT_TRUE(
      state_.GetDynamicExpectCTState(kTestDomain, &new_expect_ct_state));
  EXPECT_FALSE(new_expect_ct_state.enforce);
  EXPECT_EQ(report_uri, new_expect_ct_state.report_uri);
  EXPECT_EQ(expiry, new_expect_ct_state.expiry);
}

// Tests that dynamic Expect-CT state is serialized and deserialized correctly
// when there is also PKP and STS data present.
TEST_F(TransportSecurityPersisterTest, ExpectCTWithSTSAndPKPDataPresent) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  const GURL report_uri(kReportUri);
  TransportSecurityState::ExpectCTState expect_ct_state;
  static const char kTestDomain[] = "example.test";

  EXPECT_FALSE(state_.GetDynamicExpectCTState(kTestDomain, &expect_ct_state));

  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  state_.AddHSTS(kTestDomain, expiry, false /* include subdomains */);
  state_.AddExpectCT(kTestDomain, expiry, true /* enforce */, GURL());
  HashValue spki_hash(HASH_VALUE_SHA256);
  memset(spki_hash.data(), 0, spki_hash.size());
  HashValueVector dynamic_spki_hashes;
  dynamic_spki_hashes.push_back(spki_hash);
  state_.AddHPKP(kTestDomain, expiry, false /* include subdomains */,
                 dynamic_spki_hashes, GURL());

  std::string serialized;
  EXPECT_TRUE(persister_->SerializeData(&serialized));
  bool dirty;
  // LoadEntries() clears existing dynamic data before loading entries from
  // |serialized|.
  EXPECT_TRUE(persister_->LoadEntries(serialized, &dirty));

  TransportSecurityState::ExpectCTState new_expect_ct_state;
  EXPECT_TRUE(
      state_.GetDynamicExpectCTState(kTestDomain, &new_expect_ct_state));
  EXPECT_TRUE(new_expect_ct_state.enforce);
  EXPECT_TRUE(new_expect_ct_state.report_uri.is_empty());
  EXPECT_EQ(expiry, new_expect_ct_state.expiry);
  // Check that STS and PKP state are loaded properly as well.
  TransportSecurityState::STSState sts_state;
  EXPECT_TRUE(state_.GetDynamicSTSState(kTestDomain, &sts_state));
  EXPECT_EQ(sts_state.upgrade_mode,
            TransportSecurityState::STSState::MODE_FORCE_HTTPS);
  TransportSecurityState::PKPState pkp_state;
  EXPECT_TRUE(state_.GetDynamicPKPState(kTestDomain, &pkp_state));
  EXPECT_EQ(1u, pkp_state.spki_hashes.size());
  EXPECT_EQ(0, memcmp(pkp_state.spki_hashes[0].data(), spki_hash.data(),
                      spki_hash.size()));
}

// Tests that Expect-CT state is not serialized and persisted when the feature
// is disabled.
TEST_F(TransportSecurityPersisterTest, ExpectCTDisabled) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndDisableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  const GURL report_uri(kReportUri);
  TransportSecurityState::ExpectCTState expect_ct_state;
  static const char kTestDomain[] = "example.test";

  EXPECT_FALSE(state_.GetDynamicExpectCTState(kTestDomain, &expect_ct_state));

  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  state_.AddExpectCT(kTestDomain, expiry, true /* enforce */, GURL());
  std::string serialized;
  EXPECT_TRUE(persister_->SerializeData(&serialized));
  bool dirty;
  EXPECT_TRUE(persister_->LoadEntries(serialized, &dirty));

  TransportSecurityState::ExpectCTState new_expect_ct_state;
  EXPECT_FALSE(
      state_.GetDynamicExpectCTState(kTestDomain, &new_expect_ct_state));
}

}  // namespace

}  // namespace net
