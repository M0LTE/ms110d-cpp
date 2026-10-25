// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ms110d_tool {

// A mono WAV file's samples as doubles in -1..1 (16-bit PCM, or 32-bit float). Returns false and
// sets `error` if the file is unreadable or not mono PCM16/float32. Stereo is refused, not mixed.
bool read_wav(const std::string& path, std::vector<double>& samples, int& rate, std::string& error);

}  // namespace ms110d_tool
