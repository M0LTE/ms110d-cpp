#include "alloc_guard.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<std::size_t> g_allocations{0};

void* allocate(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(size != 0 ? size : 1);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void* allocate_aligned(std::size_t size, std::size_t alignment) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    // aligned_alloc wants a size that is a multiple of the alignment.
    const std::size_t rounded = ((size != 0 ? size : 1) + alignment - 1) / alignment * alignment;
    void* p = std::aligned_alloc(alignment, rounded);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

}  // namespace

namespace ms110d_test {
std::size_t allocation_count() noexcept { return g_allocations.load(std::memory_order_relaxed); }
}  // namespace ms110d_test

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, std::align_val_t a) { return allocate_aligned(size, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t size, std::align_val_t a) { return allocate_aligned(size, static_cast<std::size_t>(a)); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
