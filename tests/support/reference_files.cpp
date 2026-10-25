// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "reference_files.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace ms110d_test {

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

Manifest Manifest::parse(const std::string& text) {
    Manifest m;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        if (!m.kv_.emplace(key, line.substr(eq + 1)).second) {
            throw std::runtime_error("manifest repeats key " + key);
        }
    }
    return m;
}

Manifest Manifest::load(const fs::path& file) {
    return parse(read_file(file));
}

bool Manifest::has(const std::string& key) const {
    return kv_.count(key) != 0;
}

const std::string& Manifest::str(const std::string& key) const {
    const auto it = kv_.find(key);
    if (it == kv_.end()) throw std::runtime_error("manifest has no key " + key);
    return it->second;
}

long long Manifest::integer(const std::string& key) const {
    const std::string& s = str(key);
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (s.empty() || *end != '\0') throw std::runtime_error("manifest key " + key + " is not an integer: " + s);
    return v;
}

double Manifest::real(const std::string& key) const {
    const std::string& s = str(key);
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (s.empty() || *end != '\0') throw std::runtime_error("manifest key " + key + " is not a number: " + s);
    return v;
}

ReferenceCase::ReferenceCase(fs::path dir, std::string name)
    : dir_(std::move(dir)), name_(std::move(name)), manifest_(Manifest::load(dir_ / "manifest.txt")) {}

std::vector<std::uint8_t> ReferenceCase::raw(const std::string& stage, std::size_t element_size) const {
    return raw_at(dir_ / manifest_.str("file." + stage), stage, element_size);
}

std::vector<std::uint8_t> ReferenceCase::raw_at(const fs::path& file, const std::string& stage, std::size_t element_size) const {
    const std::string data = read_file(file);
    const long long count = manifest_.integer("count." + stage);
    if (data.size() != static_cast<std::size_t>(count) * element_size) {
        throw std::runtime_error(name_ + ": stage " + stage + " has " + std::to_string(data.size()) +
                                 " bytes, manifest says " + std::to_string(count) + " elements");
    }
    return std::vector<std::uint8_t>(data.begin(), data.end());
}

std::vector<std::uint8_t> ReferenceCase::bytes(const std::string& stage) const {
    return raw(stage, 1);
}

std::vector<float> ReferenceCase::real32(const std::string& stage) const {
    return decode_real32(raw(stage, 4));
}

std::vector<float> ReferenceCase::standard_real32(const std::string& stage) const {
    return decode_real32(raw_at(standard_dir() / manifest_.str("file." + stage), stage, 4));
}

std::vector<std::complex<float>> ReferenceCase::complex32(const std::string& stage) const {
    return decode_complex32(raw(stage, 8));
}

std::vector<std::complex<float>> ReferenceCase::standard_complex32(const std::string& stage) const {
    return decode_complex32(raw_at(standard_dir() / manifest_.str("file." + stage), stage, 8));
}

fs::path ReferenceCase::standard_dir() const {
    return standard_root(dir_.parent_path()) / name_;
}

std::vector<float> ReferenceCase::decode_real32(const std::vector<std::uint8_t>& b) {
    std::vector<float> out(b.size() / 4);
    for (std::size_t i = 0; i < out.size(); ++i) {
        // Little-endian float32; build the word explicitly so big-endian hosts also work.
        const std::uint32_t w = static_cast<std::uint32_t>(b[4 * i]) | (static_cast<std::uint32_t>(b[4 * i + 1]) << 8) |
                                (static_cast<std::uint32_t>(b[4 * i + 2]) << 16) |
                                (static_cast<std::uint32_t>(b[4 * i + 3]) << 24);
        std::memcpy(&out[i], &w, 4);
    }
    return out;
}

std::vector<std::complex<float>> ReferenceCase::decode_complex32(const std::vector<std::uint8_t>& b) {
    std::vector<std::complex<float>> out(b.size() / 8);
    for (std::size_t i = 0; i < out.size(); ++i) {
        float parts[2];
        for (int j = 0; j < 2; ++j) {
            const std::size_t o = 8 * i + 4 * static_cast<std::size_t>(j);
            const std::uint32_t w = static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8) |
                                    (static_cast<std::uint32_t>(b[o + 2]) << 16) |
                                    (static_cast<std::uint32_t>(b[o + 3]) << 24);
            std::memcpy(&parts[j], &w, 4);
        }
        out[i] = {parts[0], parts[1]};
    }
    return out;
}

std::optional<fs::path> reference_dir() {
    const char* env = std::getenv("MS110D_REFERENCE_DIR");
    if (env == nullptr || *env == '\0') return std::nullopt;
    fs::path p(env);
    if (!fs::exists(p / "index.txt")) return std::nullopt;
    return p;
}

fs::path reference_dir_or_skip() {
    if (auto d = reference_dir()) return *d;
    const char* require = std::getenv("MS110D_REQUIRE_REFERENCE");
    if (require != nullptr && *require != '\0' && std::strcmp(require, "0") != 0) {
        FAIL("MS110D_REQUIRE_REFERENCE is set but MS110D_REFERENCE_DIR does not hold the reference files");
    }
    SKIP("reference files not generated (run scripts/generate-reference.sh, see docs/dev/reference-files.md)");
    return {};
}

fs::path standard_root(const fs::path& reference_dir) {
    return reference_dir.parent_path() / "standard";
}

fs::path standard_reference_dir_or_skip() {
    const fs::path dir = reference_dir_or_skip();
    if (fs::exists(standard_root(dir) / load_index(dir).cases.front() / "13_audio.f32")) {
        return dir;
    }
    const char* require = std::getenv("MS110D_REQUIRE_REFERENCE");
    if (require != nullptr && *require != '\0' && std::strcmp(require, "0") != 0) {
        FAIL("MS110D_REQUIRE_REFERENCE is set but the standard-pattern reference files are absent (scripts/generate-standard-reference.sh)");
    }
    SKIP("standard-pattern reference files not generated (run scripts/generate-standard-reference.sh, see docs/dev/reference-files.md)");
    return {};
}

Index load_index(const fs::path& dir) {
    Index idx;
    // index.txt repeats case=, so the header is parsed from the other lines.
    std::string header_text;
    {
        std::istringstream all(read_file(dir / "index.txt"));
        std::string l;
        while (std::getline(all, l)) {
            if (l.rfind("case=", 0) != 0) header_text += l + "\n";
        }
    }
    const Manifest header = Manifest::parse(header_text);
    idx.format_version = static_cast<int>(header.integer("format_version"));
    idx.package_version = header.str("package_version");
    idx.git_commit = header.str("git_commit");
    idx.dirty = header.str("dirty") == "true";
    std::istringstream in(read_file(dir / "index.txt"));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("case=", 0) == 0) idx.cases.push_back(line.substr(5));
    }
    return idx;
}

}  // namespace ms110d_test
