// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cronet_test_base.h"

#include "components/grpc_support/test/quic_test_server.h"
#include "crypto/sha2.h"
#include "net/base/net_errors.h"
#include "net/cert/asn1_util.h"
#include "net/cert/mock_cert_verifier.h"
#include "net/test/cert_test_util.h"
#include "net/test/test_data_directory.h"

#pragma mark

@implementation TestDelegate {
  // Dictionary which maps tasks to completion semaphores for this TestDelegate.
  // When a request this delegate is attached to finishes (either successfully
  // or with an error), this delegate signals that task's semaphore.
  NSMutableDictionary<NSURLSessionTask*, dispatch_semaphore_t>* _semaphores;

  NSMutableDictionary<NSURLSessionDataTask*, NSMutableArray<NSData*>*>*
      _responseDataPerTask;
}

@synthesize errorPerTask = _errorPerTask;
@synthesize totalBytesReceivedPerTask = _totalBytesReceivedPerTask;
@synthesize expectedContentLengthPerTask = _expectedContentLengthPerTask;

- (id)init {
  if (self = [super init]) {
    _semaphores = [NSMutableDictionary dictionaryWithCapacity:0];
  }
  return self;
}

- (void)reset {
  _responseDataPerTask = [NSMutableDictionary dictionaryWithCapacity:0];
  _errorPerTask = [NSMutableDictionary dictionaryWithCapacity:0];
  _totalBytesReceivedPerTask = [NSMutableDictionary dictionaryWithCapacity:0];
  _expectedContentLengthPerTask =
      [NSMutableDictionary dictionaryWithCapacity:0];
}

- (NSError*)error {
  if ([_errorPerTask count] == 0)
    return nil;

  DCHECK([_errorPerTask count] == 1);
  return [[_errorPerTask objectEnumerator] nextObject];
}

- (long)totalBytesReceived {
  DCHECK([_totalBytesReceivedPerTask count] == 1);
  return [[[_totalBytesReceivedPerTask objectEnumerator] nextObject] intValue];
}

- (long)expectedContentLength {
  DCHECK([_expectedContentLengthPerTask count] == 1);
  return
      [[[_expectedContentLengthPerTask objectEnumerator] nextObject] intValue];
}

- (NSString*)responseBody {
  if ([_responseDataPerTask count] == 0)
    return nil;

  DCHECK([_responseDataPerTask count] == 1);
  NSURLSessionDataTask* task =
      [[_responseDataPerTask keyEnumerator] nextObject];

  return [self responseBody:task];
}

- (NSString*)responseBody:(NSURLSessionDataTask*)task {
  if (_responseDataPerTask[task] == nil) {
    return nil;
  }
  NSMutableString* body = [NSMutableString string];
  for (NSData* data in _responseDataPerTask[task]) {
    [body appendString:[[NSString alloc] initWithData:data
                                             encoding:NSUTF8StringEncoding]];
  }
  VLOG(3) << "responseBody size:" << [body length]
          << " chunks:" << [_responseDataPerTask[task] count];
  return body;
}

- (dispatch_semaphore_t)getSemaphoreForTask:(NSURLSessionTask*)task {
  @synchronized(_semaphores) {
    if (!_semaphores[task]) {
      _semaphores[task] = dispatch_semaphore_create(0);
    }
    return _semaphores[task];
  }
}

// |timeout_ns|, if positive, specifies how long to wait before timing out in
// nanoseconds, a value of 0 or less means do not ever time out.
- (BOOL)waitForDone:(NSURLSessionDataTask*)task
        withTimeout:(int64_t)timeout_ns {
  dispatch_semaphore_t _semaphore = [self getSemaphoreForTask:task];
  if (timeout_ns > 0) {
    BOOL request_completed =
        dispatch_semaphore_wait(
            _semaphore, dispatch_time(DISPATCH_TIME_NOW, timeout_ns)) == 0;
    if (!request_completed) {
      // Cancel the pending request; otherwise, the request is still active and
      // may invoke the delegate methods later.
      [task cancel];
      LOG(WARNING) << "The request was canceled due to timeout.";
      // Give the canceled request some time to execute didCompleteWithError
      // method with NSURLErrorCancelled error code.
      dispatch_semaphore_wait(
          _semaphore, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    }
    return request_completed;
  } else {
    return dispatch_semaphore_wait(_semaphore, DISPATCH_TIME_FOREVER) == 0;
  }
}

- (void)URLSession:(NSURLSession*)session
    didBecomeInvalidWithError:(NSError*)error {
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
  if (error)
    _errorPerTask[task] = error;

  dispatch_semaphore_t _semaphore = [self getSemaphoreForTask:task];
  dispatch_semaphore_signal(_semaphore);
}

- (void)URLSession:(NSURLSession*)session
                   task:(NSURLSessionTask*)task
    didReceiveChallenge:(NSURLAuthenticationChallenge*)challenge
      completionHandler:
          (void (^)(NSURLSessionAuthChallengeDisposition disp,
                    NSURLCredential* credential))completionHandler {
  completionHandler(NSURLSessionAuthChallengeUseCredential, nil);
}

- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)dataTask
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition disposition))
                           completionHandler {
  _expectedContentLengthPerTask[dataTask] =
      [NSNumber numberWithInt:[response expectedContentLength]];
  completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)dataTask
    didReceiveData:(NSData*)data {
  if (_totalBytesReceivedPerTask[dataTask]) {
    _totalBytesReceivedPerTask[dataTask] = [NSNumber
        numberWithInt:[_totalBytesReceivedPerTask[dataTask] intValue] +
                      [data length]];
  } else {
    _totalBytesReceivedPerTask[dataTask] =
        [NSNumber numberWithInt:[data length]];
  }

  if (_responseDataPerTask[dataTask] == nil) {
    _responseDataPerTask[dataTask] = [[NSMutableArray alloc] init];
  }
  [_responseDataPerTask[dataTask] addObject:data];
}

- (void)URLSession:(NSURLSession*)session
             dataTask:(NSURLSessionDataTask*)dataTask
    willCacheResponse:(NSCachedURLResponse*)proposedResponse
    completionHandler:
        (void (^)(NSCachedURLResponse* cachedResponse))completionHandler {
  completionHandler(proposedResponse);
}

@end

namespace cronet {

void CronetTestBase::SetUp() {
  ::testing::Test::SetUp();
  grpc_support::StartQuicTestServer();
  delegate_ = [[TestDelegate alloc] init];
}

void CronetTestBase::TearDown() {
  grpc_support::ShutdownQuicTestServer();
  ::testing::Test::TearDown();
}

// Launches the supplied |task| and blocks until it completes, with a default
// timeout of 20 seconds.  |deadline_ns|, if specified, is in nanoseconds.
// If |deadline_ns| is 0 or negative, the request will not time out.
bool CronetTestBase::StartDataTaskAndWaitForCompletion(
    NSURLSessionDataTask* task,
    int64_t deadline_ns) {
  [delegate_ reset];
  [task resume];
  return [delegate_ waitForDone:task withTimeout:deadline_ns];
}

::testing::AssertionResult CronetTestBase::IsResponseSuccessful() {
  if ([delegate_ error])
    return ::testing::AssertionFailure() << "error in response: " <<
           [[[delegate_ error] description]
               cStringUsingEncoding:NSUTF8StringEncoding];
  else
    return ::testing::AssertionSuccess() << "no errors in response";
}

::testing::AssertionResult CronetTestBase::IsResponseCanceled() {
  if ([delegate_ error] && [[delegate_ error] code] == NSURLErrorCancelled)
    return ::testing::AssertionSuccess() << "the response is canceled";
  else
    return ::testing::AssertionFailure() << "the response is not canceled."
                                         << " The response error is " <<
           [[[delegate_ error] description]
               cStringUsingEncoding:NSUTF8StringEncoding];
}

std::unique_ptr<net::MockCertVerifier> CronetTestBase::CreateMockCertVerifier(
    const std::vector<std::string>& certs,
    bool known_root) {
  std::unique_ptr<net::MockCertVerifier> mock_cert_verifier(
      new net::MockCertVerifier());
  for (const auto& cert : certs) {
    net::CertVerifyResult verify_result;
    verify_result.verified_cert =
        net::ImportCertFromFile(net::GetTestCertsDirectory(), cert);

    // By default, HPKP verification is enabled for known trust roots only.
    verify_result.is_issued_by_known_root = known_root;

    // Calculate the public key hash and add it to the verify_result.
    net::HashValue hashValue;
    CHECK(CalculatePublicKeySha256(*verify_result.verified_cert.get(),
                                   &hashValue));
    verify_result.public_key_hashes.push_back(hashValue);

    mock_cert_verifier->AddResultForCert(verify_result.verified_cert.get(),
                                         verify_result, net::OK);
  }
  return mock_cert_verifier;
}

bool CronetTestBase::CalculatePublicKeySha256(const net::X509Certificate& cert,
                                              net::HashValue* out_hash_value) {
  // Convert the cert to DER encoded bytes.
  std::string der_cert_bytes;
  net::X509Certificate::OSCertHandle cert_handle = cert.os_cert_handle();
  if (!net::X509Certificate::GetDEREncoded(cert_handle, &der_cert_bytes)) {
    LOG(INFO) << "Unable to convert the given cert to DER encoding";
    return false;
  }
  // Extract the public key from the cert.
  base::StringPiece spki_bytes;
  if (!net::asn1::ExtractSPKIFromDERCert(der_cert_bytes, &spki_bytes)) {
    LOG(INFO) << "Unable to retrieve the public key from the DER cert";
    return false;
  }
  // Calculate SHA256 hash of public key bytes.
  out_hash_value->tag = net::HASH_VALUE_SHA256;
  crypto::SHA256HashString(spki_bytes, out_hash_value->data(),
                           crypto::kSHA256Length);
  return true;
}

}  // namespace cronet
