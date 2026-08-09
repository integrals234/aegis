#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Shared little-endian primitives for canonical encoding (ADR-0002, ADR-0009).
///
/// `cpp/events/envelope.cpp` hand-rolls the same pattern for the platform
/// envelope. This header exists so every M1 wire format built on top of it —
/// exchange commands and events now, the journal/event-log/snapshot codecs in
/// `cpp/exchange/state` later — shares one implementation instead of five
/// copies that could individually drift from "fixed-width little-endian,
/// length-prefixed strings, no floating point" (envelope.hpp's canonical
/// rules).
namespace aegis::events::wire {

void put_u8(std::vector<std::byte>& out, std::uint8_t value);
void put_u16(std::vector<std::byte>& out, std::uint16_t value);
void put_u32(std::vector<std::byte>& out, std::uint32_t value);
void put_u64(std::vector<std::byte>& out, std::uint64_t value);
/// Reinterpreted as unsigned for encoding, so the byte pattern is defined for
/// negative values too (mirrors envelope.cpp's handling of EventTime).
void put_i64(std::vector<std::byte>& out, std::int64_t value);
void put_string(std::vector<std::byte>& out, std::string_view text);
void put_bytes(std::vector<std::byte>& out, std::span<const std::byte> bytes);

/// Every `take_*` returns false, leaving `offset` unspecified, when the buffer
/// is too short. Callers turn that into one truncation error rather than
/// reading past the end.
[[nodiscard]] bool take_u8(std::span<const std::byte> bytes, std::size_t& offset,
                           std::uint8_t& out);
[[nodiscard]] bool take_u16(std::span<const std::byte> bytes, std::size_t& offset,
                            std::uint16_t& out);
[[nodiscard]] bool take_u32(std::span<const std::byte> bytes, std::size_t& offset,
                            std::uint32_t& out);
[[nodiscard]] bool take_u64(std::span<const std::byte> bytes, std::size_t& offset,
                            std::uint64_t& out);
[[nodiscard]] bool take_i64(std::span<const std::byte> bytes, std::size_t& offset,
                            std::int64_t& out);
[[nodiscard]] bool take_string(std::span<const std::byte> bytes, std::size_t& offset,
                               std::string& out);

}  // namespace aegis::events::wire
