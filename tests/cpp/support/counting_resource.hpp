#pragma once

#include <cstddef>
#include <memory_resource>

/// A `std::pmr::memory_resource` that counts allocation and deallocation
/// calls and bytes (ADR-0010, AEGIS-037). Not a pool of its own — every call
/// forwards to `upstream` and is simply counted, so the counts measure
/// exactly what reached this resource, with nothing hidden.
///
/// This is deliberately per-instance, not process-global (no `operator new`
/// override anywhere in the exchange layers): allocation is attributable to
/// the one book (or one test) that owns the resource, needs no test
/// ordering discipline, and behaves identically under `asan-ubsan`.
namespace aegis::exchange::testing {

class CountingResource final : public std::pmr::memory_resource {
 public:
  explicit CountingResource(std::pmr::memory_resource* upstream = std::pmr::get_default_resource())
      : upstream_(upstream) {}

  [[nodiscard]] std::size_t allocate_calls() const { return allocate_calls_; }
  [[nodiscard]] std::size_t deallocate_calls() const { return deallocate_calls_; }
  [[nodiscard]] std::size_t bytes_allocated() const { return bytes_allocated_; }
  [[nodiscard]] std::size_t bytes_deallocated() const { return bytes_deallocated_; }

  void reset_counts() {
    allocate_calls_ = 0;
    deallocate_calls_ = 0;
    bytes_allocated_ = 0;
    bytes_deallocated_ = 0;
  }

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    ++allocate_calls_;
    bytes_allocated_ += bytes;
    return upstream_->allocate(bytes, alignment);
  }

  void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override {
    ++deallocate_calls_;
    bytes_deallocated_ += bytes;
    upstream_->deallocate(ptr, bytes, alignment);
  }

  [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  std::pmr::memory_resource* upstream_;
  std::size_t allocate_calls_{0};
  std::size_t deallocate_calls_{0};
  std::size_t bytes_allocated_{0};
  std::size_t bytes_deallocated_{0};
};

}  // namespace aegis::exchange::testing
