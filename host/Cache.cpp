// Wires the shared LruCache into the host serve pipeline (provide on open, store on serve).
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

#include "Host.hpp"

#include "Cache.hpp"

#include "core/Logger.hpp"

#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// As a PROVIDER the cache serves a re-opened asset straight from RAM (skipping the archive read and the module
// transform); as a SERVED observer it stores the bytes the host just produced for a client open. The store
// itself is LruCache (sharded, O(1), lock-light); this file is only the thin host-hook face. The prefetch
// pool shares the same instance (see Cache.hpp) and writes to it directly, so warming needs no hook.
namespace wxl::scripts::hostext
{
    /**
     * @brief Returns the single shared cache (1 GB ceiling, 60s TTL), constructed on first use.
     * @return reference to the process-wide LruCache instance.
     */
    LruCache& Cache()
    {
        static LruCache c(1024ull * 1024 * 1024, 60u * 1000u);
        return c;
    }
}

namespace
{
    namespace wlog = wxl::core::log;
    namespace hx   = wxl::scripts::hostext;

    std::atomic<uint32_t> g_stores{ 0 };

    /**
     * @brief Provider hook: serves a cache hit, copying the bytes out of the shared entry outside the cache lock.
     * @param name  asset name being opened.
     * @param out   filled with the cached bytes on a hit.
     * @return true on a cache hit, false on a miss.
     */
    bool Provide(std::string_view name, std::vector<uint8_t>& out)
    {
        hx::Bytes b = hx::Cache().Get(name);
        if (!b) return false;
        out = *b;
        return true;
    }

    /**
     * @brief Served hook: stores the bytes the host just produced for a client open, logging totals periodically.
     * @param name   asset name that was served.
     * @param bytes  served payload to cache.
     */
    void OnServed(std::string_view name, std::span<const uint8_t> bytes)
    {
        hx::Cache().Put(name, bytes);
        if (((g_stores.fetch_add(1, std::memory_order_relaxed) + 1) % 2000) == 0)
        {
            auto s = hx::Cache().Snapshot();
            wlog::Printf("cache: %zu entries, %zu / 1024 MB | hits=%llu stores=%llu",
                         s.entries, s.bytes / (1024 * 1024), s.hits, s.stores);
        }
    }

    /**
     * @brief Registers the cache provider and served hooks with the host at static-init time.
     */
    struct Registrar
    {
        Registrar()
        {
            wxl::host::RegisterProvider("cache", &Provide);
            wxl::host::RegisterServed("cache", &OnServed);
        }
    };
    Registrar g_registrar;
}
