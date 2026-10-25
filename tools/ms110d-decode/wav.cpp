// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "wav.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ms110d_tool {

namespace {

std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint32_t le16(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8;
}

}  // namespace

bool read_wav(const std::string& path, std::vector<double>& samples, int& rate, std::string& error) {
    samples.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = path + ": cannot open";
        return false;
    }
    std::vector<unsigned char> b;
    unsigned char buf[1 << 16];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
        error = path + ": not a RIFF/WAVE file";
        return false;
    }
    size_t pos = 12;
    bool have_fmt = false, is_float = false;
    while (pos + 8 <= b.size()) {
        const unsigned char* c = b.data() + pos;
        size_t len = le32(c + 4);
        if (std::memcmp(c, "fmt ", 4) == 0 && pos + 24 <= b.size()) {
            unsigned fmt = le16(c + 8), ch = le16(c + 10), bits = le16(c + 22);
            rate = static_cast<int>(le32(c + 12));
            is_float = fmt == 3;
            if (!((fmt == 1 && bits == 16) || (fmt == 3 && bits == 32)) || ch != 1) {
                error = path + ": need mono 16-bit PCM or 32-bit float";
                return false;
            }
            have_fmt = true;
        } else if (std::memcmp(c, "data", 4) == 0) {
            if (!have_fmt) {
                error = path + ": data before fmt";
                return false;
            }
            size_t avail = std::min(len, b.size() - pos - 8);
            size_t w = is_float ? 4 : 2;
            size_t n = avail / w;
            samples.resize(n);
            const unsigned char* d = c + 8;
            for (size_t i = 0; i < n; ++i) {
                if (is_float) {
                    std::uint32_t u = le32(d + 4 * i);
                    float v;
                    std::memcpy(&v, &u, 4);
                    samples[i] = static_cast<double>(v);
                } else {
                    auto s = static_cast<std::int16_t>(le16(d + 2 * i));
                    samples[i] = static_cast<double>(s) / 32768.0;
                }
            }
            return true;
        }
        pos += 8 + len + (len & 1);
    }
    error = path + ": no data chunk";
    return false;
}

}  // namespace ms110d_tool
