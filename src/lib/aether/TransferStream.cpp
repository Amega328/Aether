/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "aether/TransferStream.h"

#include "aether/RandomAccessFile.h"
#include "base/Log.h"

#include <algorithm>
#include <cstring>

namespace fs = std::filesystem;

namespace aether::transfer {

namespace {

constexpr uint32_t kStreamMagic = 0x41465332; // "AFS2"
constexpr size_t kBufferBytes = 1024 * 1024;
constexpr uint32_t kMaxHeaderManifest = 64u * 1024 * 1024;
constexpr uint64_t kFreeSpaceMargin = 64ull * 1024 * 1024;

} // namespace

void writeStreamHeader(
    BulkChannel &channel, const Token &token, unsigned index, unsigned count, const std::string *manifestPayload
)
{
  std::string header;
  wire::putU32(header, kStreamMagic);
  wire::putU8(header, static_cast<uint8_t>(index));
  wire::putU8(header, static_cast<uint8_t>(count));
  header.append(reinterpret_cast<const char *>(token.data()), token.size());
  wire::putU32(header, manifestPayload ? static_cast<uint32_t>(manifestPayload->size()) : 0);
  if (manifestPayload) {
    header += *manifestPayload;
  }
  channel.write(header.data(), header.size());
}

StreamHeader readStreamHeader(BulkChannel &channel)
{
  constexpr size_t kFixedBytes = 4 + 1 + 1 + 16 + 4;
  char fixed[kFixedBytes];
  channel.readFully(fixed, sizeof(fixed));

  wire::Reader reader(std::string_view(fixed, sizeof(fixed)));
  if (reader.u32() != kStreamMagic) {
    throw TransferError("not a transfer stream");
  }

  StreamHeader header;
  header.index = reader.u8();
  header.count = reader.u8();
  if (header.count == 0 || header.count > kMaxStreams || header.index >= header.count) {
    throw TransferError("invalid transfer stream numbering");
  }

  const auto tokenBytes = reader.bytes(header.token.size());
  std::memcpy(header.token.data(), tokenBytes.data(), header.token.size());

  const uint32_t manifestLength = reader.u32();
  if (manifestLength > kMaxHeaderManifest) {
    throw TransferError("transfer manifest is too large");
  }
  if (manifestLength > 0) {
    std::string payload(manifestLength, '\0');
    channel.readFully(payload.data(), payload.size());
    header.manifest = decodeManifest(payload);
    if (header.manifest->token != header.token) {
      throw TransferError("transfer manifest does not match its token");
    }
  }
  return header;
}

void sendStream(
    BulkChannel &channel, const Source &source, unsigned index, unsigned count, bool includeManifest,
    std::atomic<uint64_t> *progress
)
{
  const auto &manifest = source.manifest;
  const auto units = streamUnits(layoutUnits(manifest), index, count);

  if (includeManifest) {
    const auto payload = encodeManifest(manifest);
    writeStreamHeader(channel, manifest.token, index, count, &payload);
  } else {
    writeStreamHeader(channel, manifest.token, index, count, nullptr);
  }

  std::vector<char> buffer(kBufferBytes);
  std::optional<RandomAccessFile> file;
  uint32_t openEntry = UINT32_MAX;

  for (const auto &unit : units) {
    if (unit.entry != openEntry) {
      file.reset();
      file.emplace(source.paths.at(unit.entry), RandomAccessFile::Mode::ReadSequential);
      openEntry = unit.entry;
    }

    for (uint64_t done = 0; done < unit.length;) {
      const auto chunk = static_cast<size_t>((std::min<uint64_t>)(buffer.size(), unit.length - done));
      if (file->readAt(unit.offset + done, buffer.data(), chunk) != chunk) {
        throw TransferError("file changed while sending: " + manifest.entries[unit.entry].name);
      }
      channel.write(buffer.data(), chunk);
      done += chunk;
      if (progress != nullptr) {
        progress->fetch_add(chunk, std::memory_order_relaxed);
      }
    }
  }
  file.reset();

  uint8_t answer = 0;
  channel.readFully(&answer, 1);
  if (answer != kAck) {
    throw TransferError("receiver reported a failed transfer");
  }
}

ReceiveSession::ReceiveSession(Manifest manifest, fs::path stagingDir)
    : m_manifest(std::move(manifest)),
      m_stagingDir(std::move(stagingDir)),
      m_units(layoutUnits(m_manifest)),
      m_total(m_manifest.totalBytes()),
      m_started(std::chrono::steady_clock::now())
{
}

ReceiveSession::~ReceiveSession() = default;

ReceiveSession::State ReceiveSession::state() const
{
  std::scoped_lock lock{m_mutex};
  return m_state;
}

std::string ReceiveSession::error() const
{
  std::scoped_lock lock{m_mutex};
  return m_error;
}

std::vector<fs::path> ReceiveSession::topLevelPaths() const
{
  std::vector<fs::path> paths;
  for (const auto &name : topLevelNames(m_manifest)) {
    paths.push_back(pathForEntry(m_stagingDir, name));
  }
  return paths;
}

void ReceiveSession::setObserver(std::function<void(const ReceiveSession &)> observer)
{
  std::scoped_lock lock{m_mutex};
  m_observer = std::move(observer);
}

void ReceiveSession::claimStream(unsigned index, unsigned count)
{
  std::scoped_lock lock{m_mutex};
  if (m_state == State::Failed) {
    throw TransferError(m_error);
  }
  if (count == 0 || count > kMaxStreams || index >= count) {
    throw TransferError("invalid transfer stream numbering");
  }
  if (m_streamCount == 0) {
    m_streamCount = count;
    m_claimed.assign(count, false);
  } else if (m_streamCount != count) {
    throw TransferError("transfer streams disagree on the stream count");
  }
  if (m_claimed[index]) {
    throw TransferError("transfer stream received twice");
  }
  m_claimed[index] = true;
}

void ReceiveSession::prepare()
{
  std::call_once(m_prepareOnce, [this] {
    try {
      std::error_code ec;
      fs::create_directories(m_stagingDir, ec);
      if (ec) {
        throw TransferError("cannot create " + m_stagingDir.string() + ": " + ec.message());
      }

      const auto space = fs::space(m_stagingDir, ec);
      if (!ec && space.available < m_total + kFreeSpaceMargin) {
        throw TransferError("not enough free disk space for the transfer");
      }

      for (const auto &entry : m_manifest.entries) {
        const auto target = pathForEntry(m_stagingDir, entry.name);
        if (entry.directory) {
          fs::create_directories(target, ec);
          if (ec) {
            throw TransferError("cannot create " + target.string() + ": " + ec.message());
          }
          continue;
        }

        fs::create_directories(target.parent_path(), ec);
        if (ec) {
          throw TransferError("cannot create " + target.parent_path().string() + ": " + ec.message());
        }
        RandomAccessFile file(target, RandomAccessFile::Mode::Write);
        file.resize(entry.size);
      }
    } catch (const std::exception &e) {
      m_prepareError = e.what();
    }
  });

  if (!m_prepareError.empty()) {
    throw TransferError(m_prepareError);
  }
}

void ReceiveSession::receive(BulkChannel &channel, unsigned index, unsigned count)
{
  claimStream(index, count);

  {
    std::scoped_lock lock{m_mutex};
    m_active.push_back(&channel);
  }
  const auto release = [this, &channel] {
    std::scoped_lock lock{m_mutex};
    m_active.erase(std::remove(m_active.begin(), m_active.end(), &channel), m_active.end());
  };

  try {
    prepare();

    const auto units = streamUnits(m_units, index, count);
    std::vector<char> buffer(kBufferBytes);
    std::optional<RandomAccessFile> file;
    uint32_t openEntry = UINT32_MAX;

    for (const auto &unit : units) {
      if (unit.entry != openEntry) {
        file.reset();
        file.emplace(pathForEntry(m_stagingDir, m_manifest.entries[unit.entry].name), RandomAccessFile::Mode::Write);
        openEntry = unit.entry;
      }

      for (uint64_t done = 0; done < unit.length;) {
        const auto chunk = static_cast<size_t>((std::min<uint64_t>)(buffer.size(), unit.length - done));
        channel.readFully(buffer.data(), chunk);
        file->writeAt(unit.offset + done, buffer.data(), chunk);
        done += chunk;
        m_received.fetch_add(chunk, std::memory_order_relaxed);
      }
    }
    file.reset();

    const uint8_t ack = kAck;
    channel.write(&ack, 1);
  } catch (const std::exception &e) {
    release();
    fail(e.what());
    throw;
  }

  release();
  streamFinished();
}

void ReceiveSession::fail(const std::string &reason)
{
  std::function<void(const ReceiveSession &)> observer;
  {
    std::scoped_lock lock{m_mutex};
    if (m_state != State::Receiving) {
      return;
    }
    m_state = State::Failed;
    m_error = reason;
    for (auto *channel : m_active) {
      channel->cancel();
    }
    observer = m_observer;
  }

  std::error_code ec;
  fs::remove_all(m_stagingDir, ec);
  if (observer) {
    observer(*this);
  }
}

void ReceiveSession::streamFinished()
{
  bool allDone = false;
  {
    std::scoped_lock lock{m_mutex};
    allDone = ++m_streamsDone == m_streamCount && m_state == State::Receiving;
  }
  if (allDone) {
    finish();
  }
}

void ReceiveSession::finish()
{
  if (received() != m_total) {
    fail("transfer ended before all data arrived");
    return;
  }

  // Children first, so setting a directory's time isn't undone by touching its contents.
  for (auto it = m_manifest.entries.rbegin(); it != m_manifest.entries.rend(); ++it) {
    if (it->modified <= 0) {
      continue;
    }
    std::error_code ec;
    fs::last_write_time(pathForEntry(m_stagingDir, it->name), fromUnixSeconds(it->modified), ec);
  }

  std::function<void(const ReceiveSession &)> observer;
  {
    std::scoped_lock lock{m_mutex};
    if (m_state != State::Receiving) {
      return;
    }
    m_state = State::Complete;
    observer = m_observer;
  }
  if (observer) {
    observer(*this);
  }
}

} // namespace aether::transfer
