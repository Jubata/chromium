// Copyright (c) 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/http_cache_writers.h"

#include <algorithm>
#include <utility>

#include "base/auto_reset.h"
#include "base/logging.h"

#include "net/base/net_errors.h"
#include "net/disk_cache/disk_cache.h"
#include "net/http/http_cache_transaction.h"
#include "net/http/http_response_info.h"
#include "net/http/partial_data.h"

namespace net {

namespace {

bool IsValidResponseForWriter(bool is_partial,
                              const HttpResponseInfo* response_info) {
  if (!response_info->headers.get())
    return false;

  // Return false if the response code sent by the server is garbled.
  // Both 200 and 304 are valid since concurrent writing is supported.
  if (!is_partial && (response_info->headers->response_code() != 200 &&
                      response_info->headers->response_code() != 304)) {
    return false;
  }

  return true;
}

}  // namespace

HttpCache::Writers::TransactionInfo::TransactionInfo(PartialData* partial_data,
                                                     const bool is_truncated,
                                                     HttpResponseInfo info)
    : partial(partial_data), truncated(is_truncated), response_info(info) {}

HttpCache::Writers::TransactionInfo::~TransactionInfo() = default;

HttpCache::Writers::TransactionInfo::TransactionInfo(const TransactionInfo&) =
    default;

HttpCache::Writers::Writers(HttpCache* cache, HttpCache::ActiveEntry* entry)
    : cache_(cache), entry_(entry), weak_factory_(this) {}

HttpCache::Writers::~Writers() {}

int HttpCache::Writers::Read(scoped_refptr<IOBuffer> buf,
                             int buf_len,
                             const CompletionCallback& callback,
                             Transaction* transaction) {
  DCHECK(buf);
  DCHECK_GT(buf_len, 0);
  DCHECK(!callback.is_null());
  DCHECK(transaction);

  // If another transaction invoked a Read which is currently ongoing, then
  // this transaction waits for the read to complete and gets its buffer filled
  // with the data returned from that read.
  if (next_state_ != State::NONE) {
    WaitingForRead read_info(buf, buf_len, callback);
    waiting_for_read_.insert(std::make_pair(transaction, read_info));
    return ERR_IO_PENDING;
  }

  DCHECK_EQ(next_state_, State::NONE);
  DCHECK(callback_.is_null());
  DCHECK_EQ(nullptr, active_transaction_);
  DCHECK(HasTransaction(transaction));
  active_transaction_ = transaction;

  read_buf_ = std::move(buf);
  io_buf_len_ = buf_len;
  next_state_ = State::NETWORK_READ;

  int rv = DoLoop(OK);
  if (rv == ERR_IO_PENDING)
    callback_ = callback;

  return rv;
}

bool HttpCache::Writers::StopCaching(bool keep_entry) {
  // If this is the only transaction in Writers, then stopping will be
  // successful. If not, then we will not stop caching since there are
  // other consumers waiting to read from the cache.
  if (all_writers_.size() != 1)
    return false;

  network_read_only_ = true;
  if (!keep_entry) {
    should_keep_entry_ = false;
    cache_->WritersDoomEntryRestartTransactions(entry_);
  }

  return true;
}

void HttpCache::Writers::AddTransaction(Transaction* transaction,
                                        bool is_exclusive,
                                        RequestPriority priority,
                                        const TransactionInfo& info) {
  DCHECK(transaction);
  DCHECK(CanAddWriters());

  DCHECK_EQ(0u, all_writers_.count(transaction));

  // Set truncation related information.
  response_info_truncation_ = info.response_info;
  should_keep_entry_ =
      IsValidResponseForWriter(info.partial != nullptr, &(info.response_info));

  if (info.partial && !info.truncated) {
    DCHECK(!partial_do_not_truncate_);
    partial_do_not_truncate_ = true;
  }

  std::pair<Transaction*, TransactionInfo> writer(transaction, info);
  all_writers_.insert(writer);

  if (is_exclusive) {
    DCHECK_EQ(1u, all_writers_.size());
    is_exclusive_ = true;
  }

  priority_ = std::max(priority, priority_);
  if (network_transaction_) {
    network_transaction_->SetPriority(priority_);
  }
}

void HttpCache::Writers::SetNetworkTransaction(
    Transaction* transaction,
    std::unique_ptr<HttpTransaction> network_transaction) {
  DCHECK_EQ(1u, all_writers_.count(transaction));
  DCHECK(network_transaction);
  DCHECK(!network_transaction_);
  network_transaction_ = std::move(network_transaction);
  network_transaction_->SetPriority(priority_);
}

void HttpCache::Writers::ResetNetworkTransaction() {
  DCHECK(is_exclusive_);
  DCHECK_EQ(1u, all_writers_.size());
  DCHECK(all_writers_.begin()->second.partial);
  network_transaction_.reset();
}

void HttpCache::Writers::RemoveTransaction(Transaction* transaction,
                                           bool success) {
  EraseTransaction(transaction, OK);

  if (!all_writers_.empty())
    return;

  if (!success) {
    DCHECK_NE(State::CACHE_WRITE_TRUNCATED_RESPONSE, next_state_);
    if (InitiateTruncateEntry()) {
      // |this| may have been deleted after truncation, so don't touch any
      // members.
      return;
    }
  }

  cache_->WritersDoneWritingToEntry(entry_, success, should_keep_entry_,
                                    TransactionSet());
}

void HttpCache::Writers::EraseTransaction(Transaction* transaction,
                                          int result) {
  // The transaction should be part of all_writers.
  auto it = all_writers_.find(transaction);
  DCHECK(it != all_writers_.end());
  EraseTransaction(it, result);
}

HttpCache::Writers::TransactionMap::iterator
HttpCache::Writers::EraseTransaction(TransactionMap::iterator it, int result) {
  Transaction* transaction = it->first;
  transaction->WriterAboutToBeRemovedFromEntry(result);

  TransactionMap::iterator return_it = all_writers_.erase(it);

  if (all_writers_.empty() && next_state_ == State::NONE) {
    // This needs to be called to handle the edge case where even before Read is
    // invoked all transactions are removed. In that case the
    // network_transaction_ will still have a valid request info and so it
    // should be destroyed before its consumer is destroyed (request info
    // is a raw pointer owned by its consumer).
    network_transaction_.reset();
  } else {
    UpdatePriority();
  }

  if (active_transaction_ == transaction) {
    active_transaction_ = nullptr;
  } else {
    // If waiting for read, remove it from the map.
    waiting_for_read_.erase(transaction);
  }
  return return_it;
}

void HttpCache::Writers::UpdatePriority() {
  // Get the current highest priority.
  RequestPriority current_highest = MINIMUM_PRIORITY;
  for (auto& writer : all_writers_) {
    Transaction* transaction = writer.first;
    current_highest = std::max(transaction->priority(), current_highest);
  }

  if (priority_ != current_highest) {
    if (network_transaction_)
      network_transaction_->SetPriority(current_highest);
    priority_ = current_highest;
  }
}

bool HttpCache::Writers::ContainsOnlyIdleWriters() const {
  return waiting_for_read_.empty() && !active_transaction_;
}

bool HttpCache::Writers::CanAddWriters() {
  //  While cleaning up writers (truncation) we should delay adding new writers.
  //  The caller can try again later.
  if (next_state_ == State::ASYNC_OP_COMPLETE_PRE_TRUNCATE ||
      next_state_ == State::CACHE_WRITE_TRUNCATED_RESPONSE_COMPLETE) {
    DCHECK(all_writers_.empty());
    return false;
  }

  if (all_writers_.empty())
    return true;

  return !is_exclusive_ && !network_read_only_;
}

void HttpCache::Writers::ProcessFailure(int error) {
  // Notify waiting_for_read_ of the failure. Tasks will be posted for all the
  // transactions.
  ProcessWaitingForReadTransactions(error);

  // Idle readers should fail when Read is invoked on them.
  SetIdleWritersFailState(error);
}

bool HttpCache::Writers::InitiateTruncateEntry() {
  // If there is already an operation ongoing in the state machine, queue the
  // truncation to happen after the outstanding operation is complete by setting
  // the state.
  if (next_state_ != State::NONE) {
    DCHECK(next_state_ == State::CACHE_WRITE_DATA_COMPLETE ||
           next_state_ == State::NETWORK_READ_COMPLETE);
    next_state_ = State::ASYNC_OP_COMPLETE_PRE_TRUNCATE;
    return true;
  }

  if (!ShouldTruncate())
    return false;

  next_state_ = State::CACHE_WRITE_TRUNCATED_RESPONSE;
  // If not in do loop, initiate do loop.
  if (!in_do_loop_)
    DoLoop(OK);
  return true;
}

bool HttpCache::Writers::ShouldTruncate() {
  // Don't set the flag for sparse entries or for entries that cannot be
  // resumed.
  if (!should_keep_entry_ || partial_do_not_truncate_)
    return false;

  // Check the response headers for strong validators.
  // Note that if this is a 206, content-length was already fixed after calling
  // PartialData::ResponseHeadersOK().
  if (response_info_truncation_.headers->GetContentLength() <= 0 ||
      response_info_truncation_.headers->HasHeaderValue("Accept-Ranges",
                                                        "none") ||
      !response_info_truncation_.headers->HasStrongValidators()) {
    should_keep_entry_ = false;
    return false;
  }

  // Double check that there is something worth keeping.
  int current_size = entry_->disk_entry->GetDataSize(kResponseContentIndex);
  if (!current_size) {
    should_keep_entry_ = false;
    return false;
  }

  int64_t content_length =
      response_info_truncation_.headers->GetContentLength();
  if (content_length >= 0 && content_length <= current_size)
    return false;

  return true;
}

LoadState HttpCache::Writers::GetLoadState() const {
  if (network_transaction_)
    return network_transaction_->GetLoadState();
  return LOAD_STATE_IDLE;
}

HttpCache::Writers::WaitingForRead::WaitingForRead(
    scoped_refptr<IOBuffer> buf,
    int len,
    const CompletionCallback& consumer_callback)
    : read_buf(std::move(buf)),
      read_buf_len(len),
      write_len(0),
      callback(consumer_callback) {
  DCHECK(read_buf);
  DCHECK_GT(len, 0);
  DCHECK(!consumer_callback.is_null());
}

HttpCache::Writers::WaitingForRead::~WaitingForRead() {}
HttpCache::Writers::WaitingForRead::WaitingForRead(const WaitingForRead&) =
    default;

int HttpCache::Writers::DoLoop(int result) {
  DCHECK_NE(State::UNSET, next_state_);
  DCHECK_NE(State::NONE, next_state_);
  DCHECK(!in_do_loop_);

  int rv = result;
  do {
    State state = next_state_;
    next_state_ = State::UNSET;
    base::AutoReset<bool> scoped_in_do_loop(&in_do_loop_, true);
    switch (state) {
      case State::NETWORK_READ:
        DCHECK_EQ(OK, rv);
        rv = DoNetworkRead();
        break;
      case State::NETWORK_READ_COMPLETE:
        rv = DoNetworkReadComplete(rv);
        break;
      case State::CACHE_WRITE_DATA:
        rv = DoCacheWriteData(rv);
        break;
      case State::CACHE_WRITE_DATA_COMPLETE:
        rv = DoCacheWriteDataComplete(rv);
        break;
      case State::ASYNC_OP_COMPLETE_PRE_TRUNCATE:
        rv = DoAsyncOpCompletePreTruncate(rv);
        break;
      case State::CACHE_WRITE_TRUNCATED_RESPONSE:
        rv = DoCacheWriteTruncatedResponse();
        break;
      case State::CACHE_WRITE_TRUNCATED_RESPONSE_COMPLETE:
        rv = DoCacheWriteTruncatedResponseComplete(rv);
        break;
      case State::UNSET:
        NOTREACHED() << "bad state";
        rv = ERR_FAILED;
        break;
      case State::NONE:
        // Do Nothing.
        break;
    }
  } while (next_state_ != State::NONE && rv != ERR_IO_PENDING);

  // Save the callback as this object may be destroyed when the cache callback
  // is run.
  CompletionCallback callback = callback_;

  if (next_state_ == State::NONE) {
    read_buf_ = NULL;
    callback_.Reset();
    DCHECK(!all_writers_.empty() || cache_callback_);
    if (cache_callback_)
      std::move(cache_callback_).Run();
    // |this| may have been destroyed in the cache_callback_.
  }

  if (rv != ERR_IO_PENDING && !callback.is_null()) {
    base::ResetAndReturn(&callback).Run(rv);
  }
  return rv;
}

int HttpCache::Writers::DoNetworkRead() {
  DCHECK(network_transaction_);
  next_state_ = State::NETWORK_READ_COMPLETE;
  CompletionCallback io_callback =
      base::Bind(&HttpCache::Writers::OnIOComplete, weak_factory_.GetWeakPtr());
  return network_transaction_->Read(read_buf_.get(), io_buf_len_, io_callback);
}

int HttpCache::Writers::DoNetworkReadComplete(int result) {
  if (result < 0) {
    next_state_ = State::NONE;
    OnNetworkReadFailure(result);
    return result;
  }

  next_state_ = State::CACHE_WRITE_DATA;
  return result;
}

void HttpCache::Writers::OnNetworkReadFailure(int result) {
  ProcessFailure(result);

  if (active_transaction_)
    EraseTransaction(active_transaction_, result);
  active_transaction_ = nullptr;

  post_truncate_result_ = result;
  if (!InitiateTruncateEntry()) {
    post_truncate_result_ = OK;
    SetCacheCallback(false, TransactionSet());
  }
  // |this| may have been deleted after truncation, so don't touch any
  // members.
}

int HttpCache::Writers::DoCacheWriteData(int num_bytes) {
  next_state_ = State::CACHE_WRITE_DATA_COMPLETE;
  write_len_ = num_bytes;
  if (!num_bytes || network_read_only_)
    return num_bytes;

  int current_size = entry_->disk_entry->GetDataSize(kResponseContentIndex);
  CompletionCallback io_callback =
      base::Bind(&HttpCache::Writers::OnIOComplete, weak_factory_.GetWeakPtr());

  int rv = 0;

  PartialData* partial = nullptr;
  // The active transaction must be alive if this is a partial request, as
  // partial requests are exclusive and hence will always be the active
  // transaction.
  // TODO(shivanisha): When partial requests support parallel writing, this
  // assumption will not be true.
  if (active_transaction_)
    partial = all_writers_.find(active_transaction_)->second.partial;

  if (!partial) {
    rv = entry_->disk_entry->WriteData(kResponseContentIndex, current_size,
                                       read_buf_.get(), num_bytes, io_callback,
                                       true);
  } else {
    rv = partial->CacheWrite(entry_->disk_entry, read_buf_.get(), num_bytes,
                             io_callback);
  }
  return rv;
}

int HttpCache::Writers::DoCacheWriteDataComplete(int result) {
  next_state_ = State::NONE;
  if (result != write_len_) {
    // Note that it is possible for cache write to fail if the size of the file
    // exceeds the per-file limit.
    OnCacheWriteFailure();

    // |active_transaction_| can continue reading from the network.
    result = write_len_;
  } else {
    OnDataReceived(result);
  }
  return result;
}

int HttpCache::Writers::DoAsyncOpCompletePreTruncate(int result) {
  DCHECK(all_writers_.empty() && !active_transaction_);

  if (ShouldTruncate()) {
    next_state_ = State::CACHE_WRITE_TRUNCATED_RESPONSE;
  } else {
    next_state_ = State::NONE;
    SetCacheCallback(false, TransactionSet());
  }

  return OK;
}

int HttpCache::Writers::DoCacheWriteTruncatedResponse() {
  next_state_ = State::CACHE_WRITE_TRUNCATED_RESPONSE_COMPLETE;
  scoped_refptr<PickledIOBuffer> data(new PickledIOBuffer());
  response_info_truncation_.Persist(data->pickle(),
                                    true /* skip_transient_headers*/, true);
  data->Done();
  io_buf_len_ = data->pickle()->size();
  CompletionCallback io_callback =
      base::Bind(&HttpCache::Writers::OnIOComplete, weak_factory_.GetWeakPtr());
  return entry_->disk_entry->WriteData(kResponseInfoIndex, 0, data.get(),
                                       io_buf_len_, io_callback, true);
}

int HttpCache::Writers::DoCacheWriteTruncatedResponseComplete(int result) {
  next_state_ = State::NONE;
  if (result != io_buf_len_) {
    DLOG(ERROR) << "failed to write response info to cache";
    should_keep_entry_ = false;
  }

  SetCacheCallback(false, TransactionSet());
  result = post_truncate_result_;
  post_truncate_result_ = OK;
  return result;
}

void HttpCache::Writers::OnDataReceived(int result) {
  // If active_transaction_ has been destroyed and there is no other
  // transaction, we should not be in this state but in
  // ASYNC_OP_COMPLETE_PRE_TRUNCATE.
  DCHECK(!all_writers_.empty());

  auto it = all_writers_.find(active_transaction_);
  bool is_partial =
      active_transaction_ != nullptr && it->second.partial != nullptr;

  // Partial transaction will process the result, return from here.
  // This is done because partial requests handling require an awareness of both
  // headers and body state machines as they might have to go to the headers
  // phase for the next range, so it cannot be completely handled here.
  if (is_partial) {
    active_transaction_ = nullptr;
    return;
  }

  if (result == 0) {
    // Check if the response is actually completed or if not, attempt to mark
    // the entry as truncated in OnNetworkReadFailure.
    int current_size = entry_->disk_entry->GetDataSize(kResponseContentIndex);
    DCHECK(network_transaction_);
    const HttpResponseInfo* response_info =
        network_transaction_->GetResponseInfo();
    int64_t content_length = response_info->headers->GetContentLength();
    if (content_length >= 0 && content_length > current_size) {
      OnNetworkReadFailure(result);
      return;
    }

    if (active_transaction_)
      EraseTransaction(active_transaction_, result);
    active_transaction_ = nullptr;
    ProcessWaitingForReadTransactions(write_len_);

    // Invoke entry processing.
    DCHECK(ContainsOnlyIdleWriters());
    TransactionSet make_readers;
    for (auto& writer : all_writers_)
      make_readers.insert(writer.first);
    all_writers_.clear();
    SetCacheCallback(true, make_readers);
    return;
  }

  // Notify waiting_for_read_. Tasks will be posted for all the
  // transactions.
  ProcessWaitingForReadTransactions(write_len_);

  active_transaction_ = nullptr;
}

void HttpCache::Writers::OnCacheWriteFailure() {
  DLOG(ERROR) << "failed to write response data to cache";

  ProcessFailure(ERR_CACHE_WRITE_FAILURE);

  // Now writers will only be reading from the network.
  network_read_only_ = true;

  active_transaction_ = nullptr;

  should_keep_entry_ = false;
  if (all_writers_.empty()) {
    SetCacheCallback(false, TransactionSet());
  } else {
    cache_->WritersDoomEntryRestartTransactions(entry_);
  }
}

void HttpCache::Writers::ProcessWaitingForReadTransactions(int result) {
  for (auto it = waiting_for_read_.begin(); it != waiting_for_read_.end();) {
    Transaction* transaction = it->first;
    int callback_result = result;

    if (result >= 0) {  // success
      // Save the data in the waiting transaction's read buffer.
      it->second.write_len = std::min(it->second.read_buf_len, result);
      memcpy(it->second.read_buf->data(), read_buf_->data(),
             it->second.write_len);
      callback_result = it->second.write_len;
    }

    // Post task to notify transaction.
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::Bind(it->second.callback, callback_result));

    it = waiting_for_read_.erase(it);

    // If its response completion or failure, this transaction needs to be
    // removed from writers.
    if (result <= 0)
      EraseTransaction(transaction, result);
  }
}

void HttpCache::Writers::SetIdleWritersFailState(int result) {
  // Since this is only for idle transactions, waiting_for_read_
  // should be empty.
  DCHECK(waiting_for_read_.empty());
  for (auto it = all_writers_.begin(); it != all_writers_.end();) {
    Transaction* transaction = it->first;
    if (transaction == active_transaction_) {
      it++;
      continue;
    }
    it = EraseTransaction(it, result);
  }
}

void HttpCache::Writers::SetCacheCallback(bool success,
                                          const TransactionSet& make_readers) {
  DCHECK(!cache_callback_);
  cache_callback_ = base::BindOnce(&HttpCache::WritersDoneWritingToEntry,
                                   cache_->GetWeakPtr(), entry_, success,
                                   should_keep_entry_, make_readers);
}

void HttpCache::Writers::OnIOComplete(int result) {
  DoLoop(result);
}

}  // namespace net
