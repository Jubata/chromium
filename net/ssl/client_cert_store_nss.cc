// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/ssl/client_cert_store_nss.h"

#include <nss.h>
#include <ssl.h>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/strings/string_piece.h"
#include "base/task_scheduler/post_task.h"
#include "base/threading/scoped_blocking_call.h"
#include "crypto/nss_crypto_module_delegate.h"
#include "net/cert/scoped_nss_types.h"
#include "net/cert/x509_util_nss.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "net/ssl/ssl_platform_key_nss.h"
#include "net/ssl/threaded_ssl_private_key.h"
#include "net/third_party/nss/ssl/cmpcert.h"
#include "third_party/boringssl/src/include/openssl/pool.h"

namespace net {

namespace {

class ClientCertIdentityNSS : public ClientCertIdentity {
 public:
  ClientCertIdentityNSS(
      scoped_refptr<net::X509Certificate> cert,
      ScopedCERTCertificate cert_certificate,
      scoped_refptr<crypto::CryptoModuleBlockingPasswordDelegate>
          password_delegate)
      : ClientCertIdentity(std::move(cert)),
        cert_certificate_(std::move(cert_certificate)),
        password_delegate_(std::move(password_delegate)) {}
  ~ClientCertIdentityNSS() override = default;

  void AcquirePrivateKey(
      const base::Callback<void(scoped_refptr<SSLPrivateKey>)>&
          private_key_callback) override {
    // Caller is responsible for keeping the ClientCertIdentity alive until
    // the |private_key_callback| is run, so it's safe to use Unretained here.
    base::PostTaskWithTraitsAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
        base::Bind(&FetchClientCertPrivateKey, base::Unretained(certificate()),
                   cert_certificate_.get(), password_delegate_),
        private_key_callback);
  }

 private:
  ScopedCERTCertificate cert_certificate_;
  scoped_refptr<crypto::CryptoModuleBlockingPasswordDelegate>
      password_delegate_;
};

}  // namespace

ClientCertStoreNSS::ClientCertStoreNSS(
    const PasswordDelegateFactory& password_delegate_factory)
    : password_delegate_factory_(password_delegate_factory) {}

ClientCertStoreNSS::~ClientCertStoreNSS() {}

void ClientCertStoreNSS::GetClientCerts(
    const SSLCertRequestInfo& request,
    const ClientCertListCallback& callback) {
  scoped_refptr<crypto::CryptoModuleBlockingPasswordDelegate> password_delegate;
  if (!password_delegate_factory_.is_null())
    password_delegate = password_delegate_factory_.Run(request.host_and_port);
  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::Bind(&ClientCertStoreNSS::GetAndFilterCertsOnWorkerThread,
                 // Caller is responsible for keeping the ClientCertStore
                 // alive until the callback is run.
                 base::Unretained(this), std::move(password_delegate),
                 base::Unretained(&request)),
      callback);
}

// static
void ClientCertStoreNSS::FilterCertsOnWorkerThread(
    ClientCertIdentityList* identities,
    const SSLCertRequestInfo& request) {
  size_t num_raw = 0;

  auto keep_iter = identities->begin();

  base::Time now = base::Time::Now();

  for (auto examine_iter = identities->begin();
       examine_iter != identities->end(); ++examine_iter) {
    ++num_raw;

    X509Certificate* cert = (*examine_iter)->certificate();

    // Only offer unexpired certificates.
    if (now < cert->valid_start() || now > cert->valid_expiry()) {
      continue;
    }

    ScopedCERTCertificateList nss_intermediates;
    if (!MatchClientCertificateIssuers(cert, request.cert_authorities,
                                       &nss_intermediates)) {
      continue;
    }

    X509Certificate::OSCertHandles intermediates_raw;
    intermediates_raw.reserve(nss_intermediates.size());
    std::vector<bssl::UniquePtr<CRYPTO_BUFFER>> intermediates;
    intermediates.reserve(nss_intermediates.size());
    for (const ScopedCERTCertificate& nss_intermediate : nss_intermediates) {
      bssl::UniquePtr<CRYPTO_BUFFER> intermediate_cert_handle(
          X509Certificate::CreateOSCertHandleFromBytes(
              reinterpret_cast<const char*>(nss_intermediate->derCert.data),
              nss_intermediate->derCert.len));
      if (!intermediate_cert_handle)
        break;
      intermediates_raw.push_back(intermediate_cert_handle.get());
      intermediates.push_back(std::move(intermediate_cert_handle));
    }

    // Retain a copy of the intermediates. Some deployments expect the client to
    // supply intermediates out of the local store. See
    // https://crbug.com/548631.
    (*examine_iter)->SetIntermediates(intermediates_raw);

    if (examine_iter == keep_iter)
      ++keep_iter;
    else
      *keep_iter++ = std::move(*examine_iter);
  }
  identities->erase(keep_iter, identities->end());

  DVLOG(2) << "num_raw:" << num_raw << " num_filtered:" << identities->size();

  std::sort(identities->begin(), identities->end(), ClientCertIdentitySorter());
}

ClientCertIdentityList ClientCertStoreNSS::GetAndFilterCertsOnWorkerThread(
    scoped_refptr<crypto::CryptoModuleBlockingPasswordDelegate>
        password_delegate,
    const SSLCertRequestInfo* request) {
  // This method may acquire the NSS lock or reenter this code via extension
  // hooks (such as smart card UI). To ensure threads are not starved or
  // deadlocked, the base::ScopedBlockingCall below increments the thread pool
  // capacity if this method takes too much time to run.
  base::ScopedBlockingCall scoped_blocking_call(base::BlockingType::MAY_BLOCK);
  ClientCertIdentityList selected_identities;
  GetPlatformCertsOnWorkerThread(std::move(password_delegate), CertFilter(),
                                 &selected_identities);
  FilterCertsOnWorkerThread(&selected_identities, *request);
  return selected_identities;
}

// static
void ClientCertStoreNSS::GetPlatformCertsOnWorkerThread(
    scoped_refptr<crypto::CryptoModuleBlockingPasswordDelegate>
        password_delegate,
    const CertFilter& cert_filter,
    ClientCertIdentityList* identities) {
  CERTCertList* found_certs =
      CERT_FindUserCertsByUsage(CERT_GetDefaultCertDB(), certUsageSSLClient,
                                PR_FALSE, PR_FALSE, password_delegate.get());
  if (!found_certs) {
    DVLOG(2) << "No client certs found.";
    return;
  }
  for (CERTCertListNode* node = CERT_LIST_HEAD(found_certs);
       !CERT_LIST_END(node, found_certs); node = CERT_LIST_NEXT(node)) {
    if (!cert_filter.is_null() && !cert_filter.Run(node->cert))
      continue;
    // Allow UTF-8 inside PrintableStrings in client certificates. See
    // crbug.com/770323.
    X509Certificate::UnsafeCreateOptions options;
    options.printable_string_is_utf8 = true;
    scoped_refptr<X509Certificate> cert =
        x509_util::CreateX509CertificateFromCERTCertificate(node->cert, {},
                                                            options);
    if (!cert) {
      DVLOG(2) << "x509_util::CreateX509CertificateFromCERTCertificate failed";
      continue;
    }
    identities->push_back(std::make_unique<ClientCertIdentityNSS>(
        cert, x509_util::DupCERTCertificate(node->cert), password_delegate));
  }
  CERT_DestroyCertList(found_certs);
}

}  // namespace net
