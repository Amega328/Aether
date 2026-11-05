/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "net/BulkChannel.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace aether::testing {

namespace fs = std::filesystem;

class MemoryPipe
{
public:
  void write(const char *data, size_t n)
  {
    std::unique_lock lock{m_mutex};
    while (n > 0) {
      m_space.wait(lock, [this] { return m_closed || m_data.size() < kCapacity; });
      if (m_closed) {
        throw BulkChannelError("pipe closed");
      }
      const size_t take = std::min(n, kCapacity - m_data.size());
      m_data.append(data, take);
      data += take;
      n -= take;
      m_ready.notify_all();
    }
  }

  size_t read(char *out, size_t n)
  {
    std::unique_lock lock{m_mutex};
    m_ready.wait(lock, [this] { return m_closed || !m_data.empty(); });
    if (m_aborted) {
      throw BulkChannelError("pipe aborted");
    }
    if (m_data.empty()) {
      return 0;
    }
    const size_t take = std::min(n, m_data.size());
    std::memcpy(out, m_data.data(), take);
    m_data.erase(0, take);
    m_space.notify_all();
    return take;
  }

  void close()
  {
    std::scoped_lock lock{m_mutex};
    m_closed = true;
    m_ready.notify_all();
    m_space.notify_all();
  }

  void abort()
  {
    std::scoped_lock lock{m_mutex};
    m_closed = true;
    m_aborted = true;
    m_data.clear();
    m_ready.notify_all();
    m_space.notify_all();
  }

private:
  static constexpr size_t kCapacity = 1024 * 1024;

  std::mutex m_mutex;
  std::condition_variable m_ready;
  std::condition_variable m_space;
  std::string m_data;
  bool m_closed = false;
  bool m_aborted = false;
};

class PipeChannel final : public BulkChannel
{
public:
  PipeChannel(std::shared_ptr<MemoryPipe> in, std::shared_ptr<MemoryPipe> out)
      : m_in(std::move(in)),
        m_out(std::move(out))
  {
  }
  ~PipeChannel() override
  {
    m_out->close();
  }

  size_t read(void *buffer, size_t n) override
  {
    return m_in->read(static_cast<char *>(buffer), n);
  }
  void write(const void *buffer, size_t n) override
  {
    m_out->write(static_cast<const char *>(buffer), n);
  }
  void cancel() override
  {
    m_in->abort();
    m_out->abort();
  }

private:
  std::shared_ptr<MemoryPipe> m_in;
  std::shared_ptr<MemoryPipe> m_out;
};

using ChannelPair = std::pair<std::unique_ptr<BulkChannel>, std::unique_ptr<BulkChannel>>;

inline ChannelPair makePipePair()
{
  auto a = std::make_shared<MemoryPipe>();
  auto b = std::make_shared<MemoryPipe>();
  return {std::make_unique<PipeChannel>(a, b), std::make_unique<PipeChannel>(b, a)};
}

inline uint8_t patternByte(uint64_t index, uint32_t seed)
{
  return static_cast<uint8_t>(((index * 2654435761u) >> 24) ^ seed);
}

inline void writePatternFile(const fs::path &path, uint64_t size, uint32_t seed)
{
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  std::string block(1 << 20, '\0');
  for (uint64_t done = 0; done < size;) {
    const auto chunk = static_cast<size_t>(std::min<uint64_t>(block.size(), size - done));
    for (size_t i = 0; i < chunk; ++i) {
      block[i] = static_cast<char>(patternByte(done + i, seed));
    }
    file.write(block.data(), static_cast<std::streamsize>(chunk));
    done += chunk;
  }
}

inline bool filesEqual(const fs::path &a, const fs::path &b)
{
  std::ifstream fa(a, std::ios::binary);
  std::ifstream fb(b, std::ios::binary);
  if (!fa || !fb) {
    return false;
  }
  std::string ba(1 << 20, '\0');
  std::string bb(1 << 20, '\0');
  while (true) {
    fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
    fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
    if (fa.gcount() != fb.gcount() || std::memcmp(ba.data(), bb.data(), static_cast<size_t>(fa.gcount())) != 0) {
      return false;
    }
    if (fa.gcount() == 0) {
      return true;
    }
  }
}

} // namespace aether::testing
