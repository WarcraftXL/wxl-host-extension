// Sharded, O(1)-eviction, TTL-bounded byte cache keyed by asset name. Host-internal.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "LruCache.hpp"

#include <windows.h> // GetTickCount64

#include <array>
#include <atomic>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace wxl::scripts::hostext
{
    namespace
    {
        constexpr size_t   kShards          = 16;   // power of two; serve + prefetch threads spread across them
        constexpr uint64_t kSweepThrottleMs = 2000; // min gap between a shard's TTL tail sweeps
    }

    struct LruCache::Impl
    {
        // One LRU entry. The node is stable in the list (splice never moves it), so the map keys it by a
        // string_view into node.key -- no duplicate key string, no per-lookup allocation.
        struct Node { std::string key; Bytes bytes; uint64_t lastAccess; };
        using List = std::list<Node>;
        using Map  = std::unordered_map<std::string_view, List::iterator>;

        struct Shard
        {
            mutable std::mutex m;
            List     list;       // front = most-recently-used, back = least
            Map      map;        // name -> list node
            size_t   bytes = 0;
            uint64_t lastSweep = 0;
        };

        size_t   shardCap;
        uint64_t ttlMs;
        std::array<Shard, kShards> shards;
        std::atomic<unsigned long long> hits{ 0 };
        std::atomic<unsigned long long> stores{ 0 };

        Impl(size_t capBytes, uint64_t ttl) : shardCap(capBytes / kShards), ttlMs(ttl) {}

        /**
         * @brief Maps a name to its shard index by hash.
         * @param name  asset name key.
         * @return shard index in [0, kShards).
         */
        static size_t Pick(std::string_view name) { return std::hash<std::string_view>{}(name) % kShards; }

        /**
         * @brief Drops the back (LRU end) node from a shard. Caller holds the shard lock.
         * @param s  shard to evict from.
         */
        static void EvictBack(Shard& s)
        {
            Node& back = s.list.back();
            s.bytes -= back.bytes->size();
            s.map.erase(std::string_view(back.key));
            s.list.pop_back();
        }

        /**
         * @brief Drops TTL-expired entries from the LRU tail (oldest first), throttled. Caller holds the shard lock.
         * @param s    shard to sweep.
         * @param now  current tick count in milliseconds.
         */
        void SweepTail(Shard& s, uint64_t now)
        {
            if (now - s.lastSweep < kSweepThrottleMs) return;
            s.lastSweep = now;
            while (!s.list.empty() && now - s.list.back().lastAccess > ttlMs)
                EvictBack(s);
        }

        /**
         * @brief Looks up name in its shard, moving the hit to the front and counting it.
         * @param name  asset name key.
         * @return the entry's shared bytes on a hit, null on a miss.
         */
        Bytes Get(std::string_view name)
        {
            Shard& s = shards[Pick(name)];
            std::lock_guard<std::mutex> lk(s.m);
            auto it = s.map.find(name);
            if (it == s.map.end()) return nullptr;
            List::iterator node = it->second;
            node->lastAccess = GetTickCount64();
            s.list.splice(s.list.begin(), s.list, node); // O(1) move-to-front; node + its key stay put
            hits.fetch_add(1, std::memory_order_relaxed);
            return node->bytes; // cheap shared_ptr copy; the caller reads the payload outside this lock
        }

        /**
         * @brief Inserts a copy of bytes under name in its shard, sweeping and evicting to stay under capacity.
         * @param name   asset name key.
         * @param bytes  payload; ignored if larger than a shard's capacity or already present.
         */
        void Put(std::string_view name, std::span<const uint8_t> bytes)
        {
            if (bytes.size() > shardCap) return;
            // Copy the payload BEFORE locking, so the critical section is just the list/map splice.
            auto data = std::make_shared<std::vector<uint8_t>>(bytes.begin(), bytes.end());

            Shard& s = shards[Pick(name)];
            std::lock_guard<std::mutex> lk(s.m);
            if (s.map.find(name) != s.map.end()) return; // already cached

            const uint64_t now = GetTickCount64();
            SweepTail(s, now);
            while (s.bytes + data->size() > shardCap && !s.list.empty())
                EvictBack(s);

            s.list.push_front(Node{ std::string(name), std::move(data), now });
            List::iterator node = s.list.begin();
            s.bytes += node->bytes->size();
            s.map.emplace(std::string_view(node->key), node); // key views into the stable node
            stores.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief Reports whether name is present in its shard without refreshing recency.
         * @param name  asset name key.
         * @return true if name is cached.
         */
        bool Contains(std::string_view name) const
        {
            const Shard& s = shards[Pick(name)];
            std::lock_guard<std::mutex> lk(s.m);
            return s.map.find(name) != s.map.end();
        }

        /**
         * @brief Sums entry counts and resident bytes across all shards alongside the lifetime counters.
         * @return entry count, resident bytes, lifetime hits, and lifetime stores.
         */
        Stats Snapshot() const
        {
            Stats st{ 0, 0, hits.load(), stores.load() };
            for (const Shard& s : shards)
            {
                std::lock_guard<std::mutex> lk(s.m);
                st.entries += s.list.size();
                st.bytes   += s.bytes;
            }
            return st;
        }
    };

    LruCache::LruCache(size_t capBytes, uint64_t ttlMs) : m_impl(std::make_unique<Impl>(capBytes, ttlMs)) {}
    LruCache::~LruCache() = default;

    Bytes LruCache::Get(std::string_view name) { return m_impl->Get(name); }
    void  LruCache::Put(std::string_view name, std::span<const uint8_t> bytes) { m_impl->Put(name, bytes); }
    bool  LruCache::Contains(std::string_view name) const { return m_impl->Contains(name); }
    LruCache::Stats LruCache::Snapshot() const { return m_impl->Snapshot(); }
}
