#pragma once

#include <memory>

#include "source/extensions/common/async_files/async_file_handle.h"
#include "source/extensions/filters/http/cache/http_cache.h"
#include "source/extensions/http/cache/file_system_http_cache/cache_file_fixed_block.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {
namespace FileSystemHttpCache {

class ActiveCacheEntry;
class FileSystemHttpCache;

using Envoy::Extensions::Common::AsyncFiles::AsyncFileHandle;
using Envoy::Extensions::Common::AsyncFiles::CancelFunction;

class FileLookupContext : public LookupContext {
public:
  enum class State {
    // NotCacheable state means the context should not check the cache, just request upstream.
    // It's possible the upstream will change state to cacheable, in which case the first to
    // see that should change the entry state to Writing and start inserting, and others
    // should change their own state to match the entry state (either SharingStream or
    // OpeningFile).
    NotCacheable,
    // CheckCacheExistence state means the context should try to open the file. If found and not
    // expired, it should change the entry state to Written and its own state to ReadingFile,
    // otherwise entry state to Pending and its own state to Missed.
    // Only one lookup for a given key should be in CheckCacheExistence state.
    // This is the only state that should allow an InsertContext to perform an insert.
    CheckCacheExistence,
    // StreamListening means this lookup is following events on the cache entry.
    StreamListening,
    // Missed means this lookup found there was no cache entry. The next action should be
    // the filter fetching from upstream and (if cacheable) performing an insert. There
    // should only be one Missed context at a time associated with one entry, as subsequent
    // "misses" should be StreamListening, waiting on the first one to either write to the
    // cache or announce NotCacheable state.
    Missed,
    // CheckingFile is the beginning of a fresh cache lookup, that can turn out to
    // be a miss if the file is not present, or a hit that still leads to an insert if
    // the cache entry is expired.
    CheckingFile,
    // ReadingFile is the state after the cache file has been found to be valid - the cache
    // file should be read to the client.
    ReadingFile,
  };
  FileLookupContext(FileSystemHttpCache& cache,
                    std::shared_ptr<ActiveCacheEntry> active_cache_entry, LookupRequest&& lookup)
      : cache_(cache), entry_(active_cache_entry), key_(lookup.key()), lookup_(std::move(lookup)) {}

  // From LookupContext
  void getHeaders(LookupHeadersCallback&& cb) final;
  void getBody(const AdjustedByteRange& range, LookupBodyCallback&& cb) final;
  void getTrailers(LookupTrailersCallback&& cb) final;
  void onDestroy() final;
  // This shouldn't be necessary since onDestroy is supposed to always be called, but in some
  // tests it is not.
  ~FileLookupContext() override { onDestroy(); }

  const LookupRequest& lookup() const { return lookup_; }
  const Key& key() const { return key_; }

private:
  LookupHeadersCallback headers_cb_;
  LookupBodyCallback body_cb_;
  LookupTrailersCallback trailers_cb_;
  friend class ActiveCacheEntry;

  // In the event that the cache failed to retrieve, remove the cache entry from the
  // cache so we don't keep repeating the same failure.
  void invalidateCacheEntry();

  // Attempts to open the cache file. On failure calls headers_cb_ with a miss,
  // otherwise calls headers_cb_ with the headers, which may still lead to an
  // upstream request if the cache entry is expired.
  void checkCacheEntryExistence();

  std::string filepath() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // We can safely use a reference here, because the shared_ptr to a cache is guaranteed to outlive
  // all filters that use it.
  FileSystemHttpCache& cache_;
  std::shared_ptr<ActiveCacheEntry> entry_;

  // File actions may be initiated in the file thread or the filter thread, and cancelled or
  // completed from either, therefore must be guarded by a mutex.
  absl::Mutex mu_;
  AsyncFileHandle file_handle_ ABSL_GUARDED_BY(mu_);
  CancelFunction cancel_action_in_flight_ ABSL_GUARDED_BY(mu_);
  CacheFileFixedBlock header_block_ ABSL_GUARDED_BY(mu_);
  Key key_ ABSL_GUARDED_BY(mu_);
  State state_ ABSL_GUARDED_BY(mu_) = State::NotInitialized;

  const LookupRequest lookup_;
};

// TODO(ravenblack): A CacheEntryInProgressReader should be implemented to prevent
// "thundering herd" problem.
//
// First the insert needs to be performed not by using the existing request but by
// issuing its own request[s], otherwise the first client to request a resource could
// provoke failure for any other clients sharing that data-stream, by closing its
// request before the cache population is completed.
//
// The plan is to make the entire cache insert happen "out of band", and to populate
// the cache with a CacheEntryInProgress object, allowing clients to stream from it in
// parallel.
//
// This may require intercepting at the initialization of LookupContext to trigger
// immediate "InProgress" cache insertion for any resource compatible with cache
// insertion, and the beginning of that out-of-band download - this way the original
// requester can be a sibling of any subsequent requester, whereas if we waited for
// the cache filter's insert path to be reached then the process would potentially be
// much more confusing (because we will never want a stream to be doing the inserting
// if we have an external task for that, and because there would be a race where two
// clients could get past the lookup before either creates an InsertContext).
//
// The current, early implementation simply allows requests to bypass the cache when
// the cache entry is in the process of being populated. It is therefore subject to
// the "thundering herd" problem.

} // namespace FileSystemHttpCache
} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
