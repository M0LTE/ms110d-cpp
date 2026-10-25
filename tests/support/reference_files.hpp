// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Reader for the C# reference files (format_version 2; see docs/dev/reference-files.md and
// the C# repo's docs/reference-files.md). Test support only. The directory is named by the
// environment variable MS110D_REFERENCE_DIR, which CMake sets for ctest to the output of
// scripts/generate-reference.sh. Tests that need the files call reference_dir_or_skip(),
// which skips the test when they are absent, unless MS110D_REQUIRE_REFERENCE is set (CI),
// when absence is a failure.

#include <complex>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ms110d_test {

// key=value text, one pair per line. A repeated key is an error (std::runtime_error).
class Manifest {
public:
    static Manifest parse(const std::string& text);
    static Manifest load(const std::filesystem::path& file);  // throws std::runtime_error

    bool has(const std::string& key) const;
    // These throw std::runtime_error if the key is missing or not of that kind.
    const std::string& str(const std::string& key) const;
    long long integer(const std::string& key) const;
    double real(const std::string& key) const;

private:
    std::map<std::string, std::string> kv_;
};

// index.txt: the set's header and its case names, in order.
struct Index {
    int format_version = 0;
    std::string package_version;
    std::string git_commit;
    bool dirty = false;
    std::vector<std::string> cases;
};

class ReferenceCase {
public:
    ReferenceCase(std::filesystem::path dir, std::string name);

    const std::string& name() const { return name_; }
    const std::filesystem::path& dir() const { return dir_; }
    const Manifest& manifest() const { return manifest_; }

    // Stage files are named by the manifest key file.<stage> and their element counts by
    // count.<stage>; the stage names are listed in the C# docs (payload_bits, framed_bits,
    // burst_symbols, probe_values_cf32le, audio_f32le and so on). Each loader checks the file
    // size against the count and throws std::runtime_error if they disagree.
    std::vector<std::uint8_t> bytes(const std::string& stage) const;  // .bits and .sym files
    std::vector<std::complex<float>> complex32(const std::string& stage) const;  // .cf32
    std::vector<float> real32(const std::string& stage) const;  // .f32

    // The same two stages as the C# produces them with the D.5.2.2 standard probe pattern (the
    // reference files themselves hold the C# 0.2.1 default pattern, issue 68), from
    // scripts/generate-standard-reference.sh. Only the probe values and the audio differ between
    // the patterns; every other stage is shared. Only "probe_values_cf32le" and "audio_f32le" exist.
    std::vector<std::complex<float>> standard_complex32(const std::string& stage) const;
    std::vector<float> standard_real32(const std::string& stage) const;

private:
    std::vector<std::uint8_t> raw(const std::string& stage, std::size_t element_size) const;
    std::vector<std::uint8_t> raw_at(const std::filesystem::path& file, const std::string& stage, std::size_t element_size) const;
    std::filesystem::path standard_dir() const;
    static std::vector<float> decode_real32(const std::vector<std::uint8_t>& bytes);
    static std::vector<std::complex<float>> decode_complex32(const std::vector<std::uint8_t>& bytes);

    std::filesystem::path dir_;
    std::string name_;
    Manifest manifest_;
};

// The reference directory if the environment names one that holds index.txt.
std::optional<std::filesystem::path> reference_dir();

// For use at the top of a test: returns the directory, or skips the test (SKIP) when the
// files are absent, or fails it when MS110D_REQUIRE_REFERENCE is set.
std::filesystem::path reference_dir_or_skip();

// Like reference_dir_or_skip, and also needs the standard-pattern files, which sit beside it in
// <cache>/standard (the reference files are <cache>/ref).
std::filesystem::path standard_reference_dir_or_skip();
std::filesystem::path standard_root(const std::filesystem::path& reference_dir);

Index load_index(const std::filesystem::path& dir);  // throws std::runtime_error

}  // namespace ms110d_test
