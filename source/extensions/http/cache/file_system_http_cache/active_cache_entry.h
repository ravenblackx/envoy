#pragma once

#include <memory>
#include <queue>
#include "absl/container/flat_hash_map.h"
#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"
#include "source/common/protobuf/utility.h"
#include "source/extensions/filters/http/cache/http_cache.h"
#include "source/extensions/filters/http/cache/key.pb.h"
#include "source/extensions/http/cache/file_system_http_cache/insert_context.h"
#include "source/extensions/http/cache/file_system_http_cache/lookup_context.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {
namespace FileSystemHttpCache {

class FileInsertContext;
class FileLookupContext;
class FileSystemHttpCache;

class ActiveCacheEntry : public Logger::Loggable<Logger::Id::cache_filter>,
                         std::enable_shared_from_this<ActiveCacheEntry> {
public:
  ActiveCacheEntry();

  // Returns a FileLookupContext linked to this ActiveCacheEntry, with a state appropriate
  // to the ActiveCacheEntry state.
  // May change the state of the ActiveCacheEntry from New to Pending.
  LookupContextPtr makeLookupContext(FileSystemHttpCache& cache, LookupRequest&& lookup);
  // Returns a FileInsertContext linked to this ActiveCacheEntry, so it can trigger appropriate
  // stream notifications to any bound FileLookupContexts in StreamListening state.
  InsertContextPtr makeInsertContext(LookupContextPtr&& lookup_context);

  enum class State {
    // New state means this is the first client of the cache entry - it should immediately
    // update the state to Pending and attempt a lookup (then if necessary insertion).
    New,
    // Pending state means another client is already doing lookup/insertion/verification.
    // Client should subscribe to this, and act on received messages.
    Pending,
    // Writing state means another client is doing insertion. Client should subscribe to
    // this, and act on received messages.
    Writing,
    // Written state means a cache file probably exists. Client should attempt to read from
    // the file. On failure, client should mutate state to Pending and attempt insertion, or,
    // if state has already changed from Written, do whatever action is appropriate for the
    // new state.
    Written,
    // NotCacheable state means this key is considered non-cachable. Client should pass through.
    // If the passed-through response turns out to be cachable (i.e. upstream has changed
    // cache headers), client should update state to Writing, or, if state is already changed,
    // client should abort the new upstream request and use the shared one.
    NotCacheable
  };
  // Switches state to Written, removes the insert_context_, notifies all
  // subscribers.
  void insertComplete() ABSL_LOCKS_EXCLUDED(mu_);
  // Switches state to New, removes the insert_context_, aborts all subscribers.
  // Ideally this shouldn't happen, as you should be using a reliable upstream.
  void insertAbort() ABSL_LOCKS_EXCLUDED(mu_);
  // Removes the given context from subscribers.
  void unsubscribe(FileLookupContext* p) ABSL_LOCKS_EXCLUDED(mu_);
  // Populates the headers in memory while write is in progress, and
  // calls the callback of all header-subscribers.
  void headersReady(FileInsertContext* context, const Http::ResponseHeaderMap&& response_headers,
                    ResponseMetadata&& response_metadata, bool end_stream) ABSL_LOCKS_EXCLUDED(mu_);
  // Notifies subscribers waiting on body position < sz to read some body from
  // the shared file. They are all removed from subscribers, and should all
  // trigger the appropriate file-read action.
  void bodyReadyTo(size_t sz) ABSL_LOCKS_EXCLUDED(mu_);
  // Notifies subscribers waiting on trailers that trailers are ready. They are
  // all removed from subscribers, and should all immediately call the trailers
  // callback.
  void trailersReady() ABSL_LOCKS_EXCLUDED(mu_);

  // When a lookup context is first used, it calls initializeContext to find
  // out what its initial state should be. If that state is StreamListening then
  // the context must wait to receive a file handle before requesting headers.
  FileLookupContext::State initializeContext(FileLookupContext* context) ABSL_LOCKS_EXCLUDED(mu_);
  // Called by a subscriber that's ready to read headers. It will either
  // get a call-back with headers, or a call-back telling it to
  // change state to ReadingFile, or a call-back telling it to abort
  // if the write to file is aborted.
  void wantHeaders(FileLookupContext* context) ABSL_LOCKS_EXCLUDED(mu_);
  // Called by a subscriber that's ready to read a body range. It will
  // get a call-back telling it to read the file when the file is complete
  // or contains the required range, or a call-back telling it to abort if
  // the write to file is aborted.
  void wantBodyRange(FileLookupContext* context, AdjustedByteRange range) ABSL_LOCKS_EXCLUDED(mu_);
  // Called by a subscriber that's ready to read trailers. It will get a
  // call-back when the file is complete or a call-back telling it to abort
  // if the write to file is aborted.
  void wantTrailers(FileLookupContext* context) ABSL_LOCKS_EXCLUDED(mu_);

private:
  void deliverFileHandle() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  void sendHeadersTo(FileLookupContext* context) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  absl::Mutex mu_;
  State state_ ABSL_GUARDED_BY(mu_) = State::New;
  struct LookupWhileWritingContext {
    Http::ResponseHeaderMapPtr response_headers_;
    ResponseMetadata response_metadata_;
    absl::optional<uint64_t> content_length_;
    bool header_end_stream_;
  };
  std::unique_ptr<LookupWhileWritingContext> lookup_while_writing_ ABSL_GUARDED_BY(mu_);

  // FileInsertContext must call insertComplete or insertAbort before/during
  // destruction, to ensure there is no dangling pointer here.
  FileInsertContext* insert_context_ ABSL_GUARDED_BY(mu_);
  // FileLookupContext must call unsubscribe before/during destruction if it
  // is waiting for any response from the ActiveCacheEntry, to ensure there is
  // no dangling pointer in any of the below.
  // This is the set of contexts actively waiting for new content
  // to be written to the cache. All subscribers will receive a
  // postStateChanged when headers are populated, when their required chunk of
  // body has been written, or when the whole state changes (e.g. completion/abort).
  absl::flat_hash_set<FileLookupContext*> header_subscribers_ ABSL_GUARDED_BY(mu_);
  absl::flat_hash_map<FileLookupContext*, AdjustedByteRange> body_subscribers_ ABSL_GUARDED_BY(mu_);
  absl::flat_hash_set<FileLookupContext*> trailer_subscribers_ ABSL_GUARDED_BY(mu_);
  // This is the set of contexts waiting to be initialized with a file
  // handle. FIFO.
  std::deque<FileLookupContext*> file_handle_waiters_ ABSL_GUARDED_BY(mu_);
  AsyncFileHandle shared_file_handle_ ABSL_GUARDED_BY(mu_);
  Common::AsyncFiles::CancelFunction file_handle_cancel_ ABSL_GUARDED_BY(mu_);

  // The following fields and functions are only used by ActiveCacheEntries.
  friend class ActiveCacheEntries;
  void setExpiry(SystemTime expiry) { expires_at_ = expiry; }
  bool isExpiredAt(SystemTime t) const { return expires_at_ < t; }

  SystemTime expires_at_; // This is guarded by ActiveCacheEntries' mutex.
  ~ActiveCacheEntry();
};

class ActiveCacheEntries {
public:
  ActiveCacheEntries(TimeSource& time_source) : time_source_(time_source) {}

  // Returns an entry with the given key, creating it if necessary.
  std::shared_ptr<ActiveCacheEntry> getEntry(const Key& key) ABSL_LOCKS_EXCLUDED(mu_);

private:
  TimeSource& time_source_;
  std::chrono::duration<int> expiry_duration_ = std::chrono::minutes(5);
  absl::Mutex mu_;
  absl::flat_hash_map<Key, std::shared_ptr<ActiveCacheEntry>, MessageUtil, MessageUtil>
      entries_ ABSL_GUARDED_BY(mu_);
};

} // namespace FileSystemHttpCache
} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
