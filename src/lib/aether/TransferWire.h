/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aether::transfer {

class TransferError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

namespace wire {

inline void putU8(std::string &out, uint8_t v)
{
  out += static_cast<char>(v);
}

inline void putU16(std::string &out, uint16_t v)
{
  out += static_cast<char>((v >> 8) & 0xff);
  out += static_cast<char>(v & 0xff);
}

inline void putU32(std::string &out, uint32_t v)
{
  putU16(out, static_cast<uint16_t>(v >> 16));
  putU16(out, static_cast<uint16_t>(v & 0xffff));
}

inline void putU64(std::string &out, uint64_t v)
{
  putU32(out, static_cast<uint32_t>(v >> 32));
  putU32(out, static_cast<uint32_t>(v & 0xffffffff));
}

//! Bounds-checked big-endian reader over a byte view
class Reader
{
public:
  explicit Reader(std::string_view data) : m_data(data)
  {
  }

  size_t remaining() const
  {
    return m_data.size();
  }

  uint8_t u8()
  {
    need(1);
    const auto v = static_cast<uint8_t>(m_data.front());
    m_data.remove_prefix(1);
    return v;
  }

  uint16_t u16()
  {
    const uint16_t hi = u8();
    return static_cast<uint16_t>((hi << 8) | u8());
  }

  uint32_t u32()
  {
    const uint32_t hi = u16();
    return (hi << 16) | u16();
  }

  uint64_t u64()
  {
    const uint64_t hi = u32();
    return (hi << 32) | u32();
  }

  std::string_view bytes(size_t n)
  {
    need(n);
    const auto v = m_data.substr(0, n);
    m_data.remove_prefix(n);
    return v;
  }

private:
  void need(size_t n) const
  {
    if (m_data.size() < n) {
      throw TransferError("truncated transfer data");
    }
  }

  std::string_view m_data;
};

} // namespace wire
} // namespace aether::transfer
