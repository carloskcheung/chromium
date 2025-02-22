// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_DNS_HOST_CACHE_H_
#define NET_DNS_HOST_CACHE_H_

#include <stddef.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>

#include "base/gtest_prod_util.h"
#include "base/macros.h"
#include "base/threading/thread_checker.h"
#include "base/time/time.h"
#include "net/base/address_family.h"
#include "net/base/address_list.h"
#include "net/base/expiring_cache.h"
#include "net/base/net_export.h"
#include "net/dns/dns_util.h"
#include "net/dns/host_resolver_source.h"
#include "net/dns/public/dns_query_type.h"

namespace base {
class ListValue;
class TickClock;
}  // namespace base

namespace net {

// Cache used by HostResolver to map hostnames to their resolved result.
class NET_EXPORT HostCache {
 public:
  struct NET_EXPORT Key {
    Key(const std::string& hostname,
        DnsQueryType dns_query_type,
        HostResolverFlags host_resolver_flags,
        HostResolverSource host_resolver_source);
    Key(const std::string& hostname,
        AddressFamily address_family,
        HostResolverFlags host_resolver_flags);
    Key();

    bool operator<(const Key& other) const {
      // The order of comparisons of |Key| fields is arbitrary, thus
      // |dns_query_type| and |host_resolver_flags| are compared before
      // |hostname| under assumption that integer comparisons are faster than
      // string comparisons.
      return std::tie(dns_query_type, host_resolver_flags, hostname,
                      host_resolver_source) <
             std::tie(other.dns_query_type, other.host_resolver_flags,
                      other.hostname, other.host_resolver_source);
    }

    std::string hostname;
    DnsQueryType dns_query_type;
    HostResolverFlags host_resolver_flags;
    HostResolverSource host_resolver_source;
  };

  struct NET_EXPORT EntryStaleness {
    // Time since the entry's TTL has expired. Negative if not expired.
    base::TimeDelta expired_by;

    // Number of network changes since this result was cached.
    int network_changes;

    // Number of hits to the cache entry while stale (expired or past-network).
    int stale_hits;

    bool is_stale() const {
      return network_changes > 0 || expired_by >= base::TimeDelta();
    }
  };

  // Stores the latest address list that was looked up for a hostname.
  class NET_EXPORT Entry {
   public:
    enum Source : int {
      // Address list was obtained from an unknown source.
      SOURCE_UNKNOWN,
      // Address list was obtained via a DNS lookup.
      SOURCE_DNS,
      // Address list was obtained by searching a HOSTS file.
      SOURCE_HOSTS,
    };

    Entry(int error,
          const AddressList& addresses,
          Source source,
          base::TimeDelta ttl);
    // Use when |ttl| is unknown.
    Entry(int error, const AddressList& addresses, Source source);
    Entry(Entry&& entry);
    ~Entry();

    int error() const { return error_; }
    const AddressList& addresses() const { return addresses_; }
    Source source() const { return source_; }
    bool has_ttl() const { return ttl_ >= base::TimeDelta(); }
    base::TimeDelta ttl() const { return ttl_; }

    base::TimeTicks expires() const { return expires_; }

    // Public for the net-internals UI.
    int network_changes() const { return network_changes_; }

   private:
    friend class HostCache;

    Entry(const Entry& entry,
          base::TimeTicks now,
          base::TimeDelta ttl,
          int network_changes);

    Entry(int error,
          const AddressList& addresses,
          Source source,
          base::TimeTicks expires,
          int network_changes);

    int total_hits() const { return total_hits_; }
    int stale_hits() const { return stale_hits_; }

    bool IsStale(base::TimeTicks now, int network_changes) const;
    void CountHit(bool hit_is_stale);
    void GetStaleness(base::TimeTicks now,
                      int network_changes,
                      EntryStaleness* out) const;

    // The resolve results for this entry.
    int error_;
    AddressList addresses_;
    // Where addresses_ were obtained (e.g. DNS lookup, hosts file, etc).
    Source source_;
    // TTL obtained from the nameserver. Negative if unknown.
    base::TimeDelta ttl_;

    base::TimeTicks expires_;
    // Copied from the cache's network_changes_ when the entry is set; can
    // later be compared to it to see if the entry was received on the current
    // network.
    int network_changes_;
    int total_hits_;
    int stale_hits_;
  };

  // Interface for interacting with persistent storage, to be provided by the
  // embedder. Does not include support for writes that must happen immediately.
  class PersistenceDelegate {
   public:
    // Calling ScheduleWrite() signals that data has changed and should be
    // written to persistent storage. The write might be delayed.
    virtual void ScheduleWrite() = 0;
  };

  using EntryMap = std::map<Key, Entry>;

  // Constructs a HostCache that stores up to |max_entries|.
  explicit HostCache(size_t max_entries);

  ~HostCache();

  // Returns a pointer to the entry for |key|, which is valid at time
  // |now|. If there is no such entry, returns NULL.
  const Entry* Lookup(const Key& key, base::TimeTicks now);

  // Returns a pointer to the entry for |key|, whether it is valid or stale at
  // time |now|. Fills in |stale_out| with information about how stale it is.
  // If there is no entry for |key| at all, returns NULL.
  const Entry* LookupStale(const Key& key,
                           base::TimeTicks now,
                           EntryStaleness* stale_out);

  // Overwrites or creates an entry for |key|.
  // |entry| is the value to set, |now| is the current time
  // |ttl| is the "time to live".
  void Set(const Key& key,
           const Entry& entry,
           base::TimeTicks now,
           base::TimeDelta ttl);

  // Checks whether an entry exists for |hostname|.
  // If so, returns true and writes the source (e.g. DNS, HOSTS file, etc.) to
  // |source_out| and the staleness to |stale_out| (if they are not null).
  // It tries using two common address_family and host_resolver_flag
  // combinations when performing lookups in the cache; this means false
  // negatives are possible, but unlikely.
  bool HasEntry(base::StringPiece hostname,
                HostCache::Entry::Source* source_out,
                HostCache::EntryStaleness* stale_out);

  // Marks all entries as stale on account of a network change.
  void OnNetworkChange();

  void set_persistence_delegate(PersistenceDelegate* delegate);

  void set_tick_clock_for_testing(const base::TickClock* tick_clock) {
    tick_clock_ = tick_clock;
  }

  // Empties the cache.
  void clear();

  // Clears hosts matching |host_filter| from the cache.
  void ClearForHosts(
      const base::Callback<bool(const std::string&)>& host_filter);

  // Fills the provided base::ListValue with the contents of the cache for
  // serialization. |entry_list| must be non-null and will be cleared before
  // adding the cache contents.
  void GetAsListValue(base::ListValue* entry_list,
                      bool include_staleness) const;
  // Takes a base::ListValue representing cache entries and stores them in the
  // cache, skipping any that already have entries. Returns true on success,
  // false on failure.
  bool RestoreFromListValue(const base::ListValue& old_cache);
  // Returns the number of entries that were restored in the last call to
  // RestoreFromListValue().
  size_t last_restore_size() const { return restore_size_; }

  // Returns the number of entries in the cache.
  size_t size() const;

  // Following are used by net_internals UI.
  size_t max_entries() const;
  int network_changes() const { return network_changes_; }
  const EntryMap& entries() const { return entries_; }

  // Creates a default cache.
  static std::unique_ptr<HostCache> CreateDefaultCache();

 private:
  FRIEND_TEST_ALL_PREFIXES(HostCacheTest, NoCache);

  enum SetOutcome : int;
  enum LookupOutcome : int;
  enum EraseReason : int;

  Entry* LookupInternal(const Key& key);

  void RecordSet(SetOutcome outcome,
                 base::TimeTicks now,
                 const Entry* old_entry,
                 const Entry& new_entry,
                 AddressListDeltaType delta);
  void RecordUpdateStale(AddressListDeltaType delta,
                         const EntryStaleness& stale);
  void RecordLookup(LookupOutcome outcome,
                    base::TimeTicks now,
                    const Entry* entry);
  void RecordErase(EraseReason reason, base::TimeTicks now, const Entry& entry);
  void RecordEraseAll(EraseReason reason, base::TimeTicks now);

  // Returns true if this HostCache can contain no entries.
  bool caching_is_disabled() const { return max_entries_ == 0; }

  void EvictOneEntry(base::TimeTicks now);
  // Helper to insert an Entry into the cache.
  void AddEntry(const Key& key, Entry&& entry);

  // Map from hostname (presumably in lowercase canonicalized format) to
  // a resolved result entry.
  EntryMap entries_;
  size_t max_entries_;
  int network_changes_;
  // Number of cache entries that were restored in the last call to
  // RestoreFromListValue(). Used in histograms.
  size_t restore_size_;

  PersistenceDelegate* delegate_;
  // Shared tick clock, overridden for testing.
  const base::TickClock* tick_clock_;

  THREAD_CHECKER(thread_checker_);

  DISALLOW_COPY_AND_ASSIGN(HostCache);
};

}  // namespace net

#endif  // NET_DNS_HOST_CACHE_H_
