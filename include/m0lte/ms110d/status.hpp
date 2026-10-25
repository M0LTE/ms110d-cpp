// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>

namespace m0lte::ms110d {

// Result of a call that can fail, for the transmitter and the receiver. The values equal the C
// interface's status codes.
enum class Status : std::int32_t {
    Ok = 0,
    InvalidArgument = 1,  // a setting or argument out of range
    BufferTooSmall = 2,   // storage, a payload or an output buffer too small
    Unsupported = 3,      // a combination the library does not implement
    State = 4,            // the call does not fit the object's current step
};

}  // namespace m0lte::ms110d
