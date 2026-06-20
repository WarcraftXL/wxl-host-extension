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

#include "DepScan.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace wxl::scripts::hostext::depscan
{
    namespace
    {
        /**
         * @brief Tests whether s ends with suffix, case-insensitively.
         * @param s       string to test.
         * @param suffix  lowercase suffix to match against the tail of s.
         * @return true if s ends with suffix ignoring case.
         */
        bool EndsWithCI(std::string_view s, std::string_view suffix)
        {
            if (suffix.size() > s.size()) return false;
            for (size_t i = 0; i < suffix.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) != suffix[i])
                    return false;
            return true;
        }

        /**
         * @brief Reads a little-endian 32-bit value from p.
         * @param p  pointer to at least four readable bytes.
         * @return the decoded 32-bit value.
         */
        uint32_t Rd32le(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

        /**
         * @brief Packs a 4CC chunk tag as stored little-endian.
         * @param a  first tag character.
         * @param b  second tag character.
         * @param c  third tag character.
         * @param d  fourth tag character.
         * @return the four characters packed into a little-endian 32-bit tag.
         */
        constexpr uint32_t Tag(char a, char b, char c, char d)
        {
            return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) |
                   (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
        }

        /**
         * @brief Tests whether s looks like a real archive path, rejecting garbage from a misparse.
         * @param s  candidate string.
         * @return true if s is a plausible path (bounded length, no control bytes, has a separator or known suffix).
         */
        bool LooksLikePath(const std::string& s)
        {
            if (s.empty() || s.size() > 260) return false;
            for (unsigned char c : s) if (c < 0x20) return false;
            if (s.find('\\') != std::string::npos || s.find('/') != std::string::npos) return true;
            return EndsWithCI(s, ".blp") || EndsWithCI(s, ".m2") || EndsWithCI(s, ".mdx") || EndsWithCI(s, ".wmo");
        }

        /**
         * @brief Appends each NUL-terminated string packed in a name-blob payload (MTEX / MMDX / MWMO / MOTX / MODN).
         * @param p    pointer to the blob payload.
         * @param len  blob length in bytes.
         * @param out  vector path-like strings are appended to.
         */
        void AppendNameBlob(const uint8_t* p, uint32_t len, std::vector<std::string>& out)
        {
            uint32_t i = 0;
            while (i < len)
            {
                uint32_t start = i;
                while (i < len && p[i] != 0) ++i;
                if (i > start)
                {
                    std::string s(reinterpret_cast<const char*>(p + start), i - start);
                    if (LooksLikePath(s)) out.push_back(std::move(s));
                }
                ++i; // skip the NUL
            }
        }

        /**
         * @brief Walks a monolithic ADT's top-level chunks, pulling texture / model / wmo names from the name blobs.
         * @param buf  pointer to the ADT bytes.
         * @param len  byte length of the ADT.
         * @param out  vector discovered dependency names are appended to.
         */
        void ScanAdt(const uint8_t* buf, uint32_t len, std::vector<std::string>& out)
        {
            constexpr uint32_t kMTEX = Tag('M', 'T', 'E', 'X');
            constexpr uint32_t kMMDX = Tag('M', 'M', 'D', 'X');
            constexpr uint32_t kMWMO = Tag('M', 'W', 'M', 'O');
            uint32_t o = 0;
            while (o + 8 <= len)
            {
                uint32_t m = Rd32le(buf + o), sz = Rd32le(buf + o + 4);
                if (o + 8 + sz > len) break;
                if (m == kMTEX || m == kMMDX || m == kMWMO) AppendNameBlob(buf + o + 8, sz, out);
                o += 8 + sz;
            }
        }

        /**
         * @brief Scans a WMO root for textures (MOTX) and doodad models (MODN) and emits the per-group file
         *        names derived from the group count.
         * @param name  root WMO name, used to build the "_NNN.wmo" group names.
         * @param buf   pointer to the WMO root bytes.
         * @param len   byte length of the WMO root.
         * @param out   vector discovered dependency names are appended to.
         */
        void ScanWmoRoot(const std::string& name, const uint8_t* buf, uint32_t len, std::vector<std::string>& out)
        {
            constexpr uint32_t kMOHD = Tag('M', 'O', 'H', 'D');
            constexpr uint32_t kMOGI = Tag('M', 'O', 'G', 'I');
            constexpr uint32_t kMOTX = Tag('M', 'O', 'T', 'X');
            constexpr uint32_t kMODN = Tag('M', 'O', 'D', 'N');

            uint32_t groupCount = 0, o = 0;
            while (o + 8 <= len)
            {
                uint32_t m = Rd32le(buf + o), sz = Rd32le(buf + o + 4);
                if (o + 8 + sz > len) break;
                const uint8_t* data = buf + o + 8;
                if (m == kMOHD && sz >= 0x08)           groupCount = Rd32le(data + 0x04); // nGroups
                else if (m == kMOGI && groupCount == 0) groupCount = sz / 0x20;           // fallback
                else if (m == kMOTX || m == kMODN)      AppendNameBlob(data, sz, out);
                o += 8 + sz;
            }

            const std::string base = name.substr(0, name.size() - 4); // strip ".wmo"
            for (uint32_t i = 0; i < groupCount && i < 512; ++i)
            {
                char suffix[16];
                std::snprintf(suffix, sizeof(suffix), "_%03u.wmo", i);
                out.push_back(base + suffix);
            }
        }

        /**
         * @brief Scans a de-chunked MD20 model, emitting "<base>00.skin" and any inline (type 0) texture
         *        filenames. The texture list is at header+0x50 {count,offset}; each record
         *        {u32 type, u32 flags, M2Array name} spans 0x10 bytes.
         * @param name  M2 name, used to build the .skin name.
         * @param buf   pointer to the model bytes.
         * @param len   byte length of the model.
         * @param out   vector discovered dependency names are appended to.
         */
        void ScanM2(const std::string& name, const uint8_t* buf, uint32_t len, std::vector<std::string>& out)
        {
            size_t dot = name.find_last_of('.');
            if (dot != std::string::npos) out.push_back(name.substr(0, dot) + "00.skin");

            if (len < 0x58 || Rd32le(buf) != 0x3032444D /*'MD20'*/) return;

            const uint32_t texCount = Rd32le(buf + 0x50);
            const uint32_t texOfs   = Rd32le(buf + 0x54);
            if (texCount == 0 || texCount > 0x1000) return;
            if (static_cast<size_t>(texOfs) + static_cast<size_t>(texCount) * 0x10 > len) return;

            for (uint32_t i = 0; i < texCount; ++i)
            {
                const uint8_t* rec = buf + texOfs + i * 0x10;
                if (Rd32le(rec + 0x00) != 0) continue; // type 0 = inline filename
                uint32_t nameCount = Rd32le(rec + 0x08);
                uint32_t nameOfs   = Rd32le(rec + 0x0C);
                if (nameCount == 0 || nameOfs >= len) continue;
                uint32_t avail = len - nameOfs;
                uint32_t take = nameCount < avail ? nameCount : avail;
                const char* s = reinterpret_cast<const char*>(buf + nameOfs);
                uint32_t slen = 0; while (slen < take && s[slen] != 0) ++slen;
                std::string path(s, slen);
                if (LooksLikePath(path)) out.push_back(std::move(path));
            }
        }
    }

    /**
     * @brief Dispatches on the asset name's extension and appends the names it directly references.
     * @param name   served asset name; its extension selects the byte structure to walk.
     * @param bytes  served asset payload.
     * @param out    vector the discovered dependency names are appended to.
     */
    void Scan(std::string_view name, std::span<const uint8_t> bytes, std::vector<std::string>& out)
    {
        const uint8_t* buf = bytes.data();
        const uint32_t len = static_cast<uint32_t>(bytes.size());
        if (!buf || len < 8) return;

        std::string n(name);
        if (EndsWithCI(n, ".adt"))
            ScanAdt(buf, len, out);
        else if (EndsWithCI(n, ".wmo"))
        {
            // Only a root WMO carries deps; a "_NNN.wmo" group references nothing new.
            if (!(n.size() >= 8 && n[n.size() - 8] == '_'))
                ScanWmoRoot(n, buf, len, out);
        }
        else if (EndsWithCI(n, ".m2") || EndsWithCI(n, ".mdx"))
            ScanM2(n, buf, len, out);
    }
}
