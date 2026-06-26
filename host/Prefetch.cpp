// Warms the shared cache with a served asset's dependencies, ahead of the client's opens.
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
#include "DepScan.hpp"

#include "core/Logger.hpp"
#include "mpq/MpqStore.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

// When a real asset is served to the client, its direct dependencies are warmed into the shared cache on
// background threads, so the client's later synchronous opens hit the cache instead of stalling on a read +
// transform. This file is the POOL + the host-hook face; the dep extraction lives in DepScan (pure byte-
// logic). It shares the cache instance with the cache face (Cache.hpp) and warms by writing the cache
// DIRECTLY -- no hook round-trip. A warm never re-enters the serve pipeline, so it never schedules its own
// deps (DEPTH 1). Depends only on the host core + the core MpqStore, never on a format module. Each pool
// thread owns its MpqStore (StormLib never crosses threads).
namespace
{
    namespace wlog = wxl::core::log;
    namespace hx   = wxl::scripts::hostext;
    namespace scan = wxl::scripts::hostext::depscan;
    using wxl::host::mpq::MpqStore;

    constexpr uint32_t kThreads  = 3;
    constexpr size_t   kQueueCap = 32768;

    std::mutex              g_qmutex;
    std::condition_variable g_cv;
    std::deque<std::string> g_queue;        // pending dep names
    std::unordered_set<std::string> g_seen; // queued-or-done names (dedup)
    std::atomic<uint32_t>   g_warmed{ 0 };
    std::once_flag          g_poolOnce;

    /**
     * @brief Scans a served asset for its direct dependencies and enqueues the new ones, waking the pool.
     * @param name   asset name whose bytes are being scanned.
     * @param bytes  served payload to extract dependency names from.
     */
    void EnqueueDeps(std::string_view name, std::span<const uint8_t> bytes)
    {
        std::vector<std::string> deps;
        scan::Scan(name, bytes, deps);
        if (deps.empty()) return;

        bool any = false;
        {
            std::lock_guard<std::mutex> lk(g_qmutex);
            for (std::string& d : deps)
            {
                if (g_queue.size() >= kQueueCap) break;
                if (g_seen.insert(d).second) { g_queue.push_back(std::move(d)); any = true; }
            }
        }
        if (any) g_cv.notify_all();
    }

    /**
     * @brief Pool thread body: owns an MpqStore, drains the queue, warms the shared cache in place, and
     *        enqueues each warmed asset's own dependencies so the whole reachable tree is warmed ahead of
     *        the client.
     */
    void Worker()
    {
        // Background warming yields to the serve thread so it never slows the client's actual opens.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        MpqStore store;
        store.Mount(wxl::host::ClientRoot());

        std::vector<uint8_t> raw, reshaped;
        for (;;)
        {
            std::string name;
            {
                std::unique_lock<std::mutex> lk(g_qmutex);
                g_cv.wait(lk, [] { return !g_queue.empty(); });
                name = std::move(g_queue.front());
                g_queue.pop_front();
            }

            // Already warm (the main thread or another worker cached it): no copy, no produce.
            if (hx::Cache().Contains(name)) continue;

            raw.clear();
            if (!store.ReadAll(name, raw)) continue;

            // Produce the served form (apply any module transform), then store it directly in the shared
            // cache and warm this asset's own dependencies (the transitive closure, bounded by the dedup
            // set and the queue cap).
            reshaped.clear();
            const std::vector<uint8_t>& served =
                wxl::host::Transform(name, raw, reshaped) ? reshaped : raw;
            hx::Cache().Put(name, served);
            EnqueueDeps(name, served);

            uint32_t w = ++g_warmed;
            if ((w % 1000) == 0) wlog::Printf("prefetch: warmed=%u", w);
        }
    }

    /**
     * @brief Spawns and detaches the worker threads once.
     */
    void StartPool()
    {
        for (uint32_t i = 0; i < kThreads; ++i)
            std::thread(Worker).detach();
        wlog::Printf("prefetch: pool started (%u threads)", kThreads);
    }

    /**
     * @brief Served hook: scans the served bytes for deps, enqueues new ones under one lock, and wakes the pool.
     *        Fires only for a client open, since warming writes the cache directly and never through the pipeline.
     * @param name   asset name that was served.
     * @param bytes  served payload to scan for dependencies.
     */
    void OnServed(std::string_view name, std::span<const uint8_t> bytes)
    {
        std::call_once(g_poolOnce, StartPool);
        EnqueueDeps(name, bytes);
    }

    /**
     * @brief Registers the prefetch served hook with the host at static-init time.
     */
    struct Registrar
    {
        Registrar() { wxl::host::RegisterServed("prefetch", &OnServed); }
    };
    Registrar g_registrar;
}
