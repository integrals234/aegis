#include "cpp/events/wire.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace aegis::events::wire {
namespace {

constexpr std::size_t kMaxStringLength = std::numeric_limits<std::uint16_t>::max();

template <typename T>
void put_unsigned(std::vector<std::byte>& out, T value) {
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    out.push_back(static_cast<std::byte>((value >> (8U * index)) & 0xFFU));
  }
}

template <typename T>
[[nodiscard]] bool take_unsigned(std::span<const std::byte> bytes, std::size_t& offset, T& out) {
  if (offset + sizeof(T) > bytes.size()) {
    return false;
  }
  T value{0};
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    value |= static_cast<T>(std::to_integer<std::uint64_t>(bytes[offset + index]) << (8U * index));
  }
  out = value;
  offset += sizeof(T);
  return true;
}

}  // namespace

void put_u8(std::vector<std::byte>& out, std::uint8_t value) {
  put_unsigned<std::uint8_t>(out, value);
}
void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
  put_unsigned<std::uint16_t>(out, value);
}
void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
  put_unsigned<std::uint32_t>(out, value);
}
void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
  put_unsigned<std::uint64_t>(out, value);
}
void put_i64(std::vector<std::byte>& out, std::int64_t value) {
  put_unsigned<std::uint64_t>(out, static_cast<std::uint64_t>(value));
}

void put_string(std::vector<std::byte>& out, std::string_view text) {
  if (text.size() > kMaxStringLength) {
    throw std::length_error("wire string field is " + std::to_string(text.size()) +
                            " bytes, over the " + std::to_string(kMaxStringLength) +
                            "-byte limit imposed by its 16-bit length prefix");
  }
  put_u16(out, static_cast<std::uint16_t>(text.size()));
  for (const char character : text) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
}

void put_bytes(std::vector<std::byte>& out, std::span<const std::byte> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

bool take_u8(std::span<const std::byte> bytes, std::size_t& offset, std::uint8_t& out) {
  return take_unsigned<std::uint8_t>(bytes, offset, out);
}
bool take_u16(std::span<const std::byte> bytes, std::size_t& offset, std::uint16_t& out) {
  return take_unsigned<std::uint16_t>(bytes, offset, out);
}
bool take_u32(std::span<const std::byte> bytes, std::size_t& offset, std::uint32_t& out) {
  return take_unsigned<std::uint32_t>(bytes, offset, out);
}
bool take_u64(std::span<const std::byte> bytes, std::size_t& offset, std::uint64_t& out) {
  return take_unsigned<std::uint64_t>(bytes, offset, out);
}
bool take_i64(std::span<const std::byte> bytes, std::size_t& offset, std::int64_t& out) {
  std::uint64_t raw{0};
  if (!take_unsigned<std::uint64_t>(bytes, offset, raw)) {
    return false;
  }
  out = static_cast<std::int64_t>(raw);
  return true;
}

bool take_string(std::span<const std::byte> bytes, std::size_t& offset, std::string& out) {
  std::uint16_t length{0};
  if (!take_u16(bytes, offset, length)) {
    return false;
  }
  if (offset + length > bytes.size()) {
    return false;
  }
  out.assign(length, '\0');
  for (std::size_t index = 0; index < length; ++index) {
    out[index] = static_cast<char>(std::to_integer<unsigned char>(bytes[offset + index]));
  }
  offset += length;
  return true;
}

}  // namespace aegis::events::wire
