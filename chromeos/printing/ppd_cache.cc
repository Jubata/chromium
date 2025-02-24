// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/printing/ppd_cache.h"

#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/files/file_util.h"
#include "base/json/json_parser.h"
#include "base/json/json_writer.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/task_runner_util.h"
#include "base/task_scheduler/post_task.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chromeos/printing/printing_constants.h"
#include "crypto/sha2.h"
#include "net/base/io_buffer.h"
#include "net/filter/gzip_header.h"

namespace chromeos {
namespace {

// Return the (full) path to the file we expect to find the given key at.
base::FilePath FilePathForKey(const base::FilePath& base_dir,
                              const std::string& key) {
  std::string hashed_key = crypto::SHA256HashString(key);
  return base_dir.Append(base::HexEncode(hashed_key.data(), hashed_key.size()));
}

// If the cache doesn't already exist, create it.
void MaybeCreateCache(const base::FilePath& base_dir) {
  if (!base::PathExists(base_dir)) {
    base::CreateDirectory(base_dir);
  }
}

// Find implementation, blocks on file access.  Must be run on a thread that
// allows I/O.
PpdCache::FindResult FindImpl(const base::FilePath& cache_dir,
                              const std::string& key) {
  base::AssertBlockingAllowed();

  PpdCache::FindResult result;
  result.success = false;
  if (!base::PathExists(cache_dir)) {
    // If the cache dir was missing, we'll miss anyway.
    return result;
  }

  base::File file(FilePathForKey(cache_dir, key),
                  base::File::FLAG_OPEN | base::File::FLAG_READ);

  if (file.IsValid()) {
    int64_t len = file.GetLength();
    if (len >= static_cast<int64_t>(crypto::kSHA256Length) &&
        len <= static_cast<int64_t>(kMaxPpdSizeBytes) +
                   static_cast<int64_t>(crypto::kSHA256Length)) {
      std::unique_ptr<char[]> buf(new char[len]);
      if (file.ReadAtCurrentPos(buf.get(), len) == len) {
        base::StringPiece contents(buf.get(), len - crypto::kSHA256Length);
        base::StringPiece checksum(buf.get() + len - crypto::kSHA256Length,
                                   crypto::kSHA256Length);
        if (crypto::SHA256HashString(contents) == checksum) {
          base::File::Info info;
          if (file.GetInfo(&info)) {
            result.success = true;
            result.age = base::Time::Now() - info.last_modified;
            contents.CopyToString(&result.contents);
          }
        } else {
          LOG(ERROR) << "Bad checksum for cache key " << key;
        }
      }
    }
  }

  return result;
}

// Store implementation, blocks on file access.  Must be run on a thread that
// allows I/O.
void StoreImpl(const base::FilePath& cache_dir,
               const std::string& key,
               const std::string& contents) {
  base::AssertBlockingAllowed();

  MaybeCreateCache(cache_dir);
  if (contents.size() > kMaxPpdSizeBytes) {
    LOG(ERROR) << "Ignoring attempt to cache large object";
  } else {
    auto path = FilePathForKey(cache_dir, key);
    base::File file(path,
                    base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_WRITE);
    std::string checksum = crypto::SHA256HashString(contents);
    if (!file.IsValid() ||
        file.WriteAtCurrentPos(contents.data(), contents.size()) !=
            static_cast<int>(contents.size()) ||
        file.WriteAtCurrentPos(checksum.data(), checksum.size()) !=
            static_cast<int>(checksum.size())) {
      LOG(ERROR) << "Failed to create ppd cache file";
      file.Close();
      if (!base::DeleteFile(path, false)) {
        LOG(ERROR) << "Failed to cleanup failed creation.";
      }
    }
  }
}

// Implementation of the PpdCache that uses two separate task runners for Store
// and Fetch since the two operations have different priorities. Note that the
// two operations are not sequenced so there should be no expectation that a
// call to Find will return a file that was previously Stored until the Store
// callback is run.
class PpdCacheImpl : public PpdCache {
 public:
  explicit PpdCacheImpl(const base::FilePath& cache_base_dir)
      : cache_base_dir_(cache_base_dir),
        fetch_task_runner_(base::CreateSequencedTaskRunnerWithTraits(
            {base::TaskPriority::USER_VISIBLE, base::MayBlock(),
             base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})),
        store_task_runner_(base::CreateSequencedTaskRunnerWithTraits(
            {base::TaskPriority::BACKGROUND, base::MayBlock(),
             base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {}

  // Public API functions.
  void Find(const std::string& key, FindCallback cb) override {
    base::PostTaskAndReplyWithResult(
        fetch_task_runner_.get(), FROM_HERE,
        base::BindOnce(&FindImpl, cache_base_dir_, key), std::move(cb));
  }

  // Store the given contents at the given key.  If cb is non-null, it will
  // be invoked on completion.
  void Store(const std::string& key,
             const std::string& contents,
             const base::Closure& cb) override {
    store_task_runner_->PostTaskAndReply(
        FROM_HERE, base::Bind(&StoreImpl, cache_base_dir_, key, contents), cb);
  }

 private:
  ~PpdCacheImpl() override {}

  base::FilePath cache_base_dir_;
  scoped_refptr<base::SequencedTaskRunner> fetch_task_runner_;
  scoped_refptr<base::SequencedTaskRunner> store_task_runner_;

  DISALLOW_COPY_AND_ASSIGN(PpdCacheImpl);
};

}  // namespace

// static
scoped_refptr<PpdCache> PpdCache::Create(const base::FilePath& cache_base_dir) {
  return scoped_refptr<PpdCache>(new PpdCacheImpl(cache_base_dir));
}

}  // namespace chromeos
