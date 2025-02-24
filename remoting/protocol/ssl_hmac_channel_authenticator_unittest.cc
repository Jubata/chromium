// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "remoting/protocol/ssl_hmac_channel_authenticator.h"

#include <utility>

#include "base/base64.h"
#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/macros.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "base/test/test_timeouts.h"
#include "base/timer/timer.h"
#include "crypto/rsa_private_key.h"
#include "net/base/net_errors.h"
#include "net/test/cert_test_util.h"
#include "net/test/test_data_directory.h"
#include "remoting/base/rsa_key_pair.h"
#include "remoting/protocol/connection_tester.h"
#include "remoting/protocol/fake_session.h"
#include "remoting/protocol/p2p_stream_socket.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/libjingle_xmpp/xmllite/xmlelement.h"

using testing::_;
using testing::NotNull;
using testing::SaveArg;

namespace remoting {
namespace protocol {

namespace {

const char kTestSharedSecret[] = "1234-1234-5678";
const char kTestSharedSecretBad[] = "0000-0000-0001";

class MockChannelDoneCallback {
 public:
  MOCK_METHOD2(OnDone, void(int error, P2PStreamSocket* socket));
};

ACTION_P(QuitThreadOnCounter, counter) {
  --(*counter);
  EXPECT_GE(*counter, 0);
  if (*counter == 0)
    base::RunLoop::QuitCurrentWhenIdleDeprecated();
}

}  // namespace

class SslHmacChannelAuthenticatorTest : public testing::Test {
 public:
  SslHmacChannelAuthenticatorTest() {}
  ~SslHmacChannelAuthenticatorTest() override {}

 protected:
  void SetUp() override {
    base::FilePath certs_dir(net::GetTestCertsDirectory());

    base::FilePath cert_path = certs_dir.AppendASCII("unittest.selfsigned.der");
    ASSERT_TRUE(base::ReadFileToString(cert_path, &host_cert_));

    base::FilePath key_path = certs_dir.AppendASCII("unittest.key.bin");
    std::string key_string;
    ASSERT_TRUE(base::ReadFileToString(key_path, &key_string));
    std::string key_base64;
    base::Base64Encode(key_string, &key_base64);
    key_pair_ = RsaKeyPair::FromString(key_base64);
    ASSERT_TRUE(key_pair_.get());
  }

  void RunChannelAuth(int expected_client_error, int expected_host_error) {
    client_fake_socket_.reset(new FakeStreamSocket());
    host_fake_socket_.reset(new FakeStreamSocket());
    client_fake_socket_->PairWith(host_fake_socket_.get());

    client_auth_->SecureAndAuthenticate(
        std::move(client_fake_socket_),
        base::Bind(&SslHmacChannelAuthenticatorTest::OnClientConnected,
                   base::Unretained(this)));

    host_auth_->SecureAndAuthenticate(
        std::move(host_fake_socket_),
        base::Bind(&SslHmacChannelAuthenticatorTest::OnHostConnected,
                   base::Unretained(this), std::string("ref argument value")));

    // Expect two callbacks to be called - the client callback and the host
    // callback.
    int callback_counter = 2;

    if (expected_client_error != net::OK) {
      EXPECT_CALL(client_callback_, OnDone(expected_client_error, nullptr))
          .WillOnce(QuitThreadOnCounter(&callback_counter));
    } else {
      EXPECT_CALL(client_callback_, OnDone(net::OK, NotNull()))
          .WillOnce(QuitThreadOnCounter(&callback_counter));
    }

    if (expected_host_error != net::OK) {
      EXPECT_CALL(host_callback_, OnDone(expected_host_error, nullptr))
          .WillOnce(QuitThreadOnCounter(&callback_counter));
    } else {
      EXPECT_CALL(host_callback_, OnDone(net::OK, NotNull()))
          .WillOnce(QuitThreadOnCounter(&callback_counter));
    }

    // Ensure that .Run() does not run unbounded if the callbacks are never
    // called.
    base::Timer shutdown_timer(false, false);
    shutdown_timer.Start(FROM_HERE, TestTimeouts::action_timeout(),
                         base::MessageLoop::QuitWhenIdleClosure());
    base::RunLoop().Run();
  }

  void OnHostConnected(const std::string& ref_argument,
                       int error,
                       std::unique_ptr<P2PStreamSocket> socket) {
    // Try deleting the authenticator and verify that this doesn't destroy
    // reference parameters.
    host_auth_.reset();
    DCHECK_EQ(ref_argument, "ref argument value");

    host_callback_.OnDone(error, socket.get());
    host_socket_ = std::move(socket);
  }

  void OnClientConnected(int error, std::unique_ptr<P2PStreamSocket> socket) {
    client_auth_.reset();
    client_callback_.OnDone(error, socket.get());
    client_socket_ = std::move(socket);
  }

  base::MessageLoop message_loop_;

  scoped_refptr<RsaKeyPair> key_pair_;
  std::string host_cert_;
  std::unique_ptr<FakeStreamSocket> client_fake_socket_;
  std::unique_ptr<FakeStreamSocket> host_fake_socket_;
  std::unique_ptr<ChannelAuthenticator> client_auth_;
  std::unique_ptr<ChannelAuthenticator> host_auth_;
  MockChannelDoneCallback client_callback_;
  MockChannelDoneCallback host_callback_;
  std::unique_ptr<P2PStreamSocket> client_socket_;
  std::unique_ptr<P2PStreamSocket> host_socket_;

  DISALLOW_COPY_AND_ASSIGN(SslHmacChannelAuthenticatorTest);
};

// Verify that a channel can be connected using a valid shared secret.
TEST_F(SslHmacChannelAuthenticatorTest, SuccessfulAuth) {
  client_auth_ = SslHmacChannelAuthenticator::CreateForClient(
      host_cert_, kTestSharedSecret);
  host_auth_ = SslHmacChannelAuthenticator::CreateForHost(
      host_cert_, key_pair_, kTestSharedSecret);

  RunChannelAuth(net::OK, net::OK);

  ASSERT_TRUE(client_socket_.get() != nullptr);
  ASSERT_TRUE(host_socket_.get() != nullptr);

  StreamConnectionTester tester(host_socket_.get(), client_socket_.get(),
                                100, 2);

  tester.Start();
  base::RunLoop().Run();
  tester.CheckResults();
}

// Verify that channels cannot be using invalid shared secret.
TEST_F(SslHmacChannelAuthenticatorTest, InvalidChannelSecret) {
  client_auth_ = SslHmacChannelAuthenticator::CreateForClient(
      host_cert_, kTestSharedSecretBad);
  host_auth_ = SslHmacChannelAuthenticator::CreateForHost(
      host_cert_, key_pair_, kTestSharedSecret);

  RunChannelAuth(net::ERR_FAILED, net::ERR_FAILED);

  ASSERT_TRUE(host_socket_.get() == nullptr);
}

// Verify that channels cannot be using invalid certificate.
TEST_F(SslHmacChannelAuthenticatorTest, InvalidCertificate) {
  // Import a second certificate for the client to expect.
  scoped_refptr<net::X509Certificate> host_cert2(
      net::ImportCertFromFile(net::GetTestCertsDirectory(), "ok_cert.pem"));
  std::string host_cert2_der;
  ASSERT_TRUE(net::X509Certificate::GetDEREncoded(host_cert2->os_cert_handle(),
                                                  &host_cert2_der));

  client_auth_ = SslHmacChannelAuthenticator::CreateForClient(
      host_cert2_der, kTestSharedSecret);
  host_auth_ = SslHmacChannelAuthenticator::CreateForHost(
      host_cert_, key_pair_, kTestSharedSecret);

  RunChannelAuth(net::ERR_CERT_INVALID, net::ERR_CONNECTION_CLOSED);

  ASSERT_TRUE(host_socket_.get() == nullptr);
}

}  // namespace protocol
}  // namespace remoting
