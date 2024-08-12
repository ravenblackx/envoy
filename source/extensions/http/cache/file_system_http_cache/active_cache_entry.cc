#include "source/extensions/http/cache/file_system_http_cache/active_cache_entry.h"

#include "source/extensions/http/cache/file_system_http_cache/insert_context.h"
#include "source/extensions/http/cache/file_system_http_cache/lookup_context.h"
#include <fcntl.h>

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {
namespace FileSystemHttpCache {

LookupContextPtr ActiveCacheEntry::makeLookupContext(FileSystemHttpCache& cache,
                                                     LookupRequest&& lookup) {
  auto context = std::make_unique<FileLookupContext>(cache, shared_from_this(), std::move(lookup));
  initializeContext(context.get());
  return context;
}

void ActiveCacheEntry::sendHeadersTo(FileLookupContext* context) {
  mu_.AssertHeld();
  ASSERT(context->headers_cb_ != nullptr);
  ASSERT(lookup_while_writing_ != nullptr);
  auto lookup_result = context->lookup().makeLookupResult(
      Http::createHeaderMap<Http::ResponseHeaderMapImpl>(*lookup_while_writing_->response_headers_),
      ResponseMetadata{lookup_while_writing_->response_metadata_},
      lookup_while_writing_->content_length_);
  std::move(context->headers_cb_)(std::move(lookup_result),
                                  lookup_while_writing_->header_end_stream_);
  context->headers_cb_ = nullptr;
}

void ActiveCacheEntry::wantHeaders(FileLookupContext* context) {
  absl::MutexLock lock(&mu_);
  if (lookup_while_writing_ != nullptr) {
    sendHeadersTo(context);
  } else {
    header_subscribers_.insert(context);
  }
}

void ActiveCacheEntry::deliverFileHandle() {
  mu_.AssertHeld();
  ASSERT(shared_file_handle_ != nullptr);
  ASSERT(!file_handle_waiters_.empty());
  auto cancel_func = shared_file_handle_->duplicate(
      [p = shared_from_this()](absl::StatusOr<AsyncFileHandle> result) {
        absl::MutexLock lock(&p->mu_);
        if (!result.ok()) {
          for (const auto context : p->file_handle_waiters_) {
            ENVOY_LOG(warn, "file_system_cache: failed to duplicate file: {}", result.status());
            context->postCacheAbort();
          }
          p->file_handle_waiters_.clear();
          p->file_handle_cancel_ = nullptr;
          return;
        }
        if (p->file_handle_waiters_.empty()) {
          absl::Status enqueued = result.value()->close([](absl::Status) {});
          ASSERT(enqueued.ok());
          return;
        }
        FileLookupContext* context = p->file_handle_waiters_.front();
        p->file_handle_waiters_.pop_front();
        context->postFileHandle(result.value());
        if (p->file_handle_waiters_.empty()) {
          p->file_handle_cancel_ = nullptr;
        } else {
          p->deliverFileHandle();
        }
      });
  ASSERT(cancel_func.ok());
  file_handle_cancel_ = cancel_func.value();
}

void ActiveCacheEntry::unsubscribe(FileLookupContext* context) {
  absl::MutexLock lock(&mu_);
  header_subscribers_.erase(context);
  body_subscribers_.erase(context);
  trailer_subscribers_.erase(context);
  auto it = std::find(file_handle_waiters_.begin(), file_handle_waiters_.end(), context);
  if (it != file_handle_waiters_.end()) {
    file_handle_waiters_.erase(it);
  }
}

// TODO: first make upstream access out of band, so that it can avoid getting mixed up
// in the filter.
FileLookupContext::State ActiveCacheEntry::initializeContext(FileLookupContext* context) {
  absl::MutexLock lock(&mu_);
  switch (state_) {
  case State::Written:
    return FileLookupContext::State::CheckingFile;
  case State::NotCacheable:
    return FileLookupContext::State::NotCacheable;
  case State::New:
    state_ = State::Pending;
    return FileLookupContext::State::CheckCacheExistence;
  case State::Pending:
  case State::Writing:
    file_handle_waiters_.push_back(context);
    if (shared_file_handle_ != nullptr && file_handle_cancel_ == nullptr) {
      deliverFileHandle();
    }
    return FileLookupContext::State::StreamListening;
  }
}

bool ActiveCacheEntry::wantBodyRange(FileLookupContext* context, AdjustedByteRange range) {
  absl::MutexLock lock(&mu_);
  if (rangeAvailable(range)) {
    return true;
  }
  body_subscribers_.insert(context);
  return false;
}

void ActiveCacheEntry::headersReady(FileInsertContext* context,
                                    const Http::ResponseHeaderMap&& response_headers,
                                    ResponseMetadata&& response_metadata, bool end_stream) {
  absl::MutexLock lock(&mu_);
  insert_context_ = context;
  uint64_t content_length_header = 0;
  std::optional<uint64_t> content_length;
  if (absl::SimpleAtoi(response_headers.getContentLengthValue(), &content_length_header) ||
      end_stream) {
    content_length = content_length_header;
  }
  lookup_while_writing_ = std::make_unique<LookupWhileWritingContext>(LookupWhileWritingContext{
      Http::createHeaderMap<Http::ResponseHeaderMapImpl>(std::move(response_headers)),
      std::move(response_metadata), content_length, end_stream});
  for (FileLookupContext* c : header_subscribers_) {
    sendHeadersTo(c);
  }
  header_subscribers_.clear();
}

InsertContextPtr ActiveCacheEntry::makeInsertContext(LookupContextPtr&& lookup_context) {
  // Downcast to our lookup base class - no need for dynamic_cast because the only LookupContexts
  // that can possibly come here are FileLookupContext.
  std::unique_ptr<FileLookupContext> context{
      static_cast<FileLookupContext*>(lookup_context.release())};
  absl::MutexLock lock(&mu_);
  state_ = State::Writing;
  return std::make_unique<FileInsertContext>(shared_from_this(), std::move(context));
}

ActiveCacheEntry::~ActiveCacheEntry() {
  absl::MutexLock lock(&mu_);
  ASSERT(insert_context_ == nullptr);
  ASSERT(header_subscribers_.empty());
  ASSERT(body_subscribers_.empty());
  ASSERT(trailer_subscribers_.empty());
}

std::shared_ptr<ActiveCacheEntry> ActiveCacheEntries::getEntry(const Key& key) {
  const SystemTime now = time_source_.systemTime();
  absl::MutexLock lock(&mu_);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    it = entries_.emplace(key).first;
  }
  auto ret = it->second;
  ret->setExpiry(now + expiry_duration_);
  // As a lazy way of keeping the cache metadata from growing endlessly,
  // remove at most one adjacent metadata entry every time an entry is touched.
  // This should do a decent job of expiring them simply, with a low cost, and
  // without taking any long-lived locks as would be required for periodic
  // scanning.
  if (++it == entries_.end()) {
    it = entries_.begin();
  }
  if (it->second->isExpiredAt(now)) {
    entries_.erase(it);
  }
  return ret;
}

} // namespace FileSystemHttpCache
} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
