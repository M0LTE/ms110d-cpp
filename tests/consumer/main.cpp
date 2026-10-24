#include <m0lte/ms110d/config.hpp>
#include <m0lte/ms110d/mode.hpp>
#include <m0lte/ms110d/version.hpp>

#include <cstdio>
#include <cstring>

int main() {
    const auto v = m0lte::ms110d::version();
    const m0lte::ms110d::real one = 1;
    char text[64];
    std::snprintf(text, sizeof text, "%d.%d.%d", v.major, v.minor, v.patch);
    std::printf("ms110d %s (sizeof real %zu)\n", text, sizeof(one));
    // The mode tables must be reachable through the installed library.
    const auto mode = m0lte::ms110d::mode_3k(6);
    const auto il = m0lte::ms110d::interleaver_3k(6, m0lte::ms110d::InterleaverKind::Short);
    if (!mode || !il || mode->bps != 3200 || il->size_bits != 2048) return 2;
    return std::strcmp(text, EXPECTED_VERSION) == 0 ? 0 : 1;
}
