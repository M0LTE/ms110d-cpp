#include <m0lte/ms110d/config.hpp>
#include <m0lte/ms110d/version.hpp>

#include <cstdio>

int main() {
    const auto v = m0lte::ms110d::version();
    const m0lte::ms110d::real one = 1;
    std::printf("ms110d %d.%d.%d (sizeof real %zu)\n", v.major, v.minor, v.patch, sizeof(one));
    return (v.major == 0 && v.minor == 0 && v.patch == 0) ? 0 : 1;
}
