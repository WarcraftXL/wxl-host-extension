// Extracts the direct dependency names referenced by a served asset.
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
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wxl::scripts::hostext::depscan
{
    /**
     * @brief Appends the names of the files a served asset directly references (ADT -> textures/models/wmo;
     *        WMO root -> textures/doodads + group files; M2 -> .skin + textures). Pure byte-logic over byte
     *        structures shared by native and modern formats: no format module, no engine state, no locks, no IO.
     * @param name   served asset name; its extension selects the byte structure to walk.
     * @param bytes  served asset payload.
     * @param out    vector the discovered dependency names are appended to.
     */
    void Scan(std::string_view name, std::span<const uint8_t> bytes, std::vector<std::string>& out);
}
