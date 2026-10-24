#include <m0lte/ms110d/version.hpp>

namespace m0lte::ms110d {

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif
Version version() noexcept {
    return library_version;
}
}

}  // namespace m0lte::ms110d
