// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/transport_security_state.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "base/base64.h"
#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/metrics/field_trial.h"
#include "base/rand_util.h"
#include "base/strings/string_piece.h"
#include "base/test/histogram_tester.h"
#include "base/test/mock_entropy_provider.h"
#include "base/test/scoped_feature_list.h"
#include "base/values.h"
#include "build/build_config.h"
#include "crypto/openssl_util.h"
#include "crypto/sha2.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_errors.h"
#include "net/base/test_completion_callback.h"
#include "net/cert/asn1_util.h"
#include "net/cert/cert_verifier.h"
#include "net/cert/cert_verify_result.h"
#include "net/cert/ct_policy_status.h"
#include "net/cert/test_root_certs.h"
#include "net/cert/x509_cert_types.h"
#include "net/cert/x509_certificate.h"
#include "net/http/http_status_code.h"
#include "net/http/http_util.h"
#include "net/net_features.h"
#include "net/ssl/ssl_info.h"
#include "net/test/cert_test_util.h"
#include "net/test/test_data_directory.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

namespace {

namespace test_default {
#include "net/http/transport_security_state_static_unittest_default.h"
}
namespace test1 {
#include "net/http/transport_security_state_static_unittest1.h"
}
namespace test2 {
#include "net/http/transport_security_state_static_unittest2.h"
}
namespace test3 {
#include "net/http/transport_security_state_static_unittest3.h"
}

const char kHost[] = "example.test";
const char kSubdomain[] = "foo.example.test";
const uint16_t kPort = 443;
const char kReportUri[] = "http://report-example.test/test";
const char kExpectCTStaticHostname[] = "expect-ct.preloaded.test";
const char kExpectCTStaticReportURI[] =
    "http://report-uri.preloaded.test/expect-ct";
const char kExpectStapleStaticHostname[] = "expect-staple.preloaded.test";
const char kExpectStapleStaticReportURI[] =
    "http://report-uri.preloaded.test/expect-staple";
const char kExpectStapleStaticIncludeSubdomainsHostname[] =
    "include-subdomains-expect-staple.preloaded.test";

const char kGoodPin1[] = "fzP+pVAbH0hRoUphJKenIP8+2tD/d2QH9J+kQNieM6Q=";
const char kGoodPin2[] = "9vRUVdjloCa4wXUKfDWotV5eUXYD7vu0v0z9SRzQdzg=";
const char kGoodPin3[] = "Nn8jk5By4Vkq6BeOVZ7R7AC6XUUBZsWmUbJR1f1Y5FY=";

const char* const kGoodPath[] = {
    "sha256/fzP+pVAbH0hRoUphJKenIP8+2tD/d2QH9J+kQNieM6Q=",
    "sha256/9vRUVdjloCa4wXUKfDWotV5eUXYD7vu0v0z9SRzQdzg=",
    "sha256/Nn8jk5By4Vkq6BeOVZ7R7AC6XUUBZsWmUbJR1f1Y5FY=", nullptr,
};

const char* const kBadPath[] = {
    "sha256/1111111111111111111111111111111111111111111=",
    "sha256/2222222222222222222222222222222222222222222=",
    "sha256/3333333333333333333333333333333333333333333=",
    nullptr,
};

// Constructs a SignedCertificateTimestampAndStatus with the given information
// and appends it to |sct_list|.
void MakeTestSCTAndStatus(ct::SignedCertificateTimestamp::Origin origin,
                          const std::string& log_id,
                          const std::string& extensions,
                          const std::string& signature_data,
                          const base::Time& timestamp,
                          ct::SCTVerifyStatus status,
                          SignedCertificateTimestampAndStatusList* sct_list) {
  scoped_refptr<net::ct::SignedCertificateTimestamp> sct(
      new net::ct::SignedCertificateTimestamp());
  sct->version = net::ct::SignedCertificateTimestamp::V1;
  sct->log_id = log_id;
  sct->extensions = extensions;
  sct->timestamp = timestamp;
  sct->signature.signature_data = signature_data;
  sct->origin = origin;
  sct_list->push_back(net::SignedCertificateTimestampAndStatus(sct, status));
}

// A mock ReportSenderInterface that just remembers the latest report
// URI and report to be sent.
class MockCertificateReportSender
    : public TransportSecurityState::ReportSenderInterface {
 public:
  MockCertificateReportSender() {}
  ~MockCertificateReportSender() override {}

  void Send(const GURL& report_uri,
            base::StringPiece content_type,
            base::StringPiece report,
            const base::Callback<void()>& success_callback,
            const base::Callback<void(const GURL&, int, int)>& error_callback)
      override {
    latest_report_uri_ = report_uri;
    report.CopyToString(&latest_report_);
    content_type.CopyToString(&latest_content_type_);
  }

  void Clear() {
    latest_report_uri_ = GURL();
    latest_report_ = std::string();
    latest_content_type_ = std::string();
  }

  const GURL& latest_report_uri() { return latest_report_uri_; }
  const std::string& latest_report() { return latest_report_; }
  const std::string& latest_content_type() { return latest_content_type_; }

 private:
  GURL latest_report_uri_;
  std::string latest_report_;
  std::string latest_content_type_;
};

// A mock ReportSenderInterface that simulates a net error on every report sent.
class MockFailingCertificateReportSender
    : public TransportSecurityState::ReportSenderInterface {
 public:
  MockFailingCertificateReportSender() : net_error_(ERR_CONNECTION_FAILED) {}
  ~MockFailingCertificateReportSender() override {}

  int net_error() { return net_error_; }

  // TransportSecurityState::ReportSenderInterface:
  void Send(const GURL& report_uri,
            base::StringPiece content_type,
            base::StringPiece report,
            const base::Callback<void()>& success_callback,
            const base::Callback<void(const GURL&, int, int)>& error_callback)
      override {
    ASSERT_FALSE(error_callback.is_null());
    error_callback.Run(report_uri, net_error_, 0);
  }

 private:
  const int net_error_;
};

// A mock ExpectCTReporter that remembers the latest violation that was
// reported and the number of violations reported.
class MockExpectCTReporter : public TransportSecurityState::ExpectCTReporter {
 public:
  MockExpectCTReporter() : num_failures_(0) {}
  ~MockExpectCTReporter() override {}

  void OnExpectCTFailed(const HostPortPair& host_port_pair,
                        const GURL& report_uri,
                        base::Time expiration,
                        const X509Certificate* validated_certificate_chain,
                        const X509Certificate* served_certificate_chain,
                        const SignedCertificateTimestampAndStatusList&
                            signed_certificate_timestamps) override {
    num_failures_++;
    host_port_pair_ = host_port_pair;
    report_uri_ = report_uri;
    expiration_ = expiration;
    served_certificate_chain_ = served_certificate_chain;
    validated_certificate_chain_ = validated_certificate_chain;
    signed_certificate_timestamps_ = signed_certificate_timestamps;
  }

  const HostPortPair& host_port_pair() { return host_port_pair_; }
  const GURL& report_uri() { return report_uri_; }
  const base::Time& expiration() { return expiration_; }
  uint32_t num_failures() { return num_failures_; }
  const X509Certificate* served_certificate_chain() {
    return served_certificate_chain_;
  }
  const X509Certificate* validated_certificate_chain() {
    return validated_certificate_chain_;
  }
  const SignedCertificateTimestampAndStatusList&
  signed_certificate_timestamps() {
    return signed_certificate_timestamps_;
  }

 private:
  HostPortPair host_port_pair_;
  GURL report_uri_;
  base::Time expiration_;
  uint32_t num_failures_;
  const X509Certificate* served_certificate_chain_;
  const X509Certificate* validated_certificate_chain_;
  SignedCertificateTimestampAndStatusList signed_certificate_timestamps_;
};

class MockRequireCTDelegate : public TransportSecurityState::RequireCTDelegate {
 public:
  MOCK_METHOD1(IsCTRequiredForHost,
               CTRequirementLevel(const std::string& hostname));
};

void CompareCertificateChainWithList(
    const scoped_refptr<X509Certificate>& cert_chain,
    const base::ListValue* cert_list) {
  ASSERT_TRUE(cert_chain);
  std::vector<std::string> pem_encoded_chain;
  cert_chain->GetPEMEncodedChain(&pem_encoded_chain);
  EXPECT_EQ(pem_encoded_chain.size(), cert_list->GetSize());

  for (size_t i = 0; i < pem_encoded_chain.size(); i++) {
    std::string list_cert;
    ASSERT_TRUE(cert_list->GetString(i, &list_cert));
    EXPECT_EQ(pem_encoded_chain[i], list_cert);
  }
}

void CheckHPKPReport(
    const std::string& report,
    const HostPortPair& host_port_pair,
    bool include_subdomains,
    const std::string& noted_hostname,
    const scoped_refptr<X509Certificate>& served_certificate_chain,
    const scoped_refptr<X509Certificate>& validated_certificate_chain,
    const HashValueVector& known_pins) {
  std::unique_ptr<base::Value> value(base::JSONReader::Read(report));
  ASSERT_TRUE(value);
  ASSERT_TRUE(value->IsType(base::Value::Type::DICTIONARY));

  base::DictionaryValue* report_dict;
  ASSERT_TRUE(value->GetAsDictionary(&report_dict));

  std::string report_hostname;
  EXPECT_TRUE(report_dict->GetString("hostname", &report_hostname));
  EXPECT_EQ(host_port_pair.host(), report_hostname);

  int report_port;
  EXPECT_TRUE(report_dict->GetInteger("port", &report_port));
  EXPECT_EQ(host_port_pair.port(), report_port);

  bool report_include_subdomains;
  EXPECT_TRUE(report_dict->GetBoolean("include-subdomains",
                                      &report_include_subdomains));
  EXPECT_EQ(include_subdomains, report_include_subdomains);

  std::string report_noted_hostname;
  EXPECT_TRUE(report_dict->GetString("noted-hostname", &report_noted_hostname));
  EXPECT_EQ(noted_hostname, report_noted_hostname);

  // TODO(estark): check times in RFC3339 format.

  std::string report_expiration;
  EXPECT_TRUE(
      report_dict->GetString("effective-expiration-date", &report_expiration));
  EXPECT_FALSE(report_expiration.empty());

  std::string report_date;
  EXPECT_TRUE(report_dict->GetString("date-time", &report_date));
  EXPECT_FALSE(report_date.empty());

  base::ListValue* report_served_certificate_chain;
  EXPECT_TRUE(report_dict->GetList("served-certificate-chain",
                                   &report_served_certificate_chain));
  ASSERT_NO_FATAL_FAILURE(CompareCertificateChainWithList(
      served_certificate_chain, report_served_certificate_chain));

  base::ListValue* report_validated_certificate_chain;
  EXPECT_TRUE(report_dict->GetList("validated-certificate-chain",
                                   &report_validated_certificate_chain));
  ASSERT_NO_FATAL_FAILURE(CompareCertificateChainWithList(
      validated_certificate_chain, report_validated_certificate_chain));
}

// Checks the following hold for |report| such that it is a valid Expect-Staple
// report:
// 1. |report| is a JSON dictionary.
// 2. The "hostname" and "port" fields match |host_port_pair|.
// 3. The "response-status" field matches |response_status|
// 4. The "ocsp-response" field is a base64-encoded verson of |ocsp_response|,
//    and is not present when |ocsp_response| is empty.
// 5. The "cert-status" field matches |cert_status|, and is not present when
//    |cert_status| is empty.
// 6. The "validated-chain" and "serverd-chain" fields match those in
//    |ssl_info|.
void CheckSerializedExpectStapleReport(const std::string& report,
                                       const HostPortPair& host_port_pair,
                                       const SSLInfo& ssl_info,
                                       const std::string& ocsp_response,
                                       const std::string& response_status,
                                       const std::string& cert_status) {
  std::unique_ptr<base::Value> value(base::JSONReader::Read(report));
  ASSERT_TRUE(value);
  ASSERT_TRUE(value->IsType(base::Value::Type::DICTIONARY));

  base::DictionaryValue* report_dict;
  ASSERT_TRUE(value->GetAsDictionary(&report_dict));

  std::string report_hostname;
  EXPECT_TRUE(report_dict->GetString("hostname", &report_hostname));
  EXPECT_EQ(host_port_pair.host(), report_hostname);

  int report_port;
  EXPECT_TRUE(report_dict->GetInteger("port", &report_port));
  EXPECT_EQ(host_port_pair.port(), report_port);

  std::string report_response_status;
  EXPECT_TRUE(
      report_dict->GetString("response-status", &report_response_status));
  EXPECT_EQ(response_status, report_response_status);

  std::string report_ocsp_response;
  bool has_ocsp_response =
      report_dict->GetString("ocsp-response", &report_ocsp_response);

  if (!ocsp_response.empty()) {
    EXPECT_TRUE(has_ocsp_response);
    std::string decoded_ocsp_response;
    EXPECT_TRUE(
        base::Base64Decode(report_ocsp_response, &decoded_ocsp_response));
    EXPECT_EQ(ocsp_response, decoded_ocsp_response);
  } else {
    EXPECT_FALSE(has_ocsp_response);
  }

  std::string report_cert_status;
  bool has_cert_status =
      report_dict->GetString("cert-status", &report_cert_status);
  if (!cert_status.empty()) {
    EXPECT_TRUE(has_cert_status);
    EXPECT_EQ(cert_status, report_cert_status);
  } else {
    EXPECT_FALSE(has_cert_status);
  }

  base::ListValue* report_served_certificate_chain;
  bool has_served_chain = report_dict->GetList(
      "served-certificate-chain", &report_served_certificate_chain);

  base::ListValue* report_validated_certificate_chain;
  bool has_validated_chain = report_dict->GetList(
      "validated-certificate-chain", &report_validated_certificate_chain);

  EXPECT_TRUE(has_served_chain);
  EXPECT_NO_FATAL_FAILURE(CompareCertificateChainWithList(
      ssl_info.unverified_cert, report_served_certificate_chain));

  EXPECT_TRUE(has_validated_chain);
  EXPECT_NO_FATAL_FAILURE(CompareCertificateChainWithList(
      ssl_info.cert, report_validated_certificate_chain));
}

// Set up |state| for ExpectStaple, call CheckExpectStaple(), and verify the
// serialized report caught by |reporter|.
void CheckExpectStapleReport(TransportSecurityState* state,
                             MockCertificateReportSender* reporter,
                             const SSLInfo& ssl_info,
                             const std::string& ocsp_response,
                             const std::string& response_status,
                             const std::string& cert_status) {
  // Expect-Staple is preload list based, so we use the baked-in test hostname
  // from the list ("preloaded-expect-staple.badssl.com").
  HostPortPair host_port(kExpectStapleStaticHostname, 443);
  state->SetReportSender(reporter);
  state->CheckExpectStaple(host_port, ssl_info, ocsp_response);
  if (!ssl_info.is_issued_by_known_root) {
    EXPECT_EQ(GURL(), reporter->latest_report_uri());
    EXPECT_EQ(std::string(), reporter->latest_report());
    return;
  }
  EXPECT_EQ(GURL(kExpectStapleStaticReportURI), reporter->latest_report_uri());
  EXPECT_EQ("application/json; charset=utf-8", reporter->latest_content_type());
  std::string serialized_report = reporter->latest_report();
  EXPECT_NO_FATAL_FAILURE(CheckSerializedExpectStapleReport(
      serialized_report, host_port, ssl_info, ocsp_response, response_status,
      cert_status));
}

}  // namespace

class TransportSecurityStateTest : public testing::Test {
 public:
  TransportSecurityStateTest() {
    SetTransportSecurityStateSourceForTesting(&test_default::kHSTSSource);
  }

  ~TransportSecurityStateTest() override {
    SetTransportSecurityStateSourceForTesting(nullptr);
  }

  void SetUp() override {
    crypto::EnsureOpenSSLInit();
  }

  static void DisableStaticPins(TransportSecurityState* state) {
    state->enable_static_pins_ = false;
  }

  static void EnableStaticPins(TransportSecurityState* state) {
    state->enable_static_pins_ = true;
  }

  static void EnableStaticExpectCT(TransportSecurityState* state) {
    state->enable_static_expect_ct_ = true;
  }

  static void SetEnableStaticExpectStaple(TransportSecurityState* state,
                                          bool enabled) {
    state->enable_static_expect_staple_ = enabled;
  }

  static HashValueVector GetSampleSPKIHashes() {
    HashValueVector spki_hashes;
    HashValue hash(HASH_VALUE_SHA256);
    memset(hash.data(), 0, hash.size());
    spki_hashes.push_back(hash);
    return spki_hashes;
  }

  static HashValue GetSampleSPKIHash(uint8_t value) {
    HashValue hash(HASH_VALUE_SHA256);
    memset(hash.data(), value, hash.size());
    return hash;
  }

 protected:
  bool GetStaticDomainState(TransportSecurityState* state,
                            const std::string& host,
                            TransportSecurityState::STSState* sts_result,
                            TransportSecurityState::PKPState* pkp_result) {
    return state->GetStaticDomainState(host, sts_result, pkp_result);
  }

  bool GetExpectCTState(TransportSecurityState* state,
                        const std::string& host,
                        TransportSecurityState::ExpectCTState* result) {
    return state->GetStaticExpectCTState(host, result);
  }

  bool GetExpectStapleState(TransportSecurityState* state,
                            const std::string& host,
                            TransportSecurityState::ExpectStapleState* result) {
    return state->GetStaticExpectStapleState(host, result);
  }
};

TEST_F(TransportSecurityStateTest, DomainNameOddities) {
  TransportSecurityState state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);

  // DNS suffix search tests. Some DNS resolvers allow a terminal "." to
  // indicate not perform DNS suffix searching. Ensure that regardless
  // of how this is treated at the resolver layer, or at the URL/origin
  // layer (that is, whether they are treated as equivalent or distinct),
  // ensure that for policy matching, something lacking a terminal "."
  // is equivalent to something with a terminal "."
  EXPECT_FALSE(state.ShouldUpgradeToSSL("example.com"));

  state.AddHSTS("example.com", expiry, true /* include_subdomains */);
  EXPECT_TRUE(state.ShouldUpgradeToSSL("example.com"));
  // Trailing '.' should be equivalent; it's just a resolver hint
  EXPECT_TRUE(state.ShouldUpgradeToSSL("example.com."));
  // Leading '.' should be invalid
  EXPECT_FALSE(state.ShouldUpgradeToSSL(".example.com"));
  // Subdomains should work regardless
  EXPECT_TRUE(state.ShouldUpgradeToSSL("sub.example.com"));
  EXPECT_TRUE(state.ShouldUpgradeToSSL("sub.example.com."));
  // But invalid subdomains should be rejected
  EXPECT_FALSE(state.ShouldUpgradeToSSL("sub..example.com"));
  EXPECT_FALSE(state.ShouldUpgradeToSSL("sub..example.com."));

  // Now try the inverse form
  TransportSecurityState state2;
  state2.AddHSTS("example.net.", expiry, true /* include_subdomains */);
  EXPECT_TRUE(state2.ShouldUpgradeToSSL("example.net."));
  EXPECT_TRUE(state2.ShouldUpgradeToSSL("example.net"));
  EXPECT_TRUE(state2.ShouldUpgradeToSSL("sub.example.net."));
  EXPECT_TRUE(state2.ShouldUpgradeToSSL("sub.example.net"));

  // Finally, test weird things
  TransportSecurityState state3;
  state3.AddHSTS("", expiry, true /* include_subdomains */);
  EXPECT_FALSE(state3.ShouldUpgradeToSSL(""));
  EXPECT_FALSE(state3.ShouldUpgradeToSSL("."));
  EXPECT_FALSE(state3.ShouldUpgradeToSSL("..."));
  // Make sure it didn't somehow apply HSTS to the world
  EXPECT_FALSE(state3.ShouldUpgradeToSSL("example.org"));

  TransportSecurityState state4;
  state4.AddHSTS(".", expiry, true /* include_subdomains */);
  EXPECT_FALSE(state4.ShouldUpgradeToSSL(""));
  EXPECT_FALSE(state4.ShouldUpgradeToSSL("."));
  EXPECT_FALSE(state4.ShouldUpgradeToSSL("..."));
  EXPECT_FALSE(state4.ShouldUpgradeToSSL("example.org"));

  // Now do the same for preloaded entries
  TransportSecurityState state5;
  EXPECT_TRUE(state5.ShouldUpgradeToSSL("hsts-preloaded.test"));
  EXPECT_TRUE(state5.ShouldUpgradeToSSL("hsts-preloaded.test."));
  EXPECT_FALSE(state5.ShouldUpgradeToSSL("hsts-preloaded..test"));
  EXPECT_FALSE(state5.ShouldUpgradeToSSL("hsts-preloaded..test."));
}

TEST_F(TransportSecurityStateTest, SimpleMatches) {
  TransportSecurityState state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);

  EXPECT_FALSE(state.ShouldUpgradeToSSL("example.com"));
  bool include_subdomains = false;
  state.AddHSTS("example.com", expiry, include_subdomains);
  EXPECT_TRUE(state.ShouldUpgradeToSSL("example.com"));
  EXPECT_TRUE(state.ShouldSSLErrorsBeFatal("example.com"));
  EXPECT_FALSE(state.ShouldUpgradeToSSL("foo.example.com"));
  EXPECT_FALSE(state.ShouldSSLErrorsBeFatal("foo.example.com"));
}

TEST_F(TransportSecurityStateTest, MatchesCase1) {
  TransportSecurityState state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);

  EXPECT_FALSE(state.ShouldUpgradeToSSL("example.com"));
  bool include_subdomains = false;
  state.AddHSTS("EXample.coM", expiry, include_subdomains);
  EXPECT_TRUE(state.ShouldUpgradeToSSL("example.com"));
}

TEST_F(TransportSecurityStateTest, MatchesCase2) {
  TransportSecurityState state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);

  // Check dynamic entries
  EXPECT_FALSE(state.ShouldUpgradeToSSL("EXample.coM"));
  bool include_subdomains = false;
  state.AddHSTS("example.com", expiry, include_subdomains);
  EXPECT_TRUE(state.ShouldUpgradeToSSL("EXample.coM"));

  // Check static entries
  EXPECT_TRUE(state.ShouldUpgradeToSSL("hStS-prelOAded.tEsT"));
  EXPECT_TRUE(
      state.ShouldUpgradeToSSL("inClude-subDOmaIns-hsts-prEloaDed.TesT"));
}

TEST_F(TransportSecurityStateTest, SubdomainMatches) {
  TransportSecurityState state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);

  EXPECT_FALSE(state.ShouldUpgradeToSSL("example.test"));
  bool include_subdomains = true;
  state.AddHSTS("example.test", expiry, include_subdomains);
  EXPECT_TRUE(state.ShouldUpgradeToSSL("example.test"));
  EXPECT_TRUE(state.ShouldUpgradeToSSL("foo.example.test"));
  EXPECT_TRUE(state.ShouldUpgradeToSSL("foo.bar.example.test"));
  EXPECT_TRUE(state.ShouldUpgradeToSSL("foo.bar.baz.example.test"));
  EXPECT_FALSE(state.ShouldUpgradeToSSL("test"));
  EXPECT_FALSE(state.ShouldUpgradeToSSL("notexample.test"));
}

// Tests that a more-specific HSTS or HPKP rule overrides a less-specific rule
// with it, regardless of the includeSubDomains bit. This is a regression test
// for https://crbug.com/469957.
TEST_F(TransportSecurityStateTest, SubdomainCarveout) {
  const GURL report_uri(kReportUri);
  TransportSecurityState state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  const base::Time older = current_time - base::TimeDelta::FromSeconds(1000);

  state.AddHSTS("example1.test", expiry, true);
  state.AddHSTS("foo.example1.test", expiry, false);

  state.AddHPKP("example2.test", expiry, true, GetSampleSPKIHashes(),
                report_uri);
  state.AddHPKP("foo.example2.test", expiry, false, GetSampleSPKIHashes(),
                report_uri);

  EXPECT_TRUE(state.ShouldUpgradeToSSL("example1.test"));
  EXPECT_TRUE(state.ShouldUpgradeToSSL("foo.example1.test"));

  // The foo.example1.test rule overrides the example1.test rule, so
  // bar.foo.example1.test has no HSTS state.
  EXPECT_FALSE(state.ShouldUpgradeToSSL("bar.foo.example1.test"));
  EXPECT_FALSE(state.ShouldSSLErrorsBeFatal("bar.foo.example1.test"));

  EXPECT_TRUE(state.HasPublicKeyPins("example2.test"));
  EXPECT_TRUE(state.HasPublicKeyPins("foo.example2.test"));

  // The foo.example2.test rule overrides the example1.test rule, so
  // bar.foo.example2.test has no HPKP state.
  EXPECT_FALSE(state.HasPublicKeyPins("bar.foo.example2.test"));
  EXPECT_FALSE(state.ShouldSSLErrorsBeFatal("bar.foo.example2.test"));

  // Expire the foo.example*.test rules.
  state.AddHSTS("foo.example1.test", older, false);
  state.AddHPKP("foo.example2.test", older, false, GetSampleSPKIHashes(),
                report_uri);

  // Now the base example*.test rules apply to bar.foo.example*.test.
  EXPECT_TRUE(state.ShouldUpgradeToSSL("bar.foo.example1.test"));
  EXPECT_TRUE(state.ShouldSSLErrorsBeFatal("bar.foo.example1.test"));
  EXPECT_TRUE(state.HasPublicKeyPins("bar.foo.example2.test"));
  EXPECT_TRUE(state.ShouldSSLErrorsBeFatal("bar.foo.example2.test"));
}

TEST_F(TransportSecurityStateTest, FatalSSLErrors) {
  const GURL report_uri(kReportUri);
  TransportSecurityState state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);

  state.AddHSTS("example1.test", expiry, false);
  state.AddHPKP("example2.test", expiry, false, GetSampleSPKIHashes(),
                report_uri);

  // The presense of either HSTS or HPKP is enough to make SSL errors fatal.
  EXPECT_TRUE(state.ShouldSSLErrorsBeFatal("example1.test"));
  EXPECT_TRUE(state.ShouldSSLErrorsBeFatal("example2.test"));
}

// Tests that HPKP and HSTS state both expire. Also tests that expired entries
// are pruned.
TEST_F(TransportSecurityStateTest, Expiration) {
  const GURL report_uri(kReportUri);
  TransportSecurityState state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  const base::Time older = current_time - base::TimeDelta::FromSeconds(1000);

  // Note: this test assumes that inserting an entry with an expiration time in
  // the past works and is pruned on query.
  state.AddHSTS("example1.test", older, false);
  EXPECT_TRUE(TransportSecurityState::STSStateIterator(state).HasNext());
  EXPECT_FALSE(state.ShouldUpgradeToSSL("example1.test"));
  // Querying |state| for a domain should flush out expired entries.
  EXPECT_FALSE(TransportSecurityState::STSStateIterator(state).HasNext());

  state.AddHPKP("example1.test", older, false, GetSampleSPKIHashes(),
                report_uri);
  EXPECT_TRUE(TransportSecurityState::PKPStateIterator(state).HasNext());
  EXPECT_FALSE(state.HasPublicKeyPins("example1.test"));
  // Querying |state| for a domain should flush out expired entries.
  EXPECT_FALSE(TransportSecurityState::PKPStateIterator(state).HasNext());

  state.AddHSTS("example1.test", older, false);
  state.AddHPKP("example1.test", older, false, GetSampleSPKIHashes(),
                report_uri);
  EXPECT_TRUE(TransportSecurityState::STSStateIterator(state).HasNext());
  EXPECT_TRUE(TransportSecurityState::PKPStateIterator(state).HasNext());
  EXPECT_FALSE(state.ShouldSSLErrorsBeFatal("example1.test"));
  // Querying |state| for a domain should flush out expired entries.
  EXPECT_FALSE(TransportSecurityState::STSStateIterator(state).HasNext());
  EXPECT_FALSE(TransportSecurityState::PKPStateIterator(state).HasNext());

  // Test that HSTS can outlive HPKP.
  state.AddHSTS("example1.test", expiry, false);
  state.AddHPKP("example1.test", older, false, GetSampleSPKIHashes(),
                report_uri);
  EXPECT_TRUE(state.ShouldUpgradeToSSL("example1.test"));
  EXPECT_FALSE(state.HasPublicKeyPins("example1.test"));

  // Test that HPKP can outlive HSTS.
  state.AddHSTS("example2.test", older, false);
  state.AddHPKP("example2.test", expiry, false, GetSampleSPKIHashes(),
                report_uri);
  EXPECT_FALSE(state.ShouldUpgradeToSSL("example2.test"));
  EXPECT_TRUE(state.HasPublicKeyPins("example2.test"));
}

// Tests that HPKP and HSTS state are queried independently for subdomain
// matches.
TEST_F(TransportSecurityStateTest, IndependentSubdomain) {
  const GURL report_uri(kReportUri);
  TransportSecurityState state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);

  state.AddHSTS("example1.test", expiry, true);
  state.AddHPKP("example1.test", expiry, false, GetSampleSPKIHashes(),
                report_uri);

  state.AddHSTS("example2.test", expiry, false);
  state.AddHPKP("example2.test", expiry, true, GetSampleSPKIHashes(),
                report_uri);

  EXPECT_TRUE(state.ShouldUpgradeToSSL("foo.example1.test"));
  EXPECT_FALSE(state.HasPublicKeyPins("foo.example1.test"));
  EXPECT_FALSE(state.ShouldUpgradeToSSL("foo.example2.test"));
  EXPECT_TRUE(state.HasPublicKeyPins("foo.example2.test"));
}

// Tests that HPKP and HSTS state are inserted and overridden independently.
TEST_F(TransportSecurityStateTest, IndependentInsertion) {
  const GURL report_uri(kReportUri);
  TransportSecurityState state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);

  // Place an includeSubdomains HSTS entry below a normal HPKP entry.
  state.AddHSTS("example1.test", expiry, true);
  state.AddHPKP("foo.example1.test", expiry, false, GetSampleSPKIHashes(),
                report_uri);

  EXPECT_TRUE(state.ShouldUpgradeToSSL("foo.example1.test"));
  EXPECT_TRUE(state.HasPublicKeyPins("foo.example1.test"));
  EXPECT_TRUE(state.ShouldUpgradeToSSL("example1.test"));
  EXPECT_FALSE(state.HasPublicKeyPins("example1.test"));

  // Drop the includeSubdomains from the HSTS entry.
  state.AddHSTS("example1.test", expiry, false);

  EXPECT_FALSE(state.ShouldUpgradeToSSL("foo.example1.test"));
  EXPECT_TRUE(state.HasPublicKeyPins("foo.example1.test"));

  // Place an includeSubdomains HPKP entry below a normal HSTS entry.
  state.AddHSTS("foo.example2.test", expiry, false);
  state.AddHPKP("example2.test", expiry, true, GetSampleSPKIHashes(),
                report_uri);

  EXPECT_TRUE(state.ShouldUpgradeToSSL("foo.example2.test"));
  EXPECT_TRUE(state.HasPublicKeyPins("foo.example2.test"));

  // Drop the includeSubdomains from the HSTS entry.
  state.AddHPKP("example2.test", expiry, false, GetSampleSPKIHashes(),
                report_uri);

  EXPECT_TRUE(state.ShouldUpgradeToSSL("foo.example2.test"));
  EXPECT_FALSE(state.HasPublicKeyPins("foo.example2.test"));
}

// Tests that GetDynamic[PKP|STS]State returns the correct data and that the
// states are not mixed together.
TEST_F(TransportSecurityStateTest, DynamicDomainState) {
  const GURL report_uri(kReportUri);
  TransportSecurityState state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry1 = current_time + base::TimeDelta::FromSeconds(1000);
  const base::Time expiry2 = current_time + base::TimeDelta::FromSeconds(2000);

  state.AddHSTS("example.com", expiry1, true);
  state.AddHPKP("foo.example.com", expiry2, false, GetSampleSPKIHashes(),
                report_uri);

  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;
  ASSERT_TRUE(state.GetDynamicSTSState("foo.example.com", &sts_state));
  ASSERT_TRUE(state.GetDynamicPKPState("foo.example.com", &pkp_state));
  EXPECT_TRUE(sts_state.ShouldUpgradeToSSL());
  EXPECT_TRUE(pkp_state.HasPublicKeyPins());
  EXPECT_TRUE(sts_state.include_subdomains);
  EXPECT_FALSE(pkp_state.include_subdomains);
  EXPECT_EQ(expiry1, sts_state.expiry);
  EXPECT_EQ(expiry2, pkp_state.expiry);
  EXPECT_EQ("example.com", sts_state.domain);
  EXPECT_EQ("foo.example.com", pkp_state.domain);
}

// Tests that new pins always override previous pins. This should be true for
// both pins at the same domain or includeSubdomains pins at a parent domain.
TEST_F(TransportSecurityStateTest, NewPinsOverride) {
  const GURL report_uri(kReportUri);
  TransportSecurityState state;
  TransportSecurityState::PKPState pkp_state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  HashValue hash1(HASH_VALUE_SHA256);
  memset(hash1.data(), 0x01, hash1.size());
  HashValue hash2(HASH_VALUE_SHA256);
  memset(hash2.data(), 0x02, hash1.size());
  HashValue hash3(HASH_VALUE_SHA256);
  memset(hash3.data(), 0x03, hash1.size());

  state.AddHPKP("example.com", expiry, true, HashValueVector(1, hash1),
                report_uri);

  ASSERT_TRUE(state.GetDynamicPKPState("foo.example.com", &pkp_state));
  ASSERT_EQ(1u, pkp_state.spki_hashes.size());
  EXPECT_EQ(pkp_state.spki_hashes[0], hash1);

  state.AddHPKP("foo.example.com", expiry, false, HashValueVector(1, hash2),
                report_uri);

  ASSERT_TRUE(state.GetDynamicPKPState("foo.example.com", &pkp_state));
  ASSERT_EQ(1u, pkp_state.spki_hashes.size());
  EXPECT_EQ(pkp_state.spki_hashes[0], hash2);

  state.AddHPKP("foo.example.com", expiry, false, HashValueVector(1, hash3),
                report_uri);

  ASSERT_TRUE(state.GetDynamicPKPState("foo.example.com", &pkp_state));
  ASSERT_EQ(1u, pkp_state.spki_hashes.size());
  EXPECT_EQ(pkp_state.spki_hashes[0], hash3);
}

TEST_F(TransportSecurityStateTest, DeleteAllDynamicDataSince) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  TransportSecurityState::ExpectCTState expect_ct_state;

  TransportSecurityState state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  const base::Time older = current_time - base::TimeDelta::FromSeconds(1000);

  EXPECT_FALSE(state.ShouldUpgradeToSSL("example.com"));
  EXPECT_FALSE(state.HasPublicKeyPins("example.com"));
  EXPECT_FALSE(state.GetDynamicExpectCTState("example.com", &expect_ct_state));
  bool include_subdomains = false;
  state.AddHSTS("example.com", expiry, include_subdomains);
  state.AddHPKP("example.com", expiry, include_subdomains,
                GetSampleSPKIHashes(), GURL());
  state.AddExpectCT("example.com", expiry, true, GURL());

  state.DeleteAllDynamicDataSince(expiry);
  EXPECT_TRUE(state.ShouldUpgradeToSSL("example.com"));
  EXPECT_TRUE(state.HasPublicKeyPins("example.com"));
  EXPECT_TRUE(state.GetDynamicExpectCTState("example.com", &expect_ct_state));
  state.DeleteAllDynamicDataSince(older);
  EXPECT_FALSE(state.ShouldUpgradeToSSL("example.com"));
  EXPECT_FALSE(state.HasPublicKeyPins("example.com"));
  EXPECT_FALSE(state.GetDynamicExpectCTState("example.com", &expect_ct_state));

  // Dynamic data in |state| should be empty now.
  EXPECT_FALSE(TransportSecurityState::STSStateIterator(state).HasNext());
  EXPECT_FALSE(TransportSecurityState::PKPStateIterator(state).HasNext());
  EXPECT_FALSE(TransportSecurityState::ExpectCTStateIterator(state).HasNext());
}

TEST_F(TransportSecurityStateTest, DeleteDynamicDataForHost) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  TransportSecurityState state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  bool include_subdomains = false;

  state.AddHSTS("example1.test", expiry, include_subdomains);
  state.AddHPKP("example1.test", expiry, include_subdomains,
                GetSampleSPKIHashes(), GURL());
  state.AddExpectCT("example1.test", expiry, true, GURL());

  EXPECT_TRUE(state.ShouldUpgradeToSSL("example1.test"));
  EXPECT_FALSE(state.ShouldUpgradeToSSL("example2.test"));
  EXPECT_TRUE(state.HasPublicKeyPins("example1.test"));
  EXPECT_FALSE(state.HasPublicKeyPins("example2.test"));
  TransportSecurityState::ExpectCTState expect_ct_state;
  EXPECT_TRUE(state.GetDynamicExpectCTState("example1.test", &expect_ct_state));
  EXPECT_FALSE(
      state.GetDynamicExpectCTState("example2.test", &expect_ct_state));

  EXPECT_TRUE(state.DeleteDynamicDataForHost("example1.test"));
  EXPECT_FALSE(state.ShouldUpgradeToSSL("example1.test"));
  EXPECT_FALSE(state.HasPublicKeyPins("example1.test"));
  EXPECT_FALSE(
      state.GetDynamicExpectCTState("example1.test", &expect_ct_state));
}

TEST_F(TransportSecurityStateTest, LongNames) {
  TransportSecurityState state;
  const char kLongName[] =
      "lookupByWaveIdHashAndWaveIdIdAndWaveIdDomainAndWaveletIdIdAnd"
      "WaveletIdDomainAndBlipBlipid";
  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;
  // Just checks that we don't hit a NOTREACHED.
  EXPECT_FALSE(state.GetStaticDomainState(kLongName, &sts_state, &pkp_state));
  EXPECT_FALSE(state.GetDynamicSTSState(kLongName, &sts_state));
  EXPECT_FALSE(state.GetDynamicPKPState(kLongName, &pkp_state));
}

static bool AddHash(const std::string& type_and_base64, HashValueVector* out) {
  HashValue hash;
  if (!hash.FromString(type_and_base64))
    return false;

  out->push_back(hash);
  return true;
}

TEST_F(TransportSecurityStateTest, PinValidationWithoutRejectedCerts) {
  HashValueVector good_hashes, bad_hashes;

  for (size_t i = 0; kGoodPath[i]; i++) {
    EXPECT_TRUE(AddHash(kGoodPath[i], &good_hashes));
  }
  for (size_t i = 0; kBadPath[i]; i++) {
    EXPECT_TRUE(AddHash(kBadPath[i], &bad_hashes));
  }

  TransportSecurityState state;
  EnableStaticPins(&state);

  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;
  EXPECT_TRUE(state.GetStaticDomainState("no-rejected-pins-pkp.preloaded.test",
                                         &sts_state, &pkp_state));
  EXPECT_TRUE(pkp_state.HasPublicKeyPins());

  std::string failure_log;
  EXPECT_TRUE(pkp_state.CheckPublicKeyPins(good_hashes, &failure_log));
  EXPECT_FALSE(pkp_state.CheckPublicKeyPins(bad_hashes, &failure_log));
}

TEST_F(TransportSecurityStateTest, HPKPReporting) {
  HostPortPair host_port_pair(kHost, kPort);
  HostPortPair subdomain_host_port_pair(kSubdomain, kPort);
  GURL report_uri(kReportUri);
  // Two dummy certs to use as the server-sent and validated chains. The
  // contents don't matter.
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);

  HashValueVector good_hashes, bad_hashes;

  for (size_t i = 0; kGoodPath[i]; i++)
    EXPECT_TRUE(AddHash(kGoodPath[i], &good_hashes));
  for (size_t i = 0; kBadPath[i]; i++)
    EXPECT_TRUE(AddHash(kBadPath[i], &bad_hashes));

  TransportSecurityState state;
  MockCertificateReportSender mock_report_sender;
  state.SetReportSender(&mock_report_sender);

  const base::Time current_time = base::Time::Now();
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  state.AddHPKP(kHost, expiry, true, good_hashes, report_uri);

  EXPECT_EQ(GURL(), mock_report_sender.latest_report_uri());
  EXPECT_EQ(std::string(), mock_report_sender.latest_report());

  std::string failure_log;
  EXPECT_EQ(TransportSecurityState::PKPStatus::VIOLATED,
            state.CheckPublicKeyPins(
                host_port_pair, true, bad_hashes, cert1.get(), cert2.get(),
                TransportSecurityState::DISABLE_PIN_REPORTS, &failure_log));

  // No report should have been sent because of the DISABLE_PIN_REPORTS
  // argument.
  EXPECT_EQ(GURL(), mock_report_sender.latest_report_uri());
  EXPECT_EQ(std::string(), mock_report_sender.latest_report());

  EXPECT_EQ(TransportSecurityState::PKPStatus::OK,
            state.CheckPublicKeyPins(
                host_port_pair, true, good_hashes, cert1.get(), cert2.get(),
                TransportSecurityState::ENABLE_PIN_REPORTS, &failure_log));

  // No report should have been sent because there was no violation.
  EXPECT_EQ(GURL(), mock_report_sender.latest_report_uri());
  EXPECT_EQ(std::string(), mock_report_sender.latest_report());

  EXPECT_EQ(TransportSecurityState::PKPStatus::BYPASSED,
            state.CheckPublicKeyPins(
                host_port_pair, false, bad_hashes, cert1.get(), cert2.get(),
                TransportSecurityState::ENABLE_PIN_REPORTS, &failure_log));

  // No report should have been sent because the certificate chained to a
  // non-public root.
  EXPECT_EQ(GURL(), mock_report_sender.latest_report_uri());
  EXPECT_EQ(std::string(), mock_report_sender.latest_report());

  EXPECT_EQ(TransportSecurityState::PKPStatus::OK,
            state.CheckPublicKeyPins(
                host_port_pair, false, good_hashes, cert1.get(), cert2.get(),
                TransportSecurityState::ENABLE_PIN_REPORTS, &failure_log));

  // No report should have been sent because there was no violation, even though
  // the certificate chained to a local trust anchor.
  EXPECT_EQ(GURL(), mock_report_sender.latest_report_uri());
  EXPECT_EQ(std::string(), mock_report_sender.latest_report());

  EXPECT_EQ(TransportSecurityState::PKPStatus::VIOLATED,
            state.CheckPublicKeyPins(
                host_port_pair, true, bad_hashes, cert1.get(), cert2.get(),
                TransportSecurityState::ENABLE_PIN_REPORTS, &failure_log));

  // Now a report should have been sent. Check that it contains the
  // right information.
  EXPECT_EQ(report_uri, mock_report_sender.latest_report_uri());
  std::string report = mock_report_sender.latest_report();
  ASSERT_FALSE(report.empty());
  EXPECT_EQ("application/json; charset=utf-8",
            mock_report_sender.latest_content_type());
  ASSERT_NO_FATAL_FAILURE(CheckHPKPReport(report, host_port_pair, true, kHost,
                                          cert1.get(), cert2.get(),
                                          good_hashes));
  mock_report_sender.Clear();
  EXPECT_EQ(TransportSecurityState::PKPStatus::VIOLATED,
            state.CheckPublicKeyPins(subdomain_host_port_pair, true, bad_hashes,
                                     cert1.get(), cert2.get(),
                                     TransportSecurityState::ENABLE_PIN_REPORTS,
                                     &failure_log));

  // Now a report should have been sent for the subdomain. Check that it
  // contains the right information.
  EXPECT_EQ(report_uri, mock_report_sender.latest_report_uri());
  report = mock_report_sender.latest_report();
  ASSERT_FALSE(report.empty());
  EXPECT_EQ("application/json; charset=utf-8",
            mock_report_sender.latest_content_type());
  ASSERT_NO_FATAL_FAILURE(CheckHPKPReport(report, subdomain_host_port_pair,
                                          true, kHost, cert1.get(), cert2.get(),
                                          good_hashes));
}

// Tests that a histogram entry is recorded when TransportSecurityState
// fails to send an HPKP violation report.
TEST_F(TransportSecurityStateTest, UMAOnHPKPReportingFailure) {
  base::HistogramTester histograms;
  const std::string histogram_name = "Net.PublicKeyPinReportSendingFailure2";
  HostPortPair host_port_pair(kHost, kPort);
  GURL report_uri(kReportUri);
  // Two dummy certs to use as the server-sent and validated chains. The
  // contents don't matter.
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);

  HashValueVector good_hashes, bad_hashes;

  for (size_t i = 0; kGoodPath[i]; i++)
    EXPECT_TRUE(AddHash(kGoodPath[i], &good_hashes));
  for (size_t i = 0; kBadPath[i]; i++)
    EXPECT_TRUE(AddHash(kBadPath[i], &bad_hashes));

  // The histogram should start off empty.
  histograms.ExpectTotalCount(histogram_name, 0);

  TransportSecurityState state;
  MockFailingCertificateReportSender mock_report_sender;
  state.SetReportSender(&mock_report_sender);

  const base::Time current_time = base::Time::Now();
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  state.AddHPKP(kHost, expiry, true, good_hashes, report_uri);

  std::string failure_log;
  EXPECT_EQ(TransportSecurityState::PKPStatus::VIOLATED,
            state.CheckPublicKeyPins(
                host_port_pair, true, bad_hashes, cert1.get(), cert2.get(),
                TransportSecurityState::ENABLE_PIN_REPORTS, &failure_log));

  // Check that the UMA histogram was updated when the report failed to
  // send.
  histograms.ExpectTotalCount(histogram_name, 1);
  histograms.ExpectBucketCount(histogram_name, -mock_report_sender.net_error(),
                               1);
}

TEST_F(TransportSecurityStateTest, HPKPReportOnly) {
  HostPortPair host_port_pair(kHost, kPort);
  GURL report_uri(kReportUri);
  // Two dummy certs to use as the server-sent and validated chains. The
  // contents don't matter.
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);

  TransportSecurityState state;
  MockCertificateReportSender mock_report_sender;
  state.SetReportSender(&mock_report_sender);

  SSLInfo ssl_info;
  ssl_info.is_issued_by_known_root = true;
  ssl_info.unverified_cert = cert1;
  ssl_info.cert = cert2;
  for (size_t i = 0; kGoodPath[i]; i++)
    EXPECT_TRUE(AddHash(kGoodPath[i], &ssl_info.public_key_hashes));

  // HTTPS report URIs on the same host as the pin violation should not
  // be allowed, to avoid going into a report-sending loop.
  std::string header = "pin-sha256=\"" + std::string(kGoodPin1) +
                       "\";pin-sha256=\"" + std::string(kGoodPin2) +
                       "\";pin-sha256=\"" + std::string(kGoodPin3) +
                       "\";report-uri=\"https://" + host_port_pair.host() +
                       "/report\";includeSubdomains";
  EXPECT_TRUE(
      state.ProcessHPKPReportOnlyHeader(header, host_port_pair, ssl_info));
  EXPECT_TRUE(mock_report_sender.latest_report_uri().is_empty());

  // Check that a report is not sent for a Report-Only header with no
  // violation.
  mock_report_sender.Clear();
  header = "pin-sha256=\"" + std::string(kGoodPin1) + "\";pin-sha256=\"" +
           std::string(kGoodPin2) + "\";pin-sha256=\"" +
           std::string(kGoodPin3) + "\";report-uri=\"" + report_uri.spec() +
           "\";includeSubdomains";

  EXPECT_TRUE(
      state.ProcessHPKPReportOnlyHeader(header, host_port_pair, ssl_info));
  EXPECT_EQ(GURL(), mock_report_sender.latest_report_uri());
  EXPECT_EQ(std::string(), mock_report_sender.latest_report());

  // Check that a report is sent for a Report-Only header with a
  // violation.
  ssl_info.public_key_hashes.clear();
  for (size_t i = 0; kBadPath[i]; i++)
    EXPECT_TRUE(AddHash(kBadPath[i], &ssl_info.public_key_hashes));

  EXPECT_TRUE(
      state.ProcessHPKPReportOnlyHeader(header, host_port_pair, ssl_info));
  EXPECT_EQ(report_uri, mock_report_sender.latest_report_uri());
  std::string report = mock_report_sender.latest_report();
  ASSERT_FALSE(report.empty());
  EXPECT_EQ("application/json; charset=utf-8",
            mock_report_sender.latest_content_type());
  ASSERT_NO_FATAL_FAILURE(CheckHPKPReport(report, host_port_pair, true, kHost,
                                          cert1.get(), cert2.get(),
                                          ssl_info.public_key_hashes));
}

// Tests that Report-Only reports are not sent on certs that chain to
// local roots.
TEST_F(TransportSecurityStateTest, HPKPReportOnlyOnLocalRoot) {
  HostPortPair host_port_pair(kHost, kPort);
  GURL report_uri(kReportUri);
  // Two dummy certs to use as the server-sent and validated chains. The
  // contents don't matter.
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);

  std::string header =
      "pin-sha256=\"" + std::string(kGoodPin1) + "\";pin-sha256=\"" +
      std::string(kGoodPin2) + "\";pin-sha256=\"" + std::string(kGoodPin3) +
      "\";report-uri=\"" + report_uri.spec() + "\";includeSubdomains";

  TransportSecurityState state;
  MockCertificateReportSender mock_report_sender;
  state.SetReportSender(&mock_report_sender);

  SSLInfo ssl_info;
  ssl_info.is_issued_by_known_root = true;
  ssl_info.unverified_cert = cert1;
  ssl_info.cert = cert2;
  for (size_t i = 0; kGoodPath[i]; i++)
    EXPECT_TRUE(AddHash(kGoodPath[i], &ssl_info.public_key_hashes));
  ssl_info.is_issued_by_known_root = false;

  EXPECT_TRUE(
      state.ProcessHPKPReportOnlyHeader(header, host_port_pair, ssl_info));
  EXPECT_EQ(GURL(), mock_report_sender.latest_report_uri());
  EXPECT_EQ(std::string(), mock_report_sender.latest_report());
}

// Tests that ProcessHPKPReportOnlyHeader() returns false if a report-uri
// wasn't specified or if the header fails to parse.
TEST_F(TransportSecurityStateTest, HPKPReportOnlyParseErrors) {
  HostPortPair host_port_pair(kHost, kPort);
  GURL report_uri(kReportUri);
  // Two dummy certs to use as the server-sent and validated chains. The
  // contents don't matter.
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);

  std::string header = "pin-sha256=\"" + std::string(kGoodPin1) +
                       "\";pin-sha256=\"" + std::string(kGoodPin2) +
                       "\";pin-sha256=\"" + std::string(kGoodPin3) + "\"";

  TransportSecurityState state;
  MockCertificateReportSender mock_report_sender;
  state.SetReportSender(&mock_report_sender);

  SSLInfo ssl_info;
  ssl_info.is_issued_by_known_root = true;
  ssl_info.unverified_cert = cert1;
  ssl_info.cert = cert2;
  for (size_t i = 0; kGoodPath[i]; i++)
    EXPECT_TRUE(AddHash(kGoodPath[i], &ssl_info.public_key_hashes));

  EXPECT_FALSE(
      state.ProcessHPKPReportOnlyHeader(header, host_port_pair, ssl_info));
  header += ";report-uri=\"";
  EXPECT_FALSE(
      state.ProcessHPKPReportOnlyHeader(header, host_port_pair, ssl_info));
}

// Tests that pinning violations on preloaded pins trigger reports when
// the preloaded pin contains a report URI.
TEST_F(TransportSecurityStateTest, PreloadedPKPReportUri) {
  const char kPreloadedPinDomain[] = "with-report-uri-pkp.preloaded.test";
  const uint16_t kPort = 443;
  HostPortPair host_port_pair(kPreloadedPinDomain, kPort);

  TransportSecurityState state;
  MockCertificateReportSender mock_report_sender;
  state.SetReportSender(&mock_report_sender);

  EnableStaticPins(&state);

  TransportSecurityState::PKPState pkp_state;
  TransportSecurityState::STSState unused_sts_state;
  ASSERT_TRUE(state.GetStaticDomainState(kPreloadedPinDomain, &unused_sts_state,
                                         &pkp_state));
  ASSERT_TRUE(pkp_state.HasPublicKeyPins());

  GURL report_uri = pkp_state.report_uri;
  ASSERT_TRUE(report_uri.is_valid());
  ASSERT_FALSE(report_uri.is_empty());

  // Two dummy certs to use as the server-sent and validated chains. The
  // contents don't matter, as long as they are not the real google.com
  // certs in the pins.
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);

  HashValueVector bad_hashes;
  for (size_t i = 0; kBadPath[i]; i++)
    EXPECT_TRUE(AddHash(kBadPath[i], &bad_hashes));

  // Trigger a violation and check that it sends a report.
  std::string failure_log;
  EXPECT_EQ(TransportSecurityState::PKPStatus::VIOLATED,
            state.CheckPublicKeyPins(
                host_port_pair, true, bad_hashes, cert1.get(), cert2.get(),
                TransportSecurityState::ENABLE_PIN_REPORTS, &failure_log));

  EXPECT_EQ(report_uri, mock_report_sender.latest_report_uri());

  std::string report = mock_report_sender.latest_report();
  ASSERT_FALSE(report.empty());
  EXPECT_EQ("application/json; charset=utf-8",
            mock_report_sender.latest_content_type());
  ASSERT_NO_FATAL_FAILURE(CheckHPKPReport(
      report, host_port_pair, pkp_state.include_subdomains, pkp_state.domain,
      cert1.get(), cert2.get(), pkp_state.spki_hashes));
}

// Tests that report URIs are thrown out if they point to the same host,
// over HTTPS, for which a pin was violated.
TEST_F(TransportSecurityStateTest, HPKPReportUriToSameHost) {
  HostPortPair host_port_pair(kHost, kPort);
  GURL https_report_uri("https://example.test/report");
  GURL http_report_uri("http://example.test/report");
  TransportSecurityState state;
  MockCertificateReportSender mock_report_sender;
  state.SetReportSender(&mock_report_sender);

  const base::Time current_time = base::Time::Now();
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  HashValueVector good_hashes;
  for (size_t i = 0; kGoodPath[i]; i++)
    EXPECT_TRUE(AddHash(kGoodPath[i], &good_hashes));

  // Two dummy certs to use as the server-sent and validated chains. The
  // contents don't matter, as long as they don't match the certs in the pins.
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);

  HashValueVector bad_hashes;
  for (size_t i = 0; kBadPath[i]; i++)
    EXPECT_TRUE(AddHash(kBadPath[i], &bad_hashes));

  state.AddHPKP(kHost, expiry, true, good_hashes, https_report_uri);

  // Trigger a violation and check that it does not send a report
  // because the report-uri is HTTPS and same-host as the pins.
  std::string failure_log;
  EXPECT_EQ(TransportSecurityState::PKPStatus::VIOLATED,
            state.CheckPublicKeyPins(
                host_port_pair, true, bad_hashes, cert1.get(), cert2.get(),
                TransportSecurityState::ENABLE_PIN_REPORTS, &failure_log));

  EXPECT_TRUE(mock_report_sender.latest_report_uri().is_empty());

  // An HTTP report uri to the same host should be okay.
  state.AddHPKP("example.test", expiry, true, good_hashes, http_report_uri);
  EXPECT_EQ(TransportSecurityState::PKPStatus::VIOLATED,
            state.CheckPublicKeyPins(
                host_port_pair, true, bad_hashes, cert1.get(), cert2.get(),
                TransportSecurityState::ENABLE_PIN_REPORTS, &failure_log));

  EXPECT_EQ(http_report_uri, mock_report_sender.latest_report_uri());
}

// Tests that redundant reports are rate-limited.
TEST_F(TransportSecurityStateTest, HPKPReportRateLimiting) {
  HostPortPair host_port_pair(kHost, kPort);
  HostPortPair subdomain_host_port_pair(kSubdomain, kPort);
  GURL report_uri(kReportUri);
  // Two dummy certs to use as the server-sent and validated chains. The
  // contents don't matter.
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);

  HashValueVector good_hashes, bad_hashes;

  for (size_t i = 0; kGoodPath[i]; i++)
    EXPECT_TRUE(AddHash(kGoodPath[i], &good_hashes));
  for (size_t i = 0; kBadPath[i]; i++)
    EXPECT_TRUE(AddHash(kBadPath[i], &bad_hashes));

  TransportSecurityState state;
  MockCertificateReportSender mock_report_sender;
  state.SetReportSender(&mock_report_sender);

  const base::Time current_time = base::Time::Now();
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  state.AddHPKP(kHost, expiry, true, good_hashes, report_uri);

  EXPECT_EQ(GURL(), mock_report_sender.latest_report_uri());
  EXPECT_EQ(std::string(), mock_report_sender.latest_report());

  std::string failure_log;
  EXPECT_EQ(TransportSecurityState::PKPStatus::VIOLATED,
            state.CheckPublicKeyPins(
                host_port_pair, true, bad_hashes, cert1.get(), cert2.get(),
                TransportSecurityState::ENABLE_PIN_REPORTS, &failure_log));

  // A report should have been sent. Check that it contains the
  // right information.
  EXPECT_EQ(report_uri, mock_report_sender.latest_report_uri());
  std::string report = mock_report_sender.latest_report();
  ASSERT_FALSE(report.empty());
  ASSERT_NO_FATAL_FAILURE(CheckHPKPReport(report, host_port_pair, true, kHost,
                                          cert1.get(), cert2.get(),
                                          good_hashes));
  mock_report_sender.Clear();

  // Now trigger the same violation; a duplicative report should not be
  // sent.
  EXPECT_EQ(TransportSecurityState::PKPStatus::VIOLATED,
            state.CheckPublicKeyPins(
                host_port_pair, true, bad_hashes, cert1.get(), cert2.get(),
                TransportSecurityState::ENABLE_PIN_REPORTS, &failure_log));
  EXPECT_EQ(GURL(), mock_report_sender.latest_report_uri());
  EXPECT_EQ(std::string(), mock_report_sender.latest_report());

  // Trigger the same violation but with a different report-uri: it
  // should be sent.
  GURL report_uri2("http://report-example2.test/test");
  state.AddHPKP(kHost, expiry, true, good_hashes, report_uri2);
  EXPECT_EQ(TransportSecurityState::PKPStatus::VIOLATED,
            state.CheckPublicKeyPins(
                host_port_pair, true, bad_hashes, cert1.get(), cert2.get(),
                TransportSecurityState::ENABLE_PIN_REPORTS, &failure_log));
  EXPECT_EQ(report_uri2, mock_report_sender.latest_report_uri());
  report = mock_report_sender.latest_report();
  ASSERT_FALSE(report.empty());
  ASSERT_NO_FATAL_FAILURE(CheckHPKPReport(report, host_port_pair, true, kHost,
                                          cert1.get(), cert2.get(),
                                          good_hashes));
  mock_report_sender.Clear();
}

// Tests that static (preloaded) expect CT state is read correctly.
TEST_F(TransportSecurityStateTest, PreloadedExpectCT) {
  TransportSecurityState state;
  TransportSecurityStateTest::EnableStaticExpectCT(&state);
  TransportSecurityState::ExpectCTState expect_ct_state;
  EXPECT_TRUE(
      GetExpectCTState(&state, kExpectCTStaticHostname, &expect_ct_state));
  EXPECT_EQ(kExpectCTStaticHostname, expect_ct_state.domain);
  EXPECT_EQ(GURL(kExpectCTStaticReportURI), expect_ct_state.report_uri);
  EXPECT_FALSE(
      GetExpectCTState(&state, "hsts-preloaded.test", &expect_ct_state));
}

// Tests that static (preloaded) expect staple state is read correctly.
TEST_F(TransportSecurityStateTest, PreloadedExpectStaple) {
  TransportSecurityState state;
  TransportSecurityState::ExpectStapleState expect_staple_state;
  TransportSecurityStateTest::SetEnableStaticExpectStaple(&state, false);
  EXPECT_FALSE(GetExpectStapleState(&state, kExpectStapleStaticHostname,
                                    &expect_staple_state));
  TransportSecurityStateTest::SetEnableStaticExpectStaple(&state, true);
  EXPECT_TRUE(GetExpectStapleState(&state, kExpectStapleStaticHostname,
                                   &expect_staple_state));
  EXPECT_EQ(kExpectStapleStaticHostname, expect_staple_state.domain);
  EXPECT_EQ(GURL(kExpectStapleStaticReportURI), expect_staple_state.report_uri);
  EXPECT_FALSE(expect_staple_state.include_subdomains);
  EXPECT_FALSE(GetExpectStapleState(&state, "hsts-preloaded.test",
                                    &expect_staple_state));
  std::string subdomain = "subdomain.";
  subdomain += kExpectStapleStaticHostname;
  EXPECT_FALSE(GetExpectStapleState(&state, subdomain, &expect_staple_state));
}

TEST_F(TransportSecurityStateTest, PreloadedExpectStapleIncludeSubdomains) {
  TransportSecurityState state;
  TransportSecurityStateTest::SetEnableStaticExpectStaple(&state, true);
  TransportSecurityState::ExpectStapleState expect_staple_state;
  std::string subdomain = "subdomain.";
  subdomain += kExpectStapleStaticIncludeSubdomainsHostname;
  EXPECT_TRUE(GetExpectStapleState(&state, subdomain, &expect_staple_state));
  EXPECT_EQ(kExpectStapleStaticIncludeSubdomainsHostname,
            expect_staple_state.domain);
  EXPECT_TRUE(expect_staple_state.include_subdomains);
  EXPECT_EQ(GURL(kExpectStapleStaticReportURI), expect_staple_state.report_uri);
}

// Tests that the Expect CT reporter is not notified for invalid or absent
// header values.
TEST_F(TransportSecurityStateTest, InvalidExpectCTHeader) {
  HostPortPair host_port(kExpectCTStaticHostname, 443);
  SSLInfo ssl_info;
  ssl_info.ct_compliance_details_available = true;
  ssl_info.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS;
  ssl_info.is_issued_by_known_root = true;
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);
  ssl_info.unverified_cert = cert1;
  ssl_info.cert = cert2;

  TransportSecurityState state;
  TransportSecurityStateTest::EnableStaticExpectCT(&state);
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.ProcessExpectCTHeader("", host_port, ssl_info);
  EXPECT_EQ(0u, reporter.num_failures());

  state.ProcessExpectCTHeader("blah blah", host_port, ssl_info);
  EXPECT_EQ(0u, reporter.num_failures());

  state.ProcessExpectCTHeader("preload", host_port, ssl_info);
  EXPECT_EQ(1u, reporter.num_failures());
}

// Tests that the Expect CT reporter is only notified about certificates
// chaining to public roots.
TEST_F(TransportSecurityStateTest, ExpectCTNonPublicRoot) {
  HostPortPair host_port(kExpectCTStaticHostname, 443);
  SSLInfo ssl_info;
  ssl_info.ct_compliance_details_available = true;
  ssl_info.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS;
  ssl_info.is_issued_by_known_root = false;
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);
  ssl_info.unverified_cert = cert1;
  ssl_info.cert = cert2;

  TransportSecurityState state;
  TransportSecurityStateTest::EnableStaticExpectCT(&state);
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.ProcessExpectCTHeader("preload", host_port, ssl_info);
  EXPECT_EQ(0u, reporter.num_failures());

  ssl_info.is_issued_by_known_root = true;
  state.ProcessExpectCTHeader("preload", host_port, ssl_info);
  EXPECT_EQ(1u, reporter.num_failures());
}

// Tests that the Expect CT reporter is not notified when compliance
// details aren't available.
TEST_F(TransportSecurityStateTest, ExpectCTComplianceNotAvailable) {
  HostPortPair host_port(kExpectCTStaticHostname, 443);
  SSLInfo ssl_info;
  ssl_info.ct_compliance_details_available = false;
  ssl_info.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS;
  ssl_info.is_issued_by_known_root = true;
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);
  ssl_info.unverified_cert = cert1;
  ssl_info.cert = cert2;

  TransportSecurityState state;
  TransportSecurityStateTest::EnableStaticExpectCT(&state);
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.ProcessExpectCTHeader("preload", host_port, ssl_info);
  EXPECT_EQ(0u, reporter.num_failures());

  ssl_info.ct_compliance_details_available = true;
  state.ProcessExpectCTHeader("preload", host_port, ssl_info);
  EXPECT_EQ(1u, reporter.num_failures());
}

// Tests that the Expect CT reporter is not notified about compliant
// connections.
TEST_F(TransportSecurityStateTest, ExpectCTCompliantCert) {
  HostPortPair host_port(kExpectCTStaticHostname, 443);
  SSLInfo ssl_info;
  ssl_info.ct_compliance_details_available = true;
  ssl_info.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_COMPLIES_VIA_SCTS;
  ssl_info.is_issued_by_known_root = true;
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);
  ssl_info.unverified_cert = cert1;
  ssl_info.cert = cert2;

  TransportSecurityState state;
  TransportSecurityStateTest::EnableStaticExpectCT(&state);
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.ProcessExpectCTHeader("preload", host_port, ssl_info);
  EXPECT_EQ(0u, reporter.num_failures());

  ssl_info.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_NOT_DIVERSE_SCTS;
  state.ProcessExpectCTHeader("preload", host_port, ssl_info);
  EXPECT_EQ(1u, reporter.num_failures());
}

// Tests that the Expect CT reporter is not notified for preloaded Expect-CT
// when the build is not timely.
TEST_F(TransportSecurityStateTest, PreloadedExpectCTBuildNotTimely) {
  HostPortPair host_port(kExpectCTStaticHostname, 443);
  SSLInfo ssl_info;
  ssl_info.ct_compliance_details_available = true;
  ssl_info.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_BUILD_NOT_TIMELY;
  ssl_info.is_issued_by_known_root = true;
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);
  ssl_info.unverified_cert = cert1;
  ssl_info.cert = cert2;

  TransportSecurityState state;
  TransportSecurityStateTest::EnableStaticExpectCT(&state);
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.ProcessExpectCTHeader("preload", host_port, ssl_info);
  EXPECT_EQ(0u, reporter.num_failures());

  // Sanity-check that the reporter is notified if the build is timely and the
  // connection is not compliant.
  ssl_info.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_NOT_DIVERSE_SCTS;
  state.ProcessExpectCTHeader("preload", host_port, ssl_info);
  EXPECT_EQ(1u, reporter.num_failures());
}

// Tests that the Expect CT reporter is not notified for dynamic Expect-CT when
// the build is not timely.
TEST_F(TransportSecurityStateTest, DynamicExpectCTBuildNotTimely) {
  HostPortPair host_port("example.test", 443);
  SSLInfo ssl_info;
  ssl_info.ct_compliance_details_available = true;
  ssl_info.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_BUILD_NOT_TIMELY;
  ssl_info.is_issued_by_known_root = true;
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);
  ssl_info.unverified_cert = cert1;
  ssl_info.cert = cert2;

  TransportSecurityState state;
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  const char kHeader[] = "max-age=10, report-uri=http://report.test";
  state.ProcessExpectCTHeader(kHeader, host_port, ssl_info);

  // No report should have been sent and the state should not have been saved.
  EXPECT_EQ(0u, reporter.num_failures());
  TransportSecurityState::ExpectCTState expect_ct_state;
  EXPECT_FALSE(state.GetDynamicExpectCTState("example.test", &expect_ct_state));

  // Sanity-check that the reporter is notified if the build is timely and the
  // connection is not compliant.
  ssl_info.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_NOT_DIVERSE_SCTS;
  state.ProcessExpectCTHeader(kHeader, host_port, ssl_info);
  EXPECT_EQ(1u, reporter.num_failures());
}

// Tests that the Expect CT reporter is not notified for a site that
// isn't preloaded.
TEST_F(TransportSecurityStateTest, ExpectCTNotPreloaded) {
  HostPortPair host_port("not-expect-ct-preloaded.test", 443);
  SSLInfo ssl_info;
  ssl_info.ct_compliance_details_available = true;
  ssl_info.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_NOT_DIVERSE_SCTS;
  ssl_info.is_issued_by_known_root = true;
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);
  ssl_info.unverified_cert = cert1;
  ssl_info.cert = cert2;

  TransportSecurityState state;
  TransportSecurityStateTest::EnableStaticExpectCT(&state);
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.ProcessExpectCTHeader("preload", host_port, ssl_info);
  EXPECT_EQ(0u, reporter.num_failures());

  host_port.set_host(kExpectCTStaticHostname);
  state.ProcessExpectCTHeader("preload", host_port, ssl_info);
  EXPECT_EQ(1u, reporter.num_failures());
}

// Tests that the Expect CT reporter is notified for noncompliant
// connections.
TEST_F(TransportSecurityStateTest, ExpectCTReporter) {
  HostPortPair host_port(kExpectCTStaticHostname, 443);
  SSLInfo ssl_info;
  ssl_info.ct_compliance_details_available = true;
  ssl_info.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_NOT_DIVERSE_SCTS;
  ssl_info.is_issued_by_known_root = true;
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert1);
  ASSERT_TRUE(cert2);
  ssl_info.unverified_cert = cert1;
  ssl_info.cert = cert2;
  MakeTestSCTAndStatus(ct::SignedCertificateTimestamp::SCT_EMBEDDED, "test_log",
                       std::string(), std::string(), base::Time::Now(),
                       ct::SCT_STATUS_INVALID_SIGNATURE,
                       &ssl_info.signed_certificate_timestamps);

  TransportSecurityState state;
  TransportSecurityStateTest::EnableStaticExpectCT(&state);
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.ProcessExpectCTHeader("preload", host_port, ssl_info);
  EXPECT_EQ(1u, reporter.num_failures());
  EXPECT_EQ(host_port.host(), reporter.host_port_pair().host());
  EXPECT_EQ(host_port.port(), reporter.host_port_pair().port());
  EXPECT_TRUE(reporter.expiration().is_null());
  EXPECT_EQ(GURL(kExpectCTStaticReportURI), reporter.report_uri());
  EXPECT_EQ(cert1.get(), reporter.served_certificate_chain());
  EXPECT_EQ(cert2.get(), reporter.validated_certificate_chain());
  EXPECT_EQ(ssl_info.signed_certificate_timestamps.size(),
            reporter.signed_certificate_timestamps().size());
  EXPECT_EQ(ssl_info.signed_certificate_timestamps[0].status,
            reporter.signed_certificate_timestamps()[0].status);
  EXPECT_EQ(ssl_info.signed_certificate_timestamps[0].sct,
            reporter.signed_certificate_timestamps()[0].sct);
}

// Tests that the Expect CT reporter is not notified for repeated noncompliant
// connections to the same preloaded host.
TEST_F(TransportSecurityStateTest, RepeatedExpectCTReportsForStaticExpectCT) {
  HostPortPair host_port(kExpectCTStaticHostname, 443);
  SSLInfo ssl_info;
  ssl_info.ct_compliance_details_available = true;
  ssl_info.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_NOT_DIVERSE_SCTS;
  ssl_info.is_issued_by_known_root = true;
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);
  ssl_info.unverified_cert = cert1;
  ssl_info.cert = cert2;
  MakeTestSCTAndStatus(ct::SignedCertificateTimestamp::SCT_EMBEDDED, "test_log",
                       std::string(), std::string(), base::Time::Now(),
                       ct::SCT_STATUS_INVALID_SIGNATURE,
                       &ssl_info.signed_certificate_timestamps);

  TransportSecurityState state;
  TransportSecurityStateTest::EnableStaticExpectCT(&state);
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.ProcessExpectCTHeader("preload", host_port, ssl_info);
  EXPECT_EQ(1u, reporter.num_failures());

  // After processing a second header, the report should not be sent again.
  state.ProcessExpectCTHeader("preload", host_port, ssl_info);
  EXPECT_EQ(1u, reporter.num_failures());
}

// Simple test for the HSTS preload process. The trie (generated from
// transport_security_state_static_unittest1.json) contains 1 entry. Test that
// the lookup methods can find the entry and correctly decode the different
// preloaded states (HSTS, HPKP, Expect-CT, and Expect-Staple).
TEST_F(TransportSecurityStateTest, DecodePreloadedSingle) {
  SetTransportSecurityStateSourceForTesting(&test1::kHSTSSource);

  TransportSecurityState state;
  TransportSecurityStateTest::EnableStaticPins(&state);
  TransportSecurityStateTest::EnableStaticExpectCT(&state);
  TransportSecurityStateTest::SetEnableStaticExpectStaple(&state, true);

  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;
  EXPECT_TRUE(
      GetStaticDomainState(&state, "hsts.example.com", &sts_state, &pkp_state));
  EXPECT_TRUE(sts_state.include_subdomains);
  EXPECT_EQ(TransportSecurityState::STSState::MODE_FORCE_HTTPS,
            sts_state.upgrade_mode);
  EXPECT_TRUE(pkp_state.include_subdomains);
  EXPECT_EQ(GURL(), pkp_state.report_uri);
  ASSERT_EQ(1u, pkp_state.spki_hashes.size());
  EXPECT_EQ(pkp_state.spki_hashes[0], GetSampleSPKIHash(0x1));
  ASSERT_EQ(1u, pkp_state.bad_spki_hashes.size());
  EXPECT_EQ(pkp_state.bad_spki_hashes[0], GetSampleSPKIHash(0x2));

  TransportSecurityState::ExpectCTState ct_state;
  EXPECT_FALSE(GetExpectCTState(&state, "hsts.example.com", &ct_state));

  TransportSecurityState::ExpectStapleState staple_state;
  EXPECT_FALSE(GetExpectStapleState(&state, "hsts.example.com", &staple_state));
}

// More advanced test for the HSTS preload process where the trie (generated
// from transport_security_state_static_unittest2.json) contains multiple
// entries with a common prefix. Test that the lookup methods can find all
// entries and correctly decode the different preloaded states (HSTS, HPKP,
// Expect-CT, and Expect-Staple) for each entry.
TEST_F(TransportSecurityStateTest, DecodePreloadedMultiplePrefix) {
  SetTransportSecurityStateSourceForTesting(&test2::kHSTSSource);

  TransportSecurityState state;
  TransportSecurityStateTest::EnableStaticPins(&state);
  TransportSecurityStateTest::EnableStaticExpectCT(&state);
  TransportSecurityStateTest::SetEnableStaticExpectStaple(&state, true);

  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;
  TransportSecurityState::ExpectCTState ct_state;
  TransportSecurityState::ExpectStapleState staple_state;

  EXPECT_TRUE(
      GetStaticDomainState(&state, "hsts.example.com", &sts_state, &pkp_state));
  EXPECT_FALSE(sts_state.include_subdomains);
  EXPECT_EQ(TransportSecurityState::STSState::MODE_FORCE_HTTPS,
            sts_state.upgrade_mode);
  EXPECT_FALSE(pkp_state.include_subdomains);
  EXPECT_EQ(GURL(), pkp_state.report_uri);
  EXPECT_EQ(0U, pkp_state.spki_hashes.size());
  EXPECT_EQ(0U, pkp_state.bad_spki_hashes.size());
  EXPECT_FALSE(GetExpectCTState(&state, "hsts.example.com", &ct_state));
  EXPECT_FALSE(GetExpectStapleState(&state, "hsts.example.com", &staple_state));

  sts_state = TransportSecurityState::STSState();
  pkp_state = TransportSecurityState::PKPState();
  ct_state = TransportSecurityState::ExpectCTState();
  staple_state = TransportSecurityState::ExpectStapleState();
  EXPECT_TRUE(
      GetStaticDomainState(&state, "hpkp.example.com", &sts_state, &pkp_state));
  EXPECT_FALSE(sts_state.include_subdomains);
  EXPECT_EQ(TransportSecurityState::STSState::MODE_DEFAULT,
            sts_state.upgrade_mode);
  EXPECT_TRUE(pkp_state.include_subdomains);
  EXPECT_EQ(GURL("https://report.example.com/hpkp-upload"),
            pkp_state.report_uri);
  EXPECT_EQ(1U, pkp_state.spki_hashes.size());
  EXPECT_EQ(pkp_state.spki_hashes[0], GetSampleSPKIHash(0x1));
  EXPECT_EQ(0U, pkp_state.bad_spki_hashes.size());
  EXPECT_FALSE(GetExpectCTState(&state, "hpkp.example.com", &ct_state));
  EXPECT_FALSE(GetExpectStapleState(&state, "hpkp.example.com", &staple_state));

  sts_state = TransportSecurityState::STSState();
  pkp_state = TransportSecurityState::PKPState();
  ct_state = TransportSecurityState::ExpectCTState();
  staple_state = TransportSecurityState::ExpectStapleState();
  EXPECT_TRUE(GetStaticDomainState(&state, "expect-ct.example.com", &sts_state,
                                   &pkp_state));
  EXPECT_FALSE(sts_state.include_subdomains);
  EXPECT_EQ(TransportSecurityState::STSState::MODE_DEFAULT,
            sts_state.upgrade_mode);
  EXPECT_FALSE(pkp_state.include_subdomains);
  EXPECT_EQ(GURL(), pkp_state.report_uri);
  EXPECT_EQ(0U, pkp_state.spki_hashes.size());
  EXPECT_EQ(0U, pkp_state.bad_spki_hashes.size());
  EXPECT_TRUE(GetExpectCTState(&state, "expect-ct.example.com", &ct_state));
  EXPECT_EQ(GURL("https://report.example.com/ct-upload"), ct_state.report_uri);
  EXPECT_FALSE(
      GetExpectStapleState(&state, "expect-ct.example.com", &staple_state));

  sts_state = TransportSecurityState::STSState();
  pkp_state = TransportSecurityState::PKPState();
  ct_state = TransportSecurityState::ExpectCTState();
  staple_state = TransportSecurityState::ExpectStapleState();
  EXPECT_TRUE(GetStaticDomainState(&state, "expect-staple.example.com",
                                   &sts_state, &pkp_state));
  EXPECT_FALSE(sts_state.include_subdomains);
  EXPECT_EQ(TransportSecurityState::STSState::MODE_DEFAULT,
            sts_state.upgrade_mode);
  EXPECT_FALSE(pkp_state.include_subdomains);
  EXPECT_EQ(GURL(), pkp_state.report_uri);
  EXPECT_EQ(0U, pkp_state.spki_hashes.size());
  EXPECT_EQ(0U, pkp_state.bad_spki_hashes.size());
  EXPECT_FALSE(
      GetExpectCTState(&state, "expect-staple.example.com", &ct_state));
  EXPECT_TRUE(
      GetExpectStapleState(&state, "expect-staple.example.com", &staple_state));
  EXPECT_FALSE(staple_state.include_subdomains);
  EXPECT_EQ(GURL("https://report.example.com/staple-upload"),
            staple_state.report_uri);

  sts_state = TransportSecurityState::STSState();
  pkp_state = TransportSecurityState::PKPState();
  ct_state = TransportSecurityState::ExpectCTState();
  staple_state = TransportSecurityState::ExpectStapleState();
  EXPECT_TRUE(
      GetStaticDomainState(&state, "mix.example.com", &sts_state, &pkp_state));
  EXPECT_FALSE(sts_state.include_subdomains);
  EXPECT_EQ(TransportSecurityState::STSState::MODE_FORCE_HTTPS,
            sts_state.upgrade_mode);
  EXPECT_TRUE(pkp_state.include_subdomains);
  EXPECT_EQ(GURL(), pkp_state.report_uri);
  EXPECT_EQ(1U, pkp_state.spki_hashes.size());
  EXPECT_EQ(pkp_state.spki_hashes[0], GetSampleSPKIHash(0x2));
  EXPECT_EQ(1U, pkp_state.bad_spki_hashes.size());
  EXPECT_EQ(pkp_state.bad_spki_hashes[0], GetSampleSPKIHash(0x1));
  EXPECT_TRUE(GetExpectCTState(&state, "mix.example.com", &ct_state));
  EXPECT_EQ(GURL("https://report.example.com/ct-upload-alt"),
            ct_state.report_uri);
  EXPECT_TRUE(GetExpectStapleState(&state, "mix.example.com", &staple_state));
  EXPECT_TRUE(staple_state.include_subdomains);
  EXPECT_EQ(GURL("https://report.example.com/staple-upload-alt"),
            staple_state.report_uri);
}

// More advanced test for the HSTS preload process where the trie (generated
// from transport_security_state_static_unittest3.json) contains a mix of
// entries. Some entries share a prefix with the prefix also having its own
// preloaded state while others share no prefix. This results in a trie with
// several different internal structures. Test that the lookup methods can find
// all entries and correctly decode the different preloaded states (HSTS, HPKP,
// Expect-CT, and Expect-Staple) for each entry.
TEST_F(TransportSecurityStateTest, DecodePreloadedMultipleMix) {
  SetTransportSecurityStateSourceForTesting(&test3::kHSTSSource);

  TransportSecurityState state;
  TransportSecurityStateTest::EnableStaticPins(&state);
  TransportSecurityStateTest::EnableStaticExpectCT(&state);
  TransportSecurityStateTest::SetEnableStaticExpectStaple(&state, true);

  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;
  TransportSecurityState::ExpectCTState ct_state;
  TransportSecurityState::ExpectStapleState staple_state;

  EXPECT_TRUE(
      GetStaticDomainState(&state, "example.com", &sts_state, &pkp_state));
  EXPECT_TRUE(sts_state.include_subdomains);
  EXPECT_EQ(TransportSecurityState::STSState::MODE_FORCE_HTTPS,
            sts_state.upgrade_mode);
  EXPECT_FALSE(pkp_state.include_subdomains);
  EXPECT_EQ(GURL(), pkp_state.report_uri);
  EXPECT_EQ(0U, pkp_state.spki_hashes.size());
  EXPECT_EQ(0U, pkp_state.bad_spki_hashes.size());
  EXPECT_FALSE(GetExpectCTState(&state, "example.com", &ct_state));
  EXPECT_EQ(GURL(), ct_state.report_uri);
  EXPECT_TRUE(GetExpectStapleState(&state, "example.com", &staple_state));
  EXPECT_FALSE(staple_state.include_subdomains);
  EXPECT_EQ(GURL("https://report.example.com/staple-upload"),
            staple_state.report_uri);

  sts_state = TransportSecurityState::STSState();
  pkp_state = TransportSecurityState::PKPState();
  ct_state = TransportSecurityState::ExpectCTState();
  staple_state = TransportSecurityState::ExpectStapleState();
  EXPECT_TRUE(
      GetStaticDomainState(&state, "hpkp.example.com", &sts_state, &pkp_state));
  EXPECT_FALSE(sts_state.include_subdomains);
  EXPECT_EQ(TransportSecurityState::STSState::MODE_DEFAULT,
            sts_state.upgrade_mode);
  EXPECT_TRUE(pkp_state.include_subdomains);
  EXPECT_EQ(GURL("https://report.example.com/hpkp-upload"),
            pkp_state.report_uri);
  EXPECT_EQ(1U, pkp_state.spki_hashes.size());
  EXPECT_EQ(pkp_state.spki_hashes[0], GetSampleSPKIHash(0x1));
  EXPECT_EQ(0U, pkp_state.bad_spki_hashes.size());
  EXPECT_FALSE(GetExpectCTState(&state, "hpkp.example.com", &ct_state));
  EXPECT_EQ(GURL(), ct_state.report_uri);
  EXPECT_FALSE(GetExpectStapleState(&state, "hpkp.example.com", &staple_state));
  EXPECT_FALSE(staple_state.include_subdomains);
  EXPECT_EQ(GURL(), staple_state.report_uri);

  sts_state = TransportSecurityState::STSState();
  pkp_state = TransportSecurityState::PKPState();
  ct_state = TransportSecurityState::ExpectCTState();
  staple_state = TransportSecurityState::ExpectStapleState();
  EXPECT_TRUE(
      GetStaticDomainState(&state, "example.org", &sts_state, &pkp_state));
  EXPECT_FALSE(sts_state.include_subdomains);
  EXPECT_EQ(TransportSecurityState::STSState::MODE_FORCE_HTTPS,
            sts_state.upgrade_mode);
  EXPECT_FALSE(pkp_state.include_subdomains);
  EXPECT_EQ(GURL(), pkp_state.report_uri);
  EXPECT_EQ(0U, pkp_state.spki_hashes.size());
  EXPECT_EQ(0U, pkp_state.bad_spki_hashes.size());
  EXPECT_TRUE(GetExpectCTState(&state, "example.org", &ct_state));
  EXPECT_EQ(GURL("https://report.example.org/ct-upload"), ct_state.report_uri);
  EXPECT_FALSE(GetExpectStapleState(&state, "example.org", &staple_state));
  EXPECT_FALSE(staple_state.include_subdomains);
  EXPECT_EQ(GURL(), staple_state.report_uri);

  sts_state = TransportSecurityState::STSState();
  pkp_state = TransportSecurityState::PKPState();
  ct_state = TransportSecurityState::ExpectCTState();
  staple_state = TransportSecurityState::ExpectStapleState();
  EXPECT_TRUE(
      GetStaticDomainState(&state, "badssl.com", &sts_state, &pkp_state));
  EXPECT_TRUE(sts_state.include_subdomains);
  EXPECT_EQ(TransportSecurityState::STSState::MODE_DEFAULT,
            sts_state.upgrade_mode);
  EXPECT_TRUE(pkp_state.include_subdomains);
  EXPECT_EQ(GURL("https://report.example.com/hpkp-upload"),
            pkp_state.report_uri);
  EXPECT_EQ(1U, pkp_state.spki_hashes.size());
  EXPECT_EQ(pkp_state.spki_hashes[0], GetSampleSPKIHash(0x1));
  EXPECT_EQ(0U, pkp_state.bad_spki_hashes.size());
  EXPECT_FALSE(GetExpectCTState(&state, "badssl.com", &ct_state));
  EXPECT_EQ(GURL(), ct_state.report_uri);
  EXPECT_TRUE(GetExpectStapleState(&state, "badssl.com", &staple_state));
  EXPECT_TRUE(staple_state.include_subdomains);
  EXPECT_EQ(GURL("https://report.badssl.com/staple-upload"),
            staple_state.report_uri);

  sts_state = TransportSecurityState::STSState();
  pkp_state = TransportSecurityState::PKPState();
  ct_state = TransportSecurityState::ExpectCTState();
  staple_state = TransportSecurityState::ExpectStapleState();
  EXPECT_TRUE(
      GetStaticDomainState(&state, "mix.badssl.com", &sts_state, &pkp_state));
  EXPECT_FALSE(sts_state.include_subdomains);
  EXPECT_EQ(TransportSecurityState::STSState::MODE_FORCE_HTTPS,
            sts_state.upgrade_mode);
  EXPECT_TRUE(pkp_state.include_subdomains);
  EXPECT_EQ(GURL(), pkp_state.report_uri);
  EXPECT_EQ(1U, pkp_state.spki_hashes.size());
  EXPECT_EQ(pkp_state.spki_hashes[0], GetSampleSPKIHash(0x2));
  EXPECT_EQ(1U, pkp_state.bad_spki_hashes.size());
  EXPECT_EQ(pkp_state.bad_spki_hashes[0], GetSampleSPKIHash(0x1));
  EXPECT_TRUE(GetExpectCTState(&state, "mix.badssl.com", &ct_state));
  EXPECT_EQ(GURL("https://report.example.com/ct-upload"), ct_state.report_uri);
  EXPECT_TRUE(GetExpectStapleState(&state, "mix.badssl.com", &staple_state));
  EXPECT_TRUE(staple_state.include_subdomains);
  EXPECT_EQ(GURL("https://report.badssl.com/staple-upload"),
            staple_state.report_uri);

  sts_state = TransportSecurityState::STSState();
  pkp_state = TransportSecurityState::PKPState();
  ct_state = TransportSecurityState::ExpectCTState();
  staple_state = TransportSecurityState::ExpectStapleState();

  // This should be a simple entry in the context of
  // TrieWriter::IsSimpleEntry().
  EXPECT_TRUE(GetStaticDomainState(&state, "simple-entry.example.com",
                                   &sts_state, &pkp_state));
  EXPECT_TRUE(sts_state.include_subdomains);
  EXPECT_EQ(TransportSecurityState::STSState::MODE_FORCE_HTTPS,
            sts_state.upgrade_mode);
  EXPECT_FALSE(pkp_state.include_subdomains);
  EXPECT_FALSE(GetExpectCTState(&state, "simple-entry.example.com", &ct_state));
  EXPECT_FALSE(
      GetExpectStapleState(&state, "simple-entry.example.com", &staple_state));
}

static const struct ExpectStapleErrorResponseData {
  OCSPVerifyResult::ResponseStatus response_status;
  std::string response_status_string;
} kExpectStapleReportData[] = {
    {OCSPVerifyResult::MISSING, "MISSING"},
    {OCSPVerifyResult::ERROR_RESPONSE, "ERROR_RESPONSE"},
    {OCSPVerifyResult::BAD_PRODUCED_AT, "BAD_PRODUCED_AT"},
    {OCSPVerifyResult::NO_MATCHING_RESPONSE, "NO_MATCHING_RESPONSE"},
    {OCSPVerifyResult::INVALID_DATE, "INVALID_DATE"},
    {OCSPVerifyResult::PARSE_RESPONSE_ERROR, "PARSE_RESPONSE_ERROR"},
    {OCSPVerifyResult::PARSE_RESPONSE_DATA_ERROR, "PARSE_RESPONSE_DATA_ERROR"},
};

class ExpectStapleErrorResponseTest
    : public TransportSecurityStateTest,
      public testing::WithParamInterface<ExpectStapleErrorResponseData> {};

// For every |response_status| indicating an OCSP response was provided, but had
// some sort of parsing/validation error, test that the ExpectStaple report is
// serialized correctly.
TEST_P(ExpectStapleErrorResponseTest, CheckResponseStatusSerialization) {
  TransportSecurityState state;
  TransportSecurityStateTest::SetEnableStaticExpectStaple(&state, true);
  MockCertificateReportSender reporter;
  ExpectStapleErrorResponseData test = GetParam();

  std::string ocsp_response;
  if (test.response_status != OCSPVerifyResult::MISSING)
    ocsp_response = "dummy_response";

  // Two dummy certs to use as the server-sent and validated chains. The
  // contents don't matter.
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);

  SSLInfo ssl_info;
  ssl_info.cert = cert1;
  ssl_info.unverified_cert = cert2;
  ssl_info.ocsp_result.response_status = test.response_status;

  // Reports should only be sent when |is_issued_by_known_root| is true.
  ssl_info.is_issued_by_known_root = true;
  ASSERT_NO_FATAL_FAILURE(
      CheckExpectStapleReport(&state, &reporter, ssl_info, ocsp_response,
                              test.response_status_string, std::string()));
  reporter.Clear();

  // No report should be sent.
  ssl_info.is_issued_by_known_root = false;
  ASSERT_NO_FATAL_FAILURE(
      CheckExpectStapleReport(&state, &reporter, ssl_info, ocsp_response,
                              test.response_status_string, std::string()));
}

INSTANTIATE_TEST_CASE_P(ExpectStaple,
                        ExpectStapleErrorResponseTest,
                        testing::ValuesIn(kExpectStapleReportData));

static const struct ExpectStapleErrorCertStatusData {
  OCSPRevocationStatus revocation_status;
  std::string cert_status_string;
} kExpectStapleErrorCertStatusData[] = {
    {OCSPRevocationStatus::REVOKED, "REVOKED"},
    {OCSPRevocationStatus::UNKNOWN, "UNKNOWN"},
};

class ExpectStapleErrorCertStatusTest
    : public TransportSecurityStateTest,
      public testing::WithParamInterface<ExpectStapleErrorCertStatusData> {};

// Test that |revocation_status| is serialized into the |cert-status| field of
// the Expect-Staple report whenever |response_status| is PROVIDED and
// |revocation_status| != GOOD.
TEST_P(ExpectStapleErrorCertStatusTest, CheckCertStatusSerialization) {
  TransportSecurityState state;
  TransportSecurityStateTest::SetEnableStaticExpectStaple(&state, true);
  MockCertificateReportSender reporter;
  ExpectStapleErrorCertStatusData test = GetParam();
  std::string ocsp_response = "dummy_response";

  // Two dummy certs to use as the server-sent and validated chains. The
  // contents don't matter.
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);

  SSLInfo ssl_info;
  ssl_info.cert = cert1;
  ssl_info.unverified_cert = cert2;
  // |response_status| must be set to PROVIDED for |revocation_status| to have
  // meaning.
  ssl_info.ocsp_result.response_status = OCSPVerifyResult::PROVIDED;
  ssl_info.ocsp_result.revocation_status = test.revocation_status;

  // Reports should only be sent when |is_issued_by_known_root| is true.
  ssl_info.is_issued_by_known_root = true;
  ASSERT_NO_FATAL_FAILURE(CheckExpectStapleReport(&state, &reporter, ssl_info,
                                                  ocsp_response, "PROVIDED",
                                                  test.cert_status_string));
  reporter.Clear();

  ssl_info.is_issued_by_known_root = false;
  ASSERT_NO_FATAL_FAILURE(CheckExpectStapleReport(&state, &reporter, ssl_info,
                                                  ocsp_response, "PROVIDED",
                                                  test.cert_status_string));
};

INSTANTIATE_TEST_CASE_P(ExpectStaple,
                        ExpectStapleErrorCertStatusTest,
                        testing::ValuesIn(kExpectStapleErrorCertStatusData));

TEST_F(TransportSecurityStateTest, ExpectStapleDoesNotReportValidStaple) {
  TransportSecurityState state;
  TransportSecurityStateTest::SetEnableStaticExpectStaple(&state, true);
  MockCertificateReportSender reporter;
  state.SetReportSender(&reporter);

  // Baked-in preloaded Expect-Staple test hosts.
  HostPortPair host_port(kExpectStapleStaticHostname, 443);

  // Two dummy certs to use as the server-sent and validated chains. The
  // contents don't matter.
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);

  SSLInfo ssl_info;
  ssl_info.cert = cert1;
  ssl_info.unverified_cert = cert2;
  ssl_info.ocsp_result.response_status = OCSPVerifyResult::PROVIDED;
  ssl_info.ocsp_result.revocation_status = OCSPRevocationStatus::GOOD;

  std::string ocsp_response = "dummy response";

  ssl_info.is_issued_by_known_root = true;
  state.CheckExpectStaple(host_port, ssl_info, ocsp_response);
  EXPECT_EQ(GURL(), reporter.latest_report_uri());
  EXPECT_TRUE(reporter.latest_report().empty());

  ssl_info.is_issued_by_known_root = false;
  state.CheckExpectStaple(host_port, ssl_info, ocsp_response);
  EXPECT_EQ(GURL(), reporter.latest_report_uri());
  EXPECT_TRUE(reporter.latest_report().empty());
}

TEST_F(TransportSecurityStateTest, ExpectStapleRequiresPreload) {
  TransportSecurityState state;
  TransportSecurityStateTest::SetEnableStaticExpectStaple(&state, true);
  MockCertificateReportSender reporter;
  state.SetReportSender(&reporter);

  HostPortPair host_port("not-preloaded.host.example", 443);

  // Two dummy certs to use as the server-sent and validated chains. The
  // contents don't matter.
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);

  SSLInfo ssl_info;
  ssl_info.cert = cert1;
  ssl_info.unverified_cert = cert2;
  ssl_info.ocsp_result.response_status = OCSPVerifyResult::MISSING;

  // Empty response
  std::string ocsp_response;

  ssl_info.is_issued_by_known_root = true;
  state.CheckExpectStaple(host_port, ssl_info, ocsp_response);
  EXPECT_EQ(GURL(), reporter.latest_report_uri());
  EXPECT_TRUE(reporter.latest_report().empty());

  ssl_info.is_issued_by_known_root = false;
  state.CheckExpectStaple(host_port, ssl_info, ocsp_response);
  EXPECT_EQ(GURL(), reporter.latest_report_uri());
  EXPECT_TRUE(reporter.latest_report().empty());
}

// Tests that TransportSecurityState always consults the RequireCTDelegate,
// if supplied.
TEST_F(TransportSecurityStateTest, RequireCTConsultsDelegate) {
  using ::testing::_;
  using ::testing::Return;
  using CTRequirementLevel =
      TransportSecurityState::RequireCTDelegate::CTRequirementLevel;

  // Dummy cert to use as the validate chain. The contents do not matter.
  scoped_refptr<X509Certificate> cert =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert);

  HashValueVector hashes;
  hashes.push_back(HashValue(
      X509Certificate::CalculateFingerprint256(cert->os_cert_handle())));

  {
    TransportSecurityState state;
    const TransportSecurityState::CTRequirementsStatus original_status =
        state.CheckCTRequirements(
            HostPortPair("www.example.com", 443), true, hashes, cert.get(),
            cert.get(), SignedCertificateTimestampAndStatusList(),
            TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
            ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS);

    MockRequireCTDelegate always_require_delegate;
    EXPECT_CALL(always_require_delegate, IsCTRequiredForHost(_))
        .WillRepeatedly(Return(CTRequirementLevel::REQUIRED));
    state.SetRequireCTDelegate(&always_require_delegate);
    EXPECT_EQ(
        TransportSecurityState::CT_REQUIREMENTS_NOT_MET,
        state.CheckCTRequirements(
            HostPortPair("www.example.com", 443), true, hashes, cert.get(),
            cert.get(), SignedCertificateTimestampAndStatusList(),
            TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
            ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
    EXPECT_EQ(
        TransportSecurityState::CT_REQUIREMENTS_NOT_MET,
        state.CheckCTRequirements(
            HostPortPair("www.example.com", 443), true, hashes, cert.get(),
            cert.get(), SignedCertificateTimestampAndStatusList(),
            TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
            ct::CertPolicyCompliance::CERT_POLICY_NOT_DIVERSE_SCTS));
    EXPECT_EQ(
        TransportSecurityState::CT_REQUIREMENTS_MET,
        state.CheckCTRequirements(
            HostPortPair("www.example.com", 443), true, hashes, cert.get(),
            cert.get(), SignedCertificateTimestampAndStatusList(),
            TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
            ct::CertPolicyCompliance::CERT_POLICY_COMPLIES_VIA_SCTS));
    EXPECT_EQ(
        TransportSecurityState::CT_REQUIREMENTS_MET,
        state.CheckCTRequirements(
            HostPortPair("www.example.com", 443), true, hashes, cert.get(),
            cert.get(), SignedCertificateTimestampAndStatusList(),
            TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
            ct::CertPolicyCompliance::CERT_POLICY_BUILD_NOT_TIMELY));

    state.SetRequireCTDelegate(nullptr);
    EXPECT_EQ(
        original_status,
        state.CheckCTRequirements(
            HostPortPair("www.example.com", 443), true, hashes, cert.get(),
            cert.get(), SignedCertificateTimestampAndStatusList(),
            TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
            ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
  }

  {
    TransportSecurityState state;
    const TransportSecurityState::CTRequirementsStatus original_status =
        state.CheckCTRequirements(
            HostPortPair("www.example.com", 443), true, hashes, cert.get(),
            cert.get(), SignedCertificateTimestampAndStatusList(),
            TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
            ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS);

    MockRequireCTDelegate never_require_delegate;
    EXPECT_CALL(never_require_delegate, IsCTRequiredForHost(_))
        .WillRepeatedly(Return(CTRequirementLevel::NOT_REQUIRED));
    state.SetRequireCTDelegate(&never_require_delegate);
    EXPECT_EQ(
        TransportSecurityState::CT_NOT_REQUIRED,
        state.CheckCTRequirements(
            HostPortPair("www.example.com", 443), true, hashes, cert.get(),
            cert.get(), SignedCertificateTimestampAndStatusList(),
            TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
            ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
    EXPECT_EQ(
        TransportSecurityState::CT_NOT_REQUIRED,
        state.CheckCTRequirements(
            HostPortPair("www.example.com", 443), true, hashes, cert.get(),
            cert.get(), SignedCertificateTimestampAndStatusList(),
            TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
            ct::CertPolicyCompliance::CERT_POLICY_NOT_DIVERSE_SCTS));

    state.SetRequireCTDelegate(nullptr);
    EXPECT_EQ(
        original_status,
        state.CheckCTRequirements(
            HostPortPair("www.example.com", 443), true, hashes, cert.get(),
            cert.get(), SignedCertificateTimestampAndStatusList(),
            TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
            ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
  }

  {
    TransportSecurityState state;
    const TransportSecurityState::CTRequirementsStatus original_status =
        state.CheckCTRequirements(
            HostPortPair("www.example.com", 443), true, hashes, cert.get(),
            cert.get(), SignedCertificateTimestampAndStatusList(),
            TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
            ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS);

    MockRequireCTDelegate default_require_ct_delegate;
    EXPECT_CALL(default_require_ct_delegate, IsCTRequiredForHost(_))
        .WillRepeatedly(Return(CTRequirementLevel::DEFAULT));
    state.SetRequireCTDelegate(&default_require_ct_delegate);
    EXPECT_EQ(
        original_status,
        state.CheckCTRequirements(
            HostPortPair("www.example.com", 443), true, hashes, cert.get(),
            cert.get(), SignedCertificateTimestampAndStatusList(),
            TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
            ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));

    state.SetRequireCTDelegate(nullptr);
    EXPECT_EQ(
        original_status,
        state.CheckCTRequirements(
            HostPortPair("www.example.com", 443), true, hashes, cert.get(),
            cert.get(), SignedCertificateTimestampAndStatusList(),
            TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
            ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
  }
}

// Tests that Certificate Transparency is required for Symantec-issued
// certificates, unless the certificate was issued prior to 1 June 2016
// or the issuing CA is whitelisted as independently operated.
TEST_F(TransportSecurityStateTest, RequireCTForSymantec) {
  // Test certificates before and after the 1 June 2016 deadline.
  scoped_refptr<X509Certificate> before_cert =
      ImportCertFromFile(GetTestCertsDirectory(), "pre_june_2016.pem");
  ASSERT_TRUE(before_cert);
  scoped_refptr<X509Certificate> after_cert =
      ImportCertFromFile(GetTestCertsDirectory(), "post_june_2016.pem");
  ASSERT_TRUE(after_cert);

  SHA256HashValue symantec_hash_value = {
      {0xb2, 0xde, 0xf5, 0x36, 0x2a, 0xd3, 0xfa, 0xcd, 0x04, 0xbd, 0x29,
       0x04, 0x7a, 0x43, 0x84, 0x4f, 0x76, 0x70, 0x34, 0xea, 0x48, 0x92,
       0xf8, 0x0e, 0x56, 0xbe, 0xe6, 0x90, 0x24, 0x3e, 0x25, 0x02}};
  SHA256HashValue google_hash_value = {
      {0xec, 0x72, 0x29, 0x69, 0xcb, 0x64, 0x20, 0x0a, 0xb6, 0x63, 0x8f,
       0x68, 0xac, 0x53, 0x8e, 0x40, 0xab, 0xab, 0x5b, 0x19, 0xa6, 0x48,
       0x56, 0x61, 0x04, 0x2a, 0x10, 0x61, 0xc4, 0x61, 0x27, 0x76}};

  TransportSecurityState state;

  HashValueVector hashes;
  hashes.push_back(HashValue(symantec_hash_value));

  // Certificates issued by Symantec prior to 1 June 2016 should not
  // be required to be disclosed via CT.
  EXPECT_EQ(
      TransportSecurityState::CT_NOT_REQUIRED,
      state.CheckCTRequirements(
          HostPortPair("www.example.com", 443), true, hashes, before_cert.get(),
          before_cert.get(), SignedCertificateTimestampAndStatusList(),
          TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
          ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));

  // ... but certificates issued after 1 June 2016 are required to be...
  EXPECT_EQ(
      TransportSecurityState::CT_REQUIREMENTS_NOT_MET,
      state.CheckCTRequirements(
          HostPortPair("www.example.com", 443), true, hashes, after_cert.get(),
          after_cert.get(), SignedCertificateTimestampAndStatusList(),
          TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
          ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
  EXPECT_EQ(
      TransportSecurityState::CT_REQUIREMENTS_NOT_MET,
      state.CheckCTRequirements(
          HostPortPair("www.example.com", 443), true, hashes, after_cert.get(),
          after_cert.get(), SignedCertificateTimestampAndStatusList(),
          TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
          ct::CertPolicyCompliance::CERT_POLICY_NOT_DIVERSE_SCTS));
  EXPECT_EQ(
      TransportSecurityState::CT_REQUIREMENTS_MET,
      state.CheckCTRequirements(
          HostPortPair("www.example.com", 443), true, hashes, after_cert.get(),
          after_cert.get(), SignedCertificateTimestampAndStatusList(),
          TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
          ct::CertPolicyCompliance::CERT_POLICY_BUILD_NOT_TIMELY));
  EXPECT_EQ(
      TransportSecurityState::CT_REQUIREMENTS_MET,
      state.CheckCTRequirements(
          HostPortPair("www.example.com", 443), true, hashes, after_cert.get(),
          after_cert.get(), SignedCertificateTimestampAndStatusList(),
          TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
          ct::CertPolicyCompliance::CERT_POLICY_COMPLIES_VIA_SCTS));

  // ... unless they were issued by an excluded intermediate.
  hashes.push_back(HashValue(google_hash_value));
  EXPECT_EQ(
      TransportSecurityState::CT_NOT_REQUIRED,
      state.CheckCTRequirements(
          HostPortPair("www.example.com", 443), true, hashes, before_cert.get(),
          before_cert.get(), SignedCertificateTimestampAndStatusList(),
          TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
          ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
  EXPECT_EQ(
      TransportSecurityState::CT_NOT_REQUIRED,
      state.CheckCTRequirements(
          HostPortPair("www.example.com", 443), true, hashes, after_cert.get(),
          after_cert.get(), SignedCertificateTimestampAndStatusList(),
          TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
          ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));

  // And other certificates should remain unaffected.
  SHA256HashValue unrelated_hash_value = {{0x01, 0x02}};
  HashValueVector unrelated_hashes;
  unrelated_hashes.push_back(HashValue(unrelated_hash_value));

  EXPECT_EQ(TransportSecurityState::CT_NOT_REQUIRED,
            state.CheckCTRequirements(
                HostPortPair("www.example.com", 443), true, unrelated_hashes,
                before_cert.get(), before_cert.get(),
                SignedCertificateTimestampAndStatusList(),
                TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
  EXPECT_EQ(TransportSecurityState::CT_NOT_REQUIRED,
            state.CheckCTRequirements(
                HostPortPair("www.example.com", 443), true, unrelated_hashes,
                after_cert.get(), after_cert.get(),
                SignedCertificateTimestampAndStatusList(),
                TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));

  // And the emergency field trial should disable the requirement, if
  // necessary.
  hashes.clear();
  hashes.push_back(HashValue(symantec_hash_value));
  base::FieldTrialList field_trial_list(
      std::make_unique<base::MockEntropyProvider>());
  base::FieldTrialList::CreateFieldTrial("EnforceCTForProblematicRoots",
                                         "disabled");

  EXPECT_EQ(
      TransportSecurityState::CT_NOT_REQUIRED,
      state.CheckCTRequirements(
          HostPortPair("www.example.com", 443), true, hashes, before_cert.get(),
          before_cert.get(), SignedCertificateTimestampAndStatusList(),
          TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
          ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
  EXPECT_EQ(
      TransportSecurityState::CT_NOT_REQUIRED,
      state.CheckCTRequirements(
          HostPortPair("www.example.com", 443), true, hashes, after_cert.get(),
          after_cert.get(), SignedCertificateTimestampAndStatusList(),
          TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
          ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
}

// Tests that dynamic Expect-CT state is cleared from ClearDynamicData().
TEST_F(TransportSecurityStateTest, DynamicExpectCTStateCleared) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  const std::string host("example.test");
  TransportSecurityState state;
  TransportSecurityState::ExpectCTState expect_ct_state;
  const base::Time current_time = base::Time::Now();
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);

  state.AddExpectCT(host, expiry, true, GURL());
  EXPECT_TRUE(state.GetDynamicExpectCTState(host, &expect_ct_state));
  EXPECT_TRUE(expect_ct_state.enforce);
  EXPECT_TRUE(expect_ct_state.report_uri.is_empty());
  EXPECT_EQ(expiry, expect_ct_state.expiry);

  state.ClearDynamicData();
  EXPECT_FALSE(state.GetDynamicExpectCTState(host, &expect_ct_state));
}

// Tests that dynamic Expect-CT state can be added and retrieved.
TEST_F(TransportSecurityStateTest, DynamicExpectCTState) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  const std::string host("example.test");
  TransportSecurityState state;
  TransportSecurityState::ExpectCTState expect_ct_state;
  const base::Time current_time = base::Time::Now();
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);

  // Test that Expect-CT state can be added and retrieved.
  state.AddExpectCT(host, expiry, true, GURL());
  EXPECT_TRUE(state.GetDynamicExpectCTState(host, &expect_ct_state));
  EXPECT_TRUE(expect_ct_state.enforce);
  EXPECT_TRUE(expect_ct_state.report_uri.is_empty());
  EXPECT_EQ(expiry, expect_ct_state.expiry);

  // Test that Expect-CT can be updated (e.g. by changing |enforce| to false and
  // adding a report-uri).
  const GURL report_uri("https://example-report.test");
  state.AddExpectCT(host, expiry, false, report_uri);
  EXPECT_TRUE(state.GetDynamicExpectCTState(host, &expect_ct_state));
  EXPECT_FALSE(expect_ct_state.enforce);
  EXPECT_EQ(report_uri, expect_ct_state.report_uri);
  EXPECT_EQ(expiry, expect_ct_state.expiry);

  // Test that Expect-CT state is discarded when expired.
  state.AddExpectCT(host, current_time - base::TimeDelta::FromSeconds(1000),
                    true, report_uri);
  EXPECT_FALSE(state.GetDynamicExpectCTState(host, &expect_ct_state));
}

// Tests that the Expect-CT reporter is not notified for repeated dynamic
// Expect-CT violations for the same host/port.
TEST_F(TransportSecurityStateTest, DynamicExpectCTDeduping) {
  const char kHeader[] = "max-age=123,enforce,report-uri=\"http://foo.test\"";
  SSLInfo ssl;
  ssl.is_issued_by_known_root = true;
  ssl.ct_compliance_details_available = true;
  ssl.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_COMPLIES_VIA_SCTS;

  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);

  SignedCertificateTimestampAndStatusList sct_list;

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  base::Time now = base::Time::Now();
  TransportSecurityState state;
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.ProcessExpectCTHeader(kHeader, HostPortPair("example.test", 443), ssl);
  TransportSecurityState::ExpectCTState expect_ct_state;
  EXPECT_TRUE(state.GetDynamicExpectCTState("example.test", &expect_ct_state));
  EXPECT_EQ(GURL("http://foo.test"), expect_ct_state.report_uri);
  EXPECT_TRUE(expect_ct_state.enforce);
  EXPECT_LT(now, expect_ct_state.expiry);
  // No report should be sent when the header was processed over a connection
  // that complied with CT policy.
  EXPECT_EQ(0u, reporter.num_failures());

  // The first time the host fails to meet CT requirements, a report should be
  // sent.
  EXPECT_EQ(TransportSecurityState::CT_REQUIREMENTS_NOT_MET,
            state.CheckCTRequirements(
                HostPortPair("example.test", 443), true, HashValueVector(),
                cert1.get(), cert2.get(), sct_list,
                TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
  EXPECT_EQ(1u, reporter.num_failures());

  // The second time it fails to meet CT requirements, a report should not be
  // sent.
  EXPECT_EQ(TransportSecurityState::CT_REQUIREMENTS_NOT_MET,
            state.CheckCTRequirements(
                HostPortPair("example.test", 443), true, HashValueVector(),
                cert1.get(), cert2.get(), sct_list,
                TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
  EXPECT_EQ(1u, reporter.num_failures());
}

// Tests that the Expect-CT reporter is not notified for CT-compliant
// connections.
TEST_F(TransportSecurityStateTest, DynamicExpectCTCompliantConnection) {
  const char kHeader[] = "max-age=123,report-uri=\"http://foo.test\"";
  SSLInfo ssl;
  ssl.is_issued_by_known_root = true;
  ssl.ct_compliance_details_available = true;
  ssl.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_COMPLIES_VIA_SCTS;

  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);

  SignedCertificateTimestampAndStatusList sct_list;

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);

  TransportSecurityState state;
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.ProcessExpectCTHeader(kHeader, HostPortPair("example.test", 443), ssl);

  // No report should be sent when the header was processed over a connection
  // that complied with CT policy.
  EXPECT_EQ(TransportSecurityState::CT_NOT_REQUIRED,
            state.CheckCTRequirements(
                HostPortPair("example.test", 443), true, HashValueVector(),
                cert1.get(), cert2.get(), sct_list,
                TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_COMPLIES_VIA_SCTS));
  EXPECT_EQ(0u, reporter.num_failures());
}

// Tests that the Expect-CT reporter is not notified when the Expect-CT header
// is received repeatedly over non-compliant connections.
TEST_F(TransportSecurityStateTest, DynamicExpectCTHeaderProcessingDeduping) {
  const char kHeader[] = "max-age=123,enforce,report-uri=\"http://foo.test\"";
  SSLInfo ssl;
  ssl.is_issued_by_known_root = true;
  ssl.ct_compliance_details_available = true;
  ssl.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS;

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  TransportSecurityState state;
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.ProcessExpectCTHeader(kHeader, HostPortPair("example.test", 443), ssl);
  TransportSecurityState::ExpectCTState expect_ct_state;
  EXPECT_FALSE(state.GetDynamicExpectCTState("example.test", &expect_ct_state));
  // The first time the header was received over a connection that failed to
  // meet CT requirements, a report should be sent.
  EXPECT_EQ(1u, reporter.num_failures());

  // The second time the header was received, no report should be sent.
  state.ProcessExpectCTHeader(kHeader, HostPortPair("example.test", 443), ssl);
  EXPECT_EQ(1u, reporter.num_failures());
}

// Tests that dynamic Expect-CT state cannot be added when the feature is not
// enabled.
TEST_F(TransportSecurityStateTest, DynamicExpectCTStateDisabled) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndDisableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  const std::string host("example.test");
  TransportSecurityState state;
  TransportSecurityState::ExpectCTState expect_ct_state;
  const base::Time current_time = base::Time::Now();
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);

  state.AddExpectCT(host, expiry, true, GURL());
  EXPECT_FALSE(state.GetDynamicExpectCTState(host, &expect_ct_state));
}

// Tests that dynamic Expect-CT opt-ins are processed correctly (when the
// feature is enabled).
TEST_F(TransportSecurityStateTest, DynamicExpectCT) {
  const char kHeader[] = "max-age=123,enforce,report-uri=\"http://foo.test\"";
  SSLInfo ssl;
  ssl.is_issued_by_known_root = true;
  ssl.ct_compliance_details_available = true;
  ssl.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_COMPLIES_VIA_SCTS;

  // First test that the header is not processed when the feature is disabled.
  {
    base::test::ScopedFeatureList feature_list;
    feature_list.InitAndDisableFeature(
        TransportSecurityState::kDynamicExpectCTFeature);
    TransportSecurityState state;
    state.ProcessExpectCTHeader(kHeader, HostPortPair("example.test", 443),
                                ssl);
    TransportSecurityState::ExpectCTState expect_ct_state;
    EXPECT_FALSE(
        state.GetDynamicExpectCTState("example.test", &expect_ct_state));
  }

  // Now test that the header is processed when the feature is enabled.
  {
    base::test::ScopedFeatureList feature_list;
    feature_list.InitAndEnableFeature(
        TransportSecurityState::kDynamicExpectCTFeature);
    base::Time now = base::Time::Now();
    TransportSecurityState state;
    MockExpectCTReporter reporter;
    state.SetExpectCTReporter(&reporter);
    state.ProcessExpectCTHeader(kHeader, HostPortPair("example.test", 443),
                                ssl);
    TransportSecurityState::ExpectCTState expect_ct_state;
    EXPECT_TRUE(
        state.GetDynamicExpectCTState("example.test", &expect_ct_state));
    EXPECT_EQ(GURL("http://foo.test"), expect_ct_state.report_uri);
    EXPECT_TRUE(expect_ct_state.enforce);
    EXPECT_LT(now, expect_ct_state.expiry);
    // No report should be sent when the header was processed over a connection
    // that complied with CT policy.
    EXPECT_EQ(0u, reporter.num_failures());
  }
}

// Tests that dynamic Expect-CT is not processed for private roots.
TEST_F(TransportSecurityStateTest, DynamicExpectCTPrivateRoot) {
  const char kHeader[] = "max-age=123,enforce,report-uri=\"http://foo.test\"";
  SSLInfo ssl;
  ssl.is_issued_by_known_root = false;
  ssl.ct_compliance_details_available = true;
  ssl.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS;

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  TransportSecurityState state;
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.ProcessExpectCTHeader(kHeader, HostPortPair("example.test", 443), ssl);
  TransportSecurityState::ExpectCTState expect_ct_state;
  EXPECT_FALSE(state.GetDynamicExpectCTState("example.test", &expect_ct_state));
  EXPECT_EQ(0u, reporter.num_failures());
}

// Tests that dynamic Expect-CT is not processed when CT compliance status
// wasn't computed.
TEST_F(TransportSecurityStateTest, DynamicExpectCTNoComplianceDetails) {
  const char kHeader[] = "max-age=123,enforce,report-uri=\"http://foo.test\"";
  SSLInfo ssl;
  ssl.is_issued_by_known_root = true;
  ssl.ct_compliance_details_available = false;
  ssl.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS;

  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);
  ssl.unverified_cert = cert1;
  ssl.cert = cert2;

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  TransportSecurityState state;
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.ProcessExpectCTHeader(kHeader, HostPortPair("example.test", 443), ssl);
  TransportSecurityState::ExpectCTState expect_ct_state;
  EXPECT_FALSE(state.GetDynamicExpectCTState("example.test", &expect_ct_state));
  EXPECT_EQ(0u, reporter.num_failures());
}

// Tests that Expect-CT reports are sent when an Expect-CT header is received
// over a non-compliant connection.
TEST_F(TransportSecurityStateTest,
       DynamicExpectCTHeaderProcessingNonCompliant) {
  const char kHeader[] = "max-age=123,enforce,report-uri=\"http://foo.test\"";
  SSLInfo ssl;
  ssl.is_issued_by_known_root = true;
  ssl.ct_compliance_details_available = true;
  ssl.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS;

  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);
  ssl.unverified_cert = cert1;
  ssl.cert = cert2;

  MakeTestSCTAndStatus(ct::SignedCertificateTimestamp::SCT_EMBEDDED, "test_log",
                       std::string(), std::string(), base::Time::Now(),
                       ct::SCT_STATUS_INVALID_SIGNATURE,
                       &ssl.signed_certificate_timestamps);

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);
  TransportSecurityState state;
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.ProcessExpectCTHeader(kHeader, HostPortPair("example.test", 443), ssl);
  TransportSecurityState::ExpectCTState expect_ct_state;
  EXPECT_FALSE(state.GetDynamicExpectCTState("example.test", &expect_ct_state));
  EXPECT_EQ(1u, reporter.num_failures());
  EXPECT_EQ("example.test", reporter.host_port_pair().host());
  EXPECT_TRUE(reporter.expiration().is_null());
  EXPECT_EQ(cert1.get(), reporter.served_certificate_chain());
  EXPECT_EQ(cert2.get(), reporter.validated_certificate_chain());
  EXPECT_EQ(ssl.signed_certificate_timestamps.size(),
            reporter.signed_certificate_timestamps().size());
  EXPECT_EQ(ssl.signed_certificate_timestamps[0].status,
            reporter.signed_certificate_timestamps()[0].status);
  EXPECT_EQ(ssl.signed_certificate_timestamps[0].sct,
            reporter.signed_certificate_timestamps()[0].sct);
}

// Tests that CheckCTRequirements() returns the correct response if a connection
// to a host violates an Expect-CT header, and that it reports violations.
TEST_F(TransportSecurityStateTest, CheckCTRequirementsWithExpectCT) {
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);
  SignedCertificateTimestampAndStatusList sct_list;
  MakeTestSCTAndStatus(ct::SignedCertificateTimestamp::SCT_EMBEDDED, "test_log",
                       std::string(), std::string(), base::Time::Now(),
                       ct::SCT_STATUS_INVALID_SIGNATURE, &sct_list);

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);

  TransportSecurityState state;
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.AddExpectCT("example.test", expiry, true /* enforce */,
                    GURL("https://example-report.test"));
  state.AddExpectCT("example-report-only.test", expiry, false /* enforce */,
                    GURL("https://example-report.test"));
  state.AddExpectCT("example-enforce-only.test", expiry, true /* enforce */,
                    GURL());

  // Test that a connection to an unrelated host is not affected.
  EXPECT_EQ(TransportSecurityState::CT_NOT_REQUIRED,
            state.CheckCTRequirements(
                HostPortPair("example2.test", 443), true, HashValueVector(),
                cert1.get(), cert2.get(), sct_list,
                TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
  EXPECT_EQ(TransportSecurityState::CT_NOT_REQUIRED,
            state.CheckCTRequirements(
                HostPortPair("example2.test", 443), true, HashValueVector(),
                cert1.get(), cert2.get(), sct_list,
                TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_NOT_DIVERSE_SCTS));
  EXPECT_EQ(0u, reporter.num_failures());

  // A connection to an Expect-CT host should be closed and reported.
  EXPECT_EQ(TransportSecurityState::CT_REQUIREMENTS_NOT_MET,
            state.CheckCTRequirements(
                HostPortPair("example.test", 443), true, HashValueVector(),
                cert1.get(), cert2.get(), sct_list,
                TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
  EXPECT_EQ(1u, reporter.num_failures());
  EXPECT_EQ("example.test", reporter.host_port_pair().host());
  EXPECT_EQ(443, reporter.host_port_pair().port());
  EXPECT_EQ(expiry, reporter.expiration());
  EXPECT_EQ(cert1.get(), reporter.validated_certificate_chain());
  EXPECT_EQ(cert2.get(), reporter.served_certificate_chain());
  EXPECT_EQ(sct_list.size(), reporter.signed_certificate_timestamps().size());
  EXPECT_EQ(sct_list[0].status,
            reporter.signed_certificate_timestamps()[0].status);
  EXPECT_EQ(sct_list[0].sct, reporter.signed_certificate_timestamps()[0].sct);

  // A compliant connection to an Expect-CT host should not be closed or
  // reported.
  EXPECT_EQ(TransportSecurityState::CT_REQUIREMENTS_MET,
            state.CheckCTRequirements(
                HostPortPair("example.test", 443), true, HashValueVector(),
                cert1.get(), cert2.get(), sct_list,
                TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_COMPLIES_VIA_SCTS));
  EXPECT_EQ(1u, reporter.num_failures());
  EXPECT_EQ(TransportSecurityState::CT_REQUIREMENTS_MET,
            state.CheckCTRequirements(
                HostPortPair("example.test", 443), true, HashValueVector(),
                cert1.get(), cert2.get(), sct_list,
                TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_BUILD_NOT_TIMELY));
  EXPECT_EQ(1u, reporter.num_failures());

  // A connection to a report-only host should be reported only.
  EXPECT_EQ(TransportSecurityState::CT_NOT_REQUIRED,
            state.CheckCTRequirements(
                HostPortPair("example-report-only.test", 443), true,
                HashValueVector(), cert1.get(), cert2.get(), sct_list,
                TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_NOT_DIVERSE_SCTS));
  EXPECT_EQ(2u, reporter.num_failures());
  EXPECT_EQ("example-report-only.test", reporter.host_port_pair().host());
  EXPECT_EQ(443, reporter.host_port_pair().port());
  EXPECT_EQ(cert1.get(), reporter.validated_certificate_chain());
  EXPECT_EQ(cert2.get(), reporter.served_certificate_chain());
  EXPECT_EQ(sct_list.size(), reporter.signed_certificate_timestamps().size());
  EXPECT_EQ(sct_list[0].status,
            reporter.signed_certificate_timestamps()[0].status);
  EXPECT_EQ(sct_list[0].sct, reporter.signed_certificate_timestamps()[0].sct);

  // A connection to an enforce-only host should be closed but not reported.
  EXPECT_EQ(TransportSecurityState::CT_REQUIREMENTS_NOT_MET,
            state.CheckCTRequirements(
                HostPortPair("example-enforce-only.test", 443), true,
                HashValueVector(), cert1.get(), cert2.get(), sct_list,
                TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_NOT_DIVERSE_SCTS));
  EXPECT_EQ(2u, reporter.num_failures());

  // A connection with a private root should be neither enforced nor reported.
  EXPECT_EQ(TransportSecurityState::CT_NOT_REQUIRED,
            state.CheckCTRequirements(
                HostPortPair("example.test", 443), false, HashValueVector(),
                cert1.get(), cert2.get(), sct_list,
                TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
  EXPECT_EQ(2u, reporter.num_failures());

  // A connection with DISABLE_EXPECT_CT_REPORTS should not send a report.
  EXPECT_EQ(TransportSecurityState::CT_REQUIREMENTS_NOT_MET,
            state.CheckCTRequirements(
                HostPortPair("example.test", 443), true, HashValueVector(),
                cert1.get(), cert2.get(), sct_list,
                TransportSecurityState::DISABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
  EXPECT_EQ(2u, reporter.num_failures());
}

// Tests that for a host that requires CT by delegate and is also
// Expect-CT-enabled, CheckCTRequirements() sends reports.
TEST_F(TransportSecurityStateTest, CheckCTRequirementsWithExpectCTAndDelegate) {
  using ::testing::_;
  using ::testing::Return;
  using CTRequirementLevel =
      TransportSecurityState::RequireCTDelegate::CTRequirementLevel;

  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  scoped_refptr<X509Certificate> cert1 =
      ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
  ASSERT_TRUE(cert1);
  scoped_refptr<X509Certificate> cert2 =
      ImportCertFromFile(GetTestCertsDirectory(), "expired_cert.pem");
  ASSERT_TRUE(cert2);
  SignedCertificateTimestampAndStatusList sct_list;
  MakeTestSCTAndStatus(ct::SignedCertificateTimestamp::SCT_EMBEDDED, "test_log",
                       std::string(), std::string(), base::Time::Now(),
                       ct::SCT_STATUS_INVALID_SIGNATURE, &sct_list);

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);

  TransportSecurityState state;
  MockExpectCTReporter reporter;
  state.SetExpectCTReporter(&reporter);
  state.AddExpectCT("example.test", expiry, false /* enforce */,
                    GURL("https://example-report.test"));

  // A connection to an Expect-CT host, which also requires CT by the delegate,
  // should be closed and reported.
  MockRequireCTDelegate always_require_delegate;
  EXPECT_CALL(always_require_delegate, IsCTRequiredForHost(_))
      .WillRepeatedly(Return(CTRequirementLevel::REQUIRED));
  state.SetRequireCTDelegate(&always_require_delegate);
  EXPECT_EQ(TransportSecurityState::CT_REQUIREMENTS_NOT_MET,
            state.CheckCTRequirements(
                HostPortPair("example.test", 443), true, HashValueVector(),
                cert1.get(), cert2.get(), sct_list,
                TransportSecurityState::ENABLE_EXPECT_CT_REPORTS,
                ct::CertPolicyCompliance::CERT_POLICY_NOT_ENOUGH_SCTS));
  EXPECT_EQ(1u, reporter.num_failures());
  EXPECT_EQ("example.test", reporter.host_port_pair().host());
  EXPECT_EQ(443, reporter.host_port_pair().port());
  EXPECT_EQ(expiry, reporter.expiration());
  EXPECT_EQ(cert1.get(), reporter.validated_certificate_chain());
  EXPECT_EQ(cert2.get(), reporter.served_certificate_chain());
  EXPECT_EQ(sct_list.size(), reporter.signed_certificate_timestamps().size());
  EXPECT_EQ(sct_list[0].status,
            reporter.signed_certificate_timestamps()[0].status);
  EXPECT_EQ(sct_list[0].sct, reporter.signed_certificate_timestamps()[0].sct);
}

// Tests that the dynamic Expect-CT UMA histogram is recorded correctly.
TEST_F(TransportSecurityStateTest, DynamicExpectCTUMA) {
  const char kHistogramName[] = "Net.ExpectCTHeader.ParseSuccess";
  SSLInfo ssl;
  ssl.is_issued_by_known_root = true;
  ssl.ct_compliance_details_available = true;
  ssl.ct_cert_policy_compliance =
      ct::CertPolicyCompliance::CERT_POLICY_COMPLIES_VIA_SCTS;

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      TransportSecurityState::kDynamicExpectCTFeature);

  // Test that the histogram is recorded correctly when the header successfully
  // parses.
  {
    const char kHeader[] = "max-age=123,enforce,report-uri=\"http://foo.test\"";
    base::HistogramTester histograms;
    TransportSecurityState state;
    MockExpectCTReporter reporter;
    state.SetExpectCTReporter(&reporter);
    state.ProcessExpectCTHeader(kHeader, HostPortPair("example.test", 443),
                                ssl);
    histograms.ExpectTotalCount(kHistogramName, 1);
    histograms.ExpectBucketCount(kHistogramName, true, 1);
  }

  // Test that the histogram is recorded correctly when the header fails to
  // parse (due to semi-colons instead of commas).
  {
    const char kHeader[] = "max-age=123;enforce;report-uri=\"http://foo.test\"";
    base::HistogramTester histograms;
    TransportSecurityState state;
    MockExpectCTReporter reporter;
    state.SetExpectCTReporter(&reporter);
    state.ProcessExpectCTHeader(kHeader, HostPortPair("example.test", 443),
                                ssl);
    histograms.ExpectTotalCount(kHistogramName, 1);
    histograms.ExpectBucketCount(kHistogramName, false, 1);
  }
}

#if BUILDFLAG(INCLUDE_TRANSPORT_SECURITY_STATE_PRELOAD_LIST)
class TransportSecurityStateStaticTest : public TransportSecurityStateTest {
 public:
  TransportSecurityStateStaticTest() {
    SetTransportSecurityStateSourceForTesting(nullptr);
  }
};

static bool StaticShouldRedirect(const char* hostname) {
  TransportSecurityState state;
  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;
  return state.GetStaticDomainState(hostname, &sts_state, &pkp_state) &&
         sts_state.ShouldUpgradeToSSL();
}

static bool HasStaticState(const char* hostname) {
  TransportSecurityState state;
  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;
  return state.GetStaticDomainState(hostname, &sts_state, &pkp_state);
}

static bool HasStaticPublicKeyPins(const char* hostname) {
  TransportSecurityState state;
  TransportSecurityStateTest::EnableStaticPins(&state);
  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;
  if (!state.GetStaticDomainState(hostname, &sts_state, &pkp_state))
    return false;

  return pkp_state.HasPublicKeyPins();
}

static bool OnlyPinningInStaticState(const char* hostname) {
  TransportSecurityState state;
  TransportSecurityStateTest::EnableStaticPins(&state);
  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;
  if (!state.GetStaticDomainState(hostname, &sts_state, &pkp_state))
    return false;

  return (pkp_state.spki_hashes.size() > 0 ||
          pkp_state.bad_spki_hashes.size() > 0) &&
         !sts_state.ShouldUpgradeToSSL();
}

TEST_F(TransportSecurityStateStaticTest, EnableStaticPins) {
  TransportSecurityState state;
  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;

  EnableStaticPins(&state);

  EXPECT_TRUE(
      state.GetStaticDomainState("chrome.google.com", &sts_state, &pkp_state));
  EXPECT_FALSE(pkp_state.spki_hashes.empty());
}

TEST_F(TransportSecurityStateStaticTest, DisableStaticPins) {
  TransportSecurityState state;
  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;

  DisableStaticPins(&state);
  EXPECT_TRUE(
      state.GetStaticDomainState("chrome.google.com", &sts_state, &pkp_state));
  EXPECT_TRUE(pkp_state.spki_hashes.empty());
}

TEST_F(TransportSecurityStateStaticTest, IsPreloaded) {
  const std::string paypal = "paypal.com";
  const std::string www_paypal = "www.paypal.com";
  const std::string foo_paypal = "foo.paypal.com";
  const std::string a_www_paypal = "a.www.paypal.com";
  const std::string abc_paypal = "a.b.c.paypal.com";
  const std::string example = "example.com";
  const std::string aypal = "aypal.com";
  const std::string google = "google";
  const std::string www_google = "www.google";
  const std::string foo = "foo";

  TransportSecurityState state;
  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;

  EXPECT_TRUE(GetStaticDomainState(&state, paypal, &sts_state, &pkp_state));
  EXPECT_TRUE(GetStaticDomainState(&state, www_paypal, &sts_state, &pkp_state));
  EXPECT_FALSE(sts_state.include_subdomains);
  EXPECT_TRUE(GetStaticDomainState(&state, google, &sts_state, &pkp_state));
  EXPECT_TRUE(GetStaticDomainState(&state, www_google, &sts_state, &pkp_state));
  EXPECT_TRUE(GetStaticDomainState(&state, foo, &sts_state, &pkp_state));
  EXPECT_FALSE(
      GetStaticDomainState(&state, a_www_paypal, &sts_state, &pkp_state));
  EXPECT_FALSE(
      GetStaticDomainState(&state, abc_paypal, &sts_state, &pkp_state));
  EXPECT_FALSE(GetStaticDomainState(&state, example, &sts_state, &pkp_state));
  EXPECT_FALSE(GetStaticDomainState(&state, aypal, &sts_state, &pkp_state));
}

TEST_F(TransportSecurityStateStaticTest, PreloadedDomainSet) {
  TransportSecurityState state;
  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;

  // The domain wasn't being set, leading to a blank string in the
  // chrome://net-internals/#hsts UI. So test that.
  EXPECT_TRUE(
      state.GetStaticDomainState("market.android.com", &sts_state, &pkp_state));
  EXPECT_EQ(sts_state.domain, "market.android.com");
  EXPECT_EQ(pkp_state.domain, "market.android.com");
  EXPECT_TRUE(state.GetStaticDomainState("sub.market.android.com", &sts_state,
                                         &pkp_state));
  EXPECT_EQ(sts_state.domain, "market.android.com");
  EXPECT_EQ(pkp_state.domain, "market.android.com");
}

TEST_F(TransportSecurityStateStaticTest, Preloaded) {
  TransportSecurityState state;
  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;

  // We do more extensive checks for the first domain.
  EXPECT_TRUE(
      state.GetStaticDomainState("www.paypal.com", &sts_state, &pkp_state));
  EXPECT_EQ(sts_state.upgrade_mode,
            TransportSecurityState::STSState::MODE_FORCE_HTTPS);
  EXPECT_FALSE(sts_state.include_subdomains);
  EXPECT_FALSE(pkp_state.include_subdomains);

  EXPECT_TRUE(HasStaticState("paypal.com"));
  EXPECT_FALSE(HasStaticState("www2.paypal.com"));

  // Google hosts:

  EXPECT_TRUE(StaticShouldRedirect("chrome.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("checkout.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("wallet.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("docs.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("sites.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("drive.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("spreadsheets.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("appengine.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("market.android.com"));
  EXPECT_TRUE(StaticShouldRedirect("encrypted.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("accounts.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("profiles.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("mail.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("chatenabled.mail.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("talkgadget.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("hostedtalkgadget.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("talk.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("plus.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("groups.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("apis.google.com"));
  EXPECT_FALSE(StaticShouldRedirect("chart.apis.google.com"));
  EXPECT_TRUE(StaticShouldRedirect("ssl.google-analytics.com"));
  EXPECT_TRUE(StaticShouldRedirect("google"));
  EXPECT_TRUE(StaticShouldRedirect("foo.google"));
  EXPECT_TRUE(StaticShouldRedirect("foo"));
  EXPECT_TRUE(StaticShouldRedirect("domaintest.foo"));
  EXPECT_TRUE(StaticShouldRedirect("gmail.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.gmail.com"));
  EXPECT_TRUE(StaticShouldRedirect("googlemail.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.googlemail.com"));
  EXPECT_TRUE(StaticShouldRedirect("googleplex.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.googleplex.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.google-analytics.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.youtube.com"));
  EXPECT_TRUE(StaticShouldRedirect("youtube.com"));

  // These domains used to be only HSTS when SNI was available.
  EXPECT_TRUE(state.GetStaticDomainState("gmail.com", &sts_state, &pkp_state));
  EXPECT_TRUE(
      state.GetStaticDomainState("www.gmail.com", &sts_state, &pkp_state));
  EXPECT_TRUE(
      state.GetStaticDomainState("googlemail.com", &sts_state, &pkp_state));
  EXPECT_TRUE(
      state.GetStaticDomainState("www.googlemail.com", &sts_state, &pkp_state));

  // Other hosts:

  EXPECT_TRUE(StaticShouldRedirect("aladdinschools.appspot.com"));

  EXPECT_TRUE(StaticShouldRedirect("ottospora.nl"));
  EXPECT_TRUE(StaticShouldRedirect("www.ottospora.nl"));

  EXPECT_TRUE(StaticShouldRedirect("www.paycheckrecords.com"));

  EXPECT_TRUE(StaticShouldRedirect("lastpass.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.lastpass.com"));
  EXPECT_FALSE(HasStaticState("blog.lastpass.com"));

  EXPECT_TRUE(StaticShouldRedirect("keyerror.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.keyerror.com"));

  EXPECT_TRUE(StaticShouldRedirect("entropia.de"));
  EXPECT_TRUE(StaticShouldRedirect("www.entropia.de"));
  EXPECT_FALSE(HasStaticState("foo.entropia.de"));

  EXPECT_TRUE(StaticShouldRedirect("www.elanex.biz"));
  EXPECT_FALSE(HasStaticState("elanex.biz"));
  EXPECT_FALSE(HasStaticState("foo.elanex.biz"));

  EXPECT_TRUE(StaticShouldRedirect("sunshinepress.org"));
  EXPECT_TRUE(StaticShouldRedirect("www.sunshinepress.org"));
  EXPECT_TRUE(StaticShouldRedirect("a.b.sunshinepress.org"));

  EXPECT_TRUE(StaticShouldRedirect("www.noisebridge.net"));
  EXPECT_FALSE(HasStaticState("noisebridge.net"));
  EXPECT_FALSE(HasStaticState("foo.noisebridge.net"));

  EXPECT_TRUE(StaticShouldRedirect("neg9.org"));
  EXPECT_FALSE(HasStaticState("www.neg9.org"));

  EXPECT_TRUE(StaticShouldRedirect("riseup.net"));
  EXPECT_TRUE(StaticShouldRedirect("foo.riseup.net"));

  EXPECT_TRUE(StaticShouldRedirect("factor.cc"));
  EXPECT_FALSE(HasStaticState("www.factor.cc"));

  EXPECT_TRUE(StaticShouldRedirect("members.mayfirst.org"));
  EXPECT_TRUE(StaticShouldRedirect("support.mayfirst.org"));
  EXPECT_TRUE(StaticShouldRedirect("id.mayfirst.org"));
  EXPECT_TRUE(StaticShouldRedirect("lists.mayfirst.org"));
  EXPECT_FALSE(HasStaticState("www.mayfirst.org"));

  EXPECT_TRUE(StaticShouldRedirect("romab.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.romab.com"));
  EXPECT_TRUE(StaticShouldRedirect("foo.romab.com"));

  EXPECT_TRUE(StaticShouldRedirect("logentries.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.logentries.com"));
  EXPECT_FALSE(HasStaticState("foo.logentries.com"));

  EXPECT_TRUE(StaticShouldRedirect("stripe.com"));
  EXPECT_TRUE(StaticShouldRedirect("foo.stripe.com"));

  EXPECT_TRUE(StaticShouldRedirect("cloudsecurityalliance.org"));
  EXPECT_TRUE(StaticShouldRedirect("foo.cloudsecurityalliance.org"));

  EXPECT_TRUE(StaticShouldRedirect("login.sapo.pt"));
  EXPECT_TRUE(StaticShouldRedirect("foo.login.sapo.pt"));

  EXPECT_TRUE(StaticShouldRedirect("mattmccutchen.net"));
  EXPECT_TRUE(StaticShouldRedirect("foo.mattmccutchen.net"));

  EXPECT_TRUE(StaticShouldRedirect("betnet.fr"));
  EXPECT_TRUE(StaticShouldRedirect("foo.betnet.fr"));

  EXPECT_TRUE(StaticShouldRedirect("uprotect.it"));
  EXPECT_TRUE(StaticShouldRedirect("foo.uprotect.it"));

  EXPECT_TRUE(StaticShouldRedirect("squareup.com"));
  EXPECT_FALSE(HasStaticState("foo.squareup.com"));

  EXPECT_TRUE(StaticShouldRedirect("cert.se"));
  EXPECT_TRUE(StaticShouldRedirect("foo.cert.se"));

  EXPECT_TRUE(StaticShouldRedirect("crypto.is"));
  EXPECT_TRUE(StaticShouldRedirect("foo.crypto.is"));

  EXPECT_TRUE(StaticShouldRedirect("simon.butcher.name"));
  EXPECT_TRUE(StaticShouldRedirect("foo.simon.butcher.name"));

  EXPECT_TRUE(StaticShouldRedirect("linx.net"));
  EXPECT_TRUE(StaticShouldRedirect("foo.linx.net"));

  EXPECT_TRUE(StaticShouldRedirect("dropcam.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.dropcam.com"));
  EXPECT_FALSE(HasStaticState("foo.dropcam.com"));

  EXPECT_TRUE(StaticShouldRedirect("ebanking.indovinabank.com.vn"));
  EXPECT_TRUE(StaticShouldRedirect("foo.ebanking.indovinabank.com.vn"));

  EXPECT_TRUE(StaticShouldRedirect("epoxate.com"));
  EXPECT_FALSE(HasStaticState("foo.epoxate.com"));

  EXPECT_FALSE(HasStaticState("foo.torproject.org"));

  EXPECT_TRUE(StaticShouldRedirect("www.moneybookers.com"));
  EXPECT_FALSE(HasStaticState("moneybookers.com"));

  EXPECT_TRUE(StaticShouldRedirect("ledgerscope.net"));
  EXPECT_TRUE(StaticShouldRedirect("www.ledgerscope.net"));
  EXPECT_FALSE(HasStaticState("status.ledgerscope.net"));

  EXPECT_TRUE(StaticShouldRedirect("foo.app.recurly.com"));
  EXPECT_TRUE(StaticShouldRedirect("foo.api.recurly.com"));

  EXPECT_TRUE(StaticShouldRedirect("greplin.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.greplin.com"));
  EXPECT_FALSE(HasStaticState("foo.greplin.com"));

  EXPECT_TRUE(StaticShouldRedirect("luneta.nearbuysystems.com"));
  EXPECT_TRUE(StaticShouldRedirect("foo.luneta.nearbuysystems.com"));

  EXPECT_TRUE(StaticShouldRedirect("ubertt.org"));
  EXPECT_TRUE(StaticShouldRedirect("foo.ubertt.org"));

  EXPECT_TRUE(StaticShouldRedirect("pixi.me"));
  EXPECT_TRUE(StaticShouldRedirect("www.pixi.me"));

  EXPECT_TRUE(StaticShouldRedirect("grepular.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.grepular.com"));

  EXPECT_TRUE(StaticShouldRedirect("mydigipass.com"));
  EXPECT_FALSE(StaticShouldRedirect("foo.mydigipass.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.mydigipass.com"));
  EXPECT_FALSE(StaticShouldRedirect("foo.www.mydigipass.com"));
  EXPECT_TRUE(StaticShouldRedirect("developer.mydigipass.com"));
  EXPECT_FALSE(StaticShouldRedirect("foo.developer.mydigipass.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.developer.mydigipass.com"));
  EXPECT_FALSE(StaticShouldRedirect("foo.www.developer.mydigipass.com"));
  EXPECT_TRUE(StaticShouldRedirect("sandbox.mydigipass.com"));
  EXPECT_FALSE(StaticShouldRedirect("foo.sandbox.mydigipass.com"));
  EXPECT_TRUE(StaticShouldRedirect("www.sandbox.mydigipass.com"));
  EXPECT_FALSE(StaticShouldRedirect("foo.www.sandbox.mydigipass.com"));

  EXPECT_TRUE(StaticShouldRedirect("bigshinylock.minazo.net"));
  EXPECT_TRUE(StaticShouldRedirect("foo.bigshinylock.minazo.net"));

  EXPECT_TRUE(StaticShouldRedirect("crate.io"));
  EXPECT_TRUE(StaticShouldRedirect("foo.crate.io"));
}

TEST_F(TransportSecurityStateStaticTest, PreloadedPins) {
  TransportSecurityState state;
  EnableStaticPins(&state);
  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;

  // We do more extensive checks for the first domain.
  EXPECT_TRUE(
      state.GetStaticDomainState("www.paypal.com", &sts_state, &pkp_state));
  EXPECT_EQ(sts_state.upgrade_mode,
            TransportSecurityState::STSState::MODE_FORCE_HTTPS);
  EXPECT_FALSE(sts_state.include_subdomains);
  EXPECT_FALSE(pkp_state.include_subdomains);

  EXPECT_TRUE(OnlyPinningInStaticState("www.google.com"));
  EXPECT_TRUE(OnlyPinningInStaticState("foo.google.com"));
  EXPECT_TRUE(OnlyPinningInStaticState("google.com"));
  EXPECT_TRUE(OnlyPinningInStaticState("i.ytimg.com"));
  EXPECT_TRUE(OnlyPinningInStaticState("ytimg.com"));
  EXPECT_TRUE(OnlyPinningInStaticState("googleusercontent.com"));
  EXPECT_TRUE(OnlyPinningInStaticState("www.googleusercontent.com"));
  EXPECT_TRUE(OnlyPinningInStaticState("googleapis.com"));
  EXPECT_TRUE(OnlyPinningInStaticState("googleadservices.com"));
  EXPECT_TRUE(OnlyPinningInStaticState("googlecode.com"));
  EXPECT_TRUE(OnlyPinningInStaticState("appspot.com"));
  EXPECT_TRUE(OnlyPinningInStaticState("googlesyndication.com"));
  EXPECT_TRUE(OnlyPinningInStaticState("doubleclick.net"));
  EXPECT_TRUE(OnlyPinningInStaticState("googlegroups.com"));

  EXPECT_TRUE(HasStaticPublicKeyPins("torproject.org"));
  EXPECT_TRUE(HasStaticPublicKeyPins("www.torproject.org"));
  EXPECT_TRUE(HasStaticPublicKeyPins("check.torproject.org"));
  EXPECT_TRUE(HasStaticPublicKeyPins("blog.torproject.org"));
  EXPECT_FALSE(HasStaticState("foo.torproject.org"));

  EXPECT_TRUE(
      state.GetStaticDomainState("torproject.org", &sts_state, &pkp_state));
  EXPECT_FALSE(pkp_state.spki_hashes.empty());
  EXPECT_TRUE(
      state.GetStaticDomainState("www.torproject.org", &sts_state, &pkp_state));
  EXPECT_FALSE(pkp_state.spki_hashes.empty());
  EXPECT_TRUE(state.GetStaticDomainState("check.torproject.org", &sts_state,
                                         &pkp_state));
  EXPECT_FALSE(pkp_state.spki_hashes.empty());
  EXPECT_TRUE(state.GetStaticDomainState("blog.torproject.org", &sts_state,
                                         &pkp_state));
  EXPECT_FALSE(pkp_state.spki_hashes.empty());

  EXPECT_TRUE(HasStaticPublicKeyPins("www.twitter.com"));

  // Check that Facebook subdomains have pinning but not HSTS.
  EXPECT_TRUE(
      state.GetStaticDomainState("facebook.com", &sts_state, &pkp_state));
  EXPECT_FALSE(pkp_state.spki_hashes.empty());
  EXPECT_TRUE(StaticShouldRedirect("facebook.com"));

  EXPECT_TRUE(
      state.GetStaticDomainState("foo.facebook.com", &sts_state, &pkp_state));
  EXPECT_FALSE(pkp_state.spki_hashes.empty());
  EXPECT_FALSE(StaticShouldRedirect("foo.facebook.com"));

  EXPECT_TRUE(
      state.GetStaticDomainState("www.facebook.com", &sts_state, &pkp_state));
  EXPECT_FALSE(pkp_state.spki_hashes.empty());
  EXPECT_TRUE(StaticShouldRedirect("www.facebook.com"));

  EXPECT_TRUE(state.GetStaticDomainState("foo.www.facebook.com", &sts_state,
                                         &pkp_state));
  EXPECT_FALSE(pkp_state.spki_hashes.empty());
  EXPECT_TRUE(StaticShouldRedirect("foo.www.facebook.com"));
}

TEST_F(TransportSecurityStateStaticTest, BuiltinCertPins) {
  TransportSecurityState state;
  EnableStaticPins(&state);
  TransportSecurityState::STSState sts_state;
  TransportSecurityState::PKPState pkp_state;

  EXPECT_TRUE(
      state.GetStaticDomainState("chrome.google.com", &sts_state, &pkp_state));
  EXPECT_TRUE(HasStaticPublicKeyPins("chrome.google.com"));

  HashValueVector hashes;
  std::string failure_log;
  // Checks that a built-in list does exist.
  EXPECT_FALSE(pkp_state.CheckPublicKeyPins(hashes, &failure_log));
  EXPECT_FALSE(HasStaticPublicKeyPins("www.paypal.com"));

  EXPECT_TRUE(HasStaticPublicKeyPins("docs.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("1.docs.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("sites.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("drive.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("spreadsheets.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("wallet.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("checkout.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("appengine.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("market.android.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("encrypted.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("accounts.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("profiles.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("mail.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("chatenabled.mail.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("talkgadget.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("hostedtalkgadget.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("talk.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("plus.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("groups.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("apis.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("www.google-analytics.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("www.youtube.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("youtube.com"));

  EXPECT_TRUE(HasStaticPublicKeyPins("ssl.gstatic.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("gstatic.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("www.gstatic.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("ssl.google-analytics.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("www.googleplex.com"));

  EXPECT_TRUE(HasStaticPublicKeyPins("twitter.com"));
  EXPECT_FALSE(HasStaticPublicKeyPins("foo.twitter.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("www.twitter.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("api.twitter.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("oauth.twitter.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("mobile.twitter.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("dev.twitter.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("business.twitter.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("platform.twitter.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("si0.twimg.com"));
}

TEST_F(TransportSecurityStateStaticTest, OptionalHSTSCertPins) {
  TransportSecurityState state;
  EnableStaticPins(&state);

  EXPECT_TRUE(HasStaticPublicKeyPins("google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("www.google.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("mail-attachment.googleusercontent.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("www.youtube.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("i.ytimg.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("googleapis.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("ajax.googleapis.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("googleadservices.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("pagead2.googleadservices.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("googlecode.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("kibbles.googlecode.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("appspot.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("googlesyndication.com"));
  EXPECT_TRUE(HasStaticPublicKeyPins("doubleclick.net"));
  EXPECT_TRUE(HasStaticPublicKeyPins("ad.doubleclick.net"));
  EXPECT_FALSE(HasStaticPublicKeyPins("learn.doubleclick.net"));
  EXPECT_TRUE(HasStaticPublicKeyPins("a.googlegroups.com"));
}

TEST_F(TransportSecurityStateStaticTest, OverrideBuiltins) {
  EXPECT_TRUE(HasStaticPublicKeyPins("google.com"));
  EXPECT_FALSE(StaticShouldRedirect("google.com"));
  EXPECT_FALSE(StaticShouldRedirect("www.google.com"));

  TransportSecurityState state;
  const base::Time current_time(base::Time::Now());
  const base::Time expiry = current_time + base::TimeDelta::FromSeconds(1000);
  state.AddHSTS("www.google.com", expiry, true);

  EXPECT_TRUE(state.ShouldUpgradeToSSL("www.google.com"));
}
#endif  // BUILDFLAG(INCLUDE_TRANSPORT_SECURITY_STATE_PRELOAD_LIST)

}  // namespace net
