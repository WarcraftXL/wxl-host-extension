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

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

/**
 * @brief Name -> bytes cache for the serve hot path: sharded by name hash, intrusive LRU per shard
 *        (list + map) for O(1) move-to-front and O(1) eviction, with a TTL tail sweep. Bytes are held
 *        by shared_ptr so a lookup hands back a reference under the lock and the caller reads it
 *        outside, keeping the critical section free of payload copies.
 */
namespace wxl::scripts::hostext
{
    using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

    class LruCache
    {
    public:
        /**
         * @brief Constructs the cache with a total byte ceiling split evenly across shards and a per-entry TTL.
         * @param capBytes  total byte capacity, divided across the shards.
         * @param ttlMs      idle lifetime of an entry in milliseconds before a tail sweep drops it.
         */
        LruCache(size_t capBytes, uint64_t ttlMs);
        ~LruCache();

        /**
         * @brief Looks up name, refreshing recency on a hit.
         * @param name  asset name key.
         * @return shared bytes on a hit (kept alive for the caller to read outside the lock), null on a miss.
         */
        Bytes Get(std::string_view name);

        /**
         * @brief Stores a copy of bytes under name; no-op if already present or larger than a shard's capacity.
         * @param name   asset name key.
         * @param bytes  payload, copied before the shard lock is taken.
         */
        void Put(std::string_view name, std::span<const uint8_t> bytes);

        /**
         * @brief Reports whether name is cached without copying its payload or refreshing recency.
         * @param name  asset name key.
         * @return true if name is present in its shard.
         */
        bool Contains(std::string_view name) const;

        struct Stats { size_t entries; size_t bytes; unsigned long long hits; unsigned long long stores; };

        /**
         * @brief Samples live cache totals and lifetime counters across all shards.
         * @return entry count, resident bytes, lifetime hits, and lifetime stores.
         */
        Stats Snapshot() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
