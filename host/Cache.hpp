// Accessor for the single shared cache instance, used by both the serve face and the prefetch pool.
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

#include "LruCache.hpp"

// The cache face (Cache.cpp) plugs this instance into the serve pipeline (Provide + Served); the prefetch
// pool (Prefetch.cpp) reads and writes it in place (Contains / Put) to warm dependencies. Both share this
// one LruCache instance.
namespace wxl::scripts::hostext
{
    /**
     * @brief Returns the single shared cache (1 GB ceiling, 60s TTL), constructed on first use.
     * @return reference to the process-wide LruCache instance.
     */
    LruCache& Cache();
}
