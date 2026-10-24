#include <m0lte/ms110d/config.hpp>
#include <m0lte/ms110d/version.hpp>

#include <cstdio>
#include <cstring>

int main() {
    const auto v = m0lte::ms110d::version();
    const m0lte::ms110d::real one = 1;
    char text[64];
    std::snprintf(text, sizeof text, "%d.%d.%d", v.major, v.minor, v.patch);
    std::printf("ms110d %s (sizeof real %zu)\n", text, sizeof(one));
    return std::strcmp(text, EXPECTED_VERSION) == 0 ? 0 : 1;
}
