// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/devtools/protocol/storage_handler.h"

#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/strings/string_split.h"
#include "content/browser/cache_storage/cache_storage_context_impl.h"
#include "content/browser/indexed_db/indexed_db_context_impl.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "storage/browser/quota/quota_client.h"
#include "storage/browser/quota/quota_manager.h"
#include "storage/common/quota/quota_status_code.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace content {
namespace protocol {

namespace {
Storage::StorageType GetTypeName(storage::QuotaClient::ID id) {
  switch (id) {
    case storage::QuotaClient::kFileSystem:
      return Storage::StorageTypeEnum::File_systems;
    case storage::QuotaClient::kDatabase:
      return Storage::StorageTypeEnum::Websql;
    case storage::QuotaClient::kAppcache:
      return Storage::StorageTypeEnum::Appcache;
    case storage::QuotaClient::kIndexedDatabase:
      return Storage::StorageTypeEnum::Indexeddb;
    case storage::QuotaClient::kServiceWorkerCache:
      return Storage::StorageTypeEnum::Cache_storage;
    case storage::QuotaClient::kServiceWorker:
      return Storage::StorageTypeEnum::Service_workers;
    default:
      return Storage::StorageTypeEnum::Other;
  }
}

void ReportUsageAndQuotaDataOnUIThread(
    std::unique_ptr<StorageHandler::GetUsageAndQuotaCallback> callback,
    storage::QuotaStatusCode code,
    int64_t usage,
    int64_t quota,
    base::flat_map<storage::QuotaClient::ID, int64_t> usage_breakdown) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (code != storage::kQuotaStatusOk) {
    return callback->sendFailure(
        Response::Error("Quota information is not available"));
  }

  std::unique_ptr<Array<Storage::UsageForType>> usageList =
      Array<Storage::UsageForType>::create();
  for (const auto& usage : usage_breakdown) {
    std::unique_ptr<Storage::UsageForType> entry =
        Storage::UsageForType::Create()
            .SetStorageType(GetTypeName(usage.first))
            .SetUsage(usage.second)
            .Build();
    usageList->addItem(std::move(entry));
  }
  callback->sendSuccess(usage, quota, std::move(usageList));
}

void GotUsageAndQuotaDataCallback(
    std::unique_ptr<StorageHandler::GetUsageAndQuotaCallback> callback,
    storage::QuotaStatusCode code,
    int64_t usage,
    int64_t quota,
    base::flat_map<storage::QuotaClient::ID, int64_t> usage_breakdown) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  BrowserThread::PostTask(
      BrowserThread::UI, FROM_HERE,
      base::BindOnce(ReportUsageAndQuotaDataOnUIThread,
                     base::Passed(std::move(callback)), code, usage, quota,
                     std::move(usage_breakdown)));
}

void GetUsageAndQuotaOnIOThread(
    storage::QuotaManager* manager,
    const GURL& url,
    std::unique_ptr<StorageHandler::GetUsageAndQuotaCallback> callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  manager->GetUsageAndQuotaWithBreakdown(
      url, storage::kStorageTypeTemporary,
      base::Bind(&GotUsageAndQuotaDataCallback,
                 base::Passed(std::move(callback))));
}
}  // namespace

// Observer that listens on the IO thread for cache storage notifications and
// informs the StorageHandler on the UI thread for origins of interest.
// Created on the UI thread but predominantly used and deleted on the IO thread.
// Registered on creation as an observer in CacheStorageContextImpl,
// unregistered on destruction.
class StorageHandler::CacheStorageObserver : CacheStorageContextImpl::Observer {
 public:
  CacheStorageObserver(base::WeakPtr<StorageHandler> owner_storage_handler,
                       CacheStorageContextImpl* cache_storage_context)
      : owner_(owner_storage_handler), context_(cache_storage_context) {
    BrowserThread::PostTask(
        BrowserThread::IO, FROM_HERE,
        base::BindOnce(&CacheStorageObserver::AddObserverOnIOThread,
                       base::Unretained(this)));
  }

  ~CacheStorageObserver() override {
    DCHECK_CURRENTLY_ON(BrowserThread::IO);
    context_->RemoveObserver(this);
  }

  void TrackOriginOnIOThread(const url::Origin& origin) {
    DCHECK_CURRENTLY_ON(BrowserThread::IO);
    if (origins_.find(origin) != origins_.end())
      return;
    origins_.insert(origin);
  }

  void UntrackOriginOnIOThread(const url::Origin& origin) {
    DCHECK_CURRENTLY_ON(BrowserThread::IO);
    origins_.erase(origin);
  }

  void OnCacheListChanged(const url::Origin& origin) override {
    auto found = origins_.find(origin);
    if (found == origins_.end())
      return;
    BrowserThread::PostTask(
        BrowserThread::UI, FROM_HERE,
        base::BindOnce(&StorageHandler::NotifyCacheStorageListChanged, owner_,
                       origin.Serialize()));
  }

  void OnCacheContentChanged(const url::Origin& origin,
                             const std::string& cache_name) override {
    if (origins_.find(origin) == origins_.end())
      return;
    BrowserThread::PostTask(
        BrowserThread::UI, FROM_HERE,
        base::BindOnce(&StorageHandler::NotifyCacheStorageContentChanged,
                       owner_, origin.Serialize(), cache_name));
  }

 private:
  void AddObserverOnIOThread() {
    DCHECK_CURRENTLY_ON(BrowserThread::IO);
    context_->AddObserver(this);
  }

  // Maintained on the IO thread to avoid thread contention.
  base::flat_set<url::Origin> origins_;

  base::WeakPtr<StorageHandler> owner_;
  scoped_refptr<CacheStorageContextImpl> context_;

  DISALLOW_COPY_AND_ASSIGN(CacheStorageObserver);
};

// Observer that listens on the IDB thread for IndexedDB notifications and
// informs the StorageHandler on the UI thread for origins of interest.
// Created on the UI thread but predominantly used and deleted on the IDB
// thread.
// Registered on creation as an observer in IndexedDBContextImpl, unregistered
// on destruction.
class StorageHandler::IndexedDBObserver : IndexedDBContextImpl::Observer {
 public:
  IndexedDBObserver(base::WeakPtr<StorageHandler> owner_storage_handler,
                    IndexedDBContextImpl* indexed_db_context)
      : owner_(owner_storage_handler), context_(indexed_db_context) {
    context_->TaskRunner()->PostTask(
        FROM_HERE, base::BindOnce(&IndexedDBObserver::AddObserverOnIDBThread,
                                  base::Unretained(this)));
  }

  ~IndexedDBObserver() override {
    DCHECK(context_->TaskRunner()->RunsTasksInCurrentSequence());
    context_->RemoveObserver(this);
  }

  void TrackOriginOnIDBThread(const url::Origin& origin) {
    DCHECK(context_->TaskRunner()->RunsTasksInCurrentSequence());
    if (origins_.find(origin) != origins_.end())
      return;
    origins_.insert(origin);
  }

  void UntrackOriginOnIDBThread(const url::Origin& origin) {
    DCHECK(context_->TaskRunner()->RunsTasksInCurrentSequence());
    origins_.erase(origin);
  }

  void OnIndexedDBListChanged(const url::Origin& origin) override {
    auto found = origins_.find(origin);
    if (found == origins_.end())
      return;
    BrowserThread::PostTask(
        BrowserThread::UI, FROM_HERE,
        base::BindOnce(&StorageHandler::NotifyIndexedDBListChanged, owner_,
                       origin.Serialize()));
  }

  void OnIndexedDBContentChanged(
      const url::Origin& origin,
      const base::string16& database_name,
      const base::string16& object_store_name) override {
    auto found = origins_.find(origin);
    if (found == origins_.end())
      return;
    BrowserThread::PostTask(
        BrowserThread::UI, FROM_HERE,
        base::BindOnce(&StorageHandler::NotifyIndexedDBContentChanged, owner_,
                       origin.Serialize(), database_name, object_store_name));
  }

  base::SequencedTaskRunner* TaskRunner() const {
    return context_->TaskRunner();
  }

 private:
  void AddObserverOnIDBThread() {
    DCHECK(context_->TaskRunner()->RunsTasksInCurrentSequence());
    context_->AddObserver(this);
  }

  // Maintained on the IDB thread to avoid thread contention.
  base::flat_set<url::Origin> origins_;

  base::WeakPtr<StorageHandler> owner_;
  scoped_refptr<IndexedDBContextImpl> context_;

  DISALLOW_COPY_AND_ASSIGN(IndexedDBObserver);
};

StorageHandler::StorageHandler()
    : DevToolsDomainHandler(Storage::Metainfo::domainName),
      process_(nullptr),
      weak_ptr_factory_(this) {}

StorageHandler::~StorageHandler() {
  DCHECK(!cache_storage_observer_);
  DCHECK(!indexed_db_observer_);
}

void StorageHandler::Wire(UberDispatcher* dispatcher) {
  frontend_ = std::make_unique<Storage::Frontend>(dispatcher->channel());
  Storage::Dispatcher::wire(dispatcher, this);
}

void StorageHandler::SetRenderer(RenderProcessHost* process_host,
                                 RenderFrameHostImpl* frame_host) {
  process_ = process_host;
}

Response StorageHandler::Disable() {
  if (cache_storage_observer_) {
    BrowserThread::DeleteSoon(BrowserThread::IO, FROM_HERE,
                              cache_storage_observer_.release());
  }
  if (indexed_db_observer_) {
    indexed_db_observer_->TaskRunner()->DeleteSoon(
        FROM_HERE, std::move(indexed_db_observer_));
  }

  return Response::OK();
}

Response StorageHandler::ClearDataForOrigin(
    const std::string& origin,
    const std::string& storage_types) {
  if (!process_)
    return Response::InternalError();

  StoragePartition* partition = process_->GetStoragePartition();
  std::vector<std::string> types = base::SplitString(
      storage_types, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  std::unordered_set<std::string> set(types.begin(), types.end());
  uint32_t remove_mask = 0;
  if (set.count(Storage::StorageTypeEnum::Appcache))
    remove_mask |= StoragePartition::REMOVE_DATA_MASK_APPCACHE;
  if (set.count(Storage::StorageTypeEnum::Cookies))
    remove_mask |= StoragePartition::REMOVE_DATA_MASK_COOKIES;
  if (set.count(Storage::StorageTypeEnum::File_systems))
    remove_mask |= StoragePartition::REMOVE_DATA_MASK_FILE_SYSTEMS;
  if (set.count(Storage::StorageTypeEnum::Indexeddb))
    remove_mask |= StoragePartition::REMOVE_DATA_MASK_INDEXEDDB;
  if (set.count(Storage::StorageTypeEnum::Local_storage))
    remove_mask |= StoragePartition::REMOVE_DATA_MASK_LOCAL_STORAGE;
  if (set.count(Storage::StorageTypeEnum::Shader_cache))
    remove_mask |= StoragePartition::REMOVE_DATA_MASK_SHADER_CACHE;
  if (set.count(Storage::StorageTypeEnum::Websql))
    remove_mask |= StoragePartition::REMOVE_DATA_MASK_WEBSQL;
  if (set.count(Storage::StorageTypeEnum::Service_workers))
    remove_mask |= StoragePartition::REMOVE_DATA_MASK_SERVICE_WORKERS;
  if (set.count(Storage::StorageTypeEnum::Cache_storage))
    remove_mask |= StoragePartition::REMOVE_DATA_MASK_CACHE_STORAGE;
  if (set.count(Storage::StorageTypeEnum::All))
    remove_mask |= StoragePartition::REMOVE_DATA_MASK_ALL;

  if (!remove_mask)
    return Response::InvalidParams("No valid storage type specified");

  partition->ClearDataForOrigin(
      remove_mask,
      StoragePartition::QUOTA_MANAGED_STORAGE_MASK_ALL,
      GURL(origin),
      partition->GetURLRequestContext(),
      base::Bind(&base::DoNothing));
  return Response::OK();
}

void StorageHandler::GetUsageAndQuota(
    const String& origin,
    std::unique_ptr<GetUsageAndQuotaCallback> callback) {
  if (!process_)
    return callback->sendFailure(Response::InternalError());

  GURL origin_url(origin);
  if (!origin_url.is_valid()) {
    return callback->sendFailure(
        Response::Error(origin + " is not a valid URL"));
  }

  storage::QuotaManager* manager =
      process_->GetStoragePartition()->GetQuotaManager();
  BrowserThread::PostTask(
      BrowserThread::IO, FROM_HERE,
      base::BindOnce(&GetUsageAndQuotaOnIOThread, base::RetainedRef(manager),
                     origin_url, base::Passed(std::move(callback))));
}

Response StorageHandler::TrackCacheStorageForOrigin(const std::string& origin) {
  if (!process_)
    return Response::InternalError();

  GURL origin_url(origin);
  if (!origin_url.is_valid())
    return Response::InvalidParams(origin + " is not a valid URL");

  BrowserThread::PostTask(
      BrowserThread::IO, FROM_HERE,
      base::BindOnce(&CacheStorageObserver::TrackOriginOnIOThread,
                     base::Unretained(GetCacheStorageObserver()),
                     url::Origin::Create(origin_url)));
  return Response::OK();
}

Response StorageHandler::UntrackCacheStorageForOrigin(
    const std::string& origin) {
  if (!process_)
    return Response::InternalError();

  GURL origin_url(origin);
  if (!origin_url.is_valid())
    return Response::InvalidParams(origin + " is not a valid URL");

  BrowserThread::PostTask(
      BrowserThread::IO, FROM_HERE,
      base::BindOnce(&CacheStorageObserver::UntrackOriginOnIOThread,
                     base::Unretained(GetCacheStorageObserver()),
                     url::Origin::Create(origin_url)));
  return Response::OK();
}

Response StorageHandler::TrackIndexedDBForOrigin(const std::string& origin) {
  if (!process_)
    return Response::InternalError();

  GURL origin_url(origin);
  if (!origin_url.is_valid())
    return Response::InvalidParams(origin + " is not a valid URL");

  GetIndexedDBObserver()->TaskRunner()->PostTask(
      FROM_HERE, base::BindOnce(&IndexedDBObserver::TrackOriginOnIDBThread,
                                base::Unretained(GetIndexedDBObserver()),
                                url::Origin::Create(origin_url)));
  return Response::OK();
}

Response StorageHandler::UntrackIndexedDBForOrigin(const std::string& origin) {
  if (!process_)
    return Response::InternalError();

  GURL origin_url(origin);
  if (!origin_url.is_valid())
    return Response::InvalidParams(origin + " is not a valid URL");

  GetIndexedDBObserver()->TaskRunner()->PostTask(
      FROM_HERE, base::BindOnce(&IndexedDBObserver::UntrackOriginOnIDBThread,
                                base::Unretained(GetIndexedDBObserver()),
                                url::Origin::Create(origin_url)));
  return Response::OK();
}

StorageHandler::CacheStorageObserver*
StorageHandler::GetCacheStorageObserver() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (!cache_storage_observer_) {
    cache_storage_observer_ = std::make_unique<CacheStorageObserver>(
        weak_ptr_factory_.GetWeakPtr(),
        static_cast<CacheStorageContextImpl*>(
            process_->GetStoragePartition()->GetCacheStorageContext()));
  }
  return cache_storage_observer_.get();
}

StorageHandler::IndexedDBObserver* StorageHandler::GetIndexedDBObserver() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (!indexed_db_observer_) {
    indexed_db_observer_ = std::make_unique<IndexedDBObserver>(
        weak_ptr_factory_.GetWeakPtr(),
        static_cast<IndexedDBContextImpl*>(
            process_->GetStoragePartition()->GetIndexedDBContext()));
  }
  return indexed_db_observer_.get();
}

void StorageHandler::NotifyCacheStorageListChanged(const std::string& origin) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  frontend_->CacheStorageListUpdated(origin);
}

void StorageHandler::NotifyCacheStorageContentChanged(const std::string& origin,
                                                      const std::string& name) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  frontend_->CacheStorageContentUpdated(origin, name);
}

void StorageHandler::NotifyIndexedDBListChanged(const std::string& origin) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  frontend_->IndexedDBListUpdated(origin);
}

void StorageHandler::NotifyIndexedDBContentChanged(
    const std::string& origin,
    const base::string16& database_name,
    const base::string16& object_store_name) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  frontend_->IndexedDBContentUpdated(origin, base::UTF16ToUTF8(database_name),
                                     base::UTF16ToUTF8(object_store_name));
}

}  // namespace protocol
}  // namespace content
