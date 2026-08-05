#pragma once
// Violation 1: a strategy reaching straight into the exchange book.
#include "cpp/exchange/order_book/book.hpp"
// Violation 2: a strategy holding a gateway, bypassing risk and the OMS.
#include "cpp/participant/oms/gateway.hpp"
// Violation 3: a parent-relative include routing around the DAG.
#include "../../common/clock.hpp"
// Violation 4: Python headers outside the bindings layer.
#include <pybind11/pybind11.h>

// Violation 5: opening a namespace this layer does not own.
namespace aegis::exchange {
class Rogue {};
}  // namespace aegis::exchange
