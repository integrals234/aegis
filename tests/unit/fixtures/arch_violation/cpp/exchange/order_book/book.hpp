#pragma once
namespace aegis::exchange {
class Book {};
}  // namespace aegis::exchange

// Violation 6: hidden mutable global state in a deterministic core.
int g_last_sequence = 0;
