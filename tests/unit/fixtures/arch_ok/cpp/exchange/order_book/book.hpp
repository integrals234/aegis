#pragma once
#include "cpp/common/clock.hpp"
#include "cpp/events/envelope.hpp"
namespace aegis::exchange {
class Book {
 public:
  explicit Book(const aegis::common::Clock& clock);
};
}  // namespace aegis::exchange
