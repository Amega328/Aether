/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "aether/TransferManifest.h"
#include "net/BulkChannel.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

//! Data connections of a large file transfer
/*!
Each connection carries one stream, always flowing from sender to receiver:

  u32 magic, u8 stream index, u8 stream count, 16 token bytes,
  u32 manifest length, manifest bytes (stream 0 only, otherwise empty)
  ... the raw bytes of the stream's units, see TransferManifest.h ...

and the receiver answers with a single acknowledgement byte once everything
has been written to disk.
*/
namespace aether::transfer {

constexpr uint8_t kAck = 0x06;
constexpr uint8_t kNak = 0x15;

struct StreamHeader
{
  Token token{};
  unsigned index = 0;
  unsigned count = 1;
  std::optional<Manifest> manifest;
};

//! Send the header; \c manifestPayload is attached when non-null.
void writeStreamHeader(
    BulkChannel &channel, const Token &token, unsigned index, unsigned count, const std::string *manifestPayload
);
StreamHeader readStreamHeader(BulkChannel &channel);

//! Send one stream of \c source and wait for the receiver's acknowledgement.
/*!
Throws TransferError or BulkChannelError on failure, including when a file
shrinks or disappears while being sent. \c progress, if given, is advanced as
bytes are written.
*/
void sendStream(
    BulkChannel &channel, const Source &source, unsigned index, unsigned count, bool includeManifest,
    std::atomic<uint64_t> *progress = nullptr
);

//! Files being received into a staging directory
/*!
Shared by all streams of one transfer. Streams may arrive in any order and run
on separate threads; the session completes once every stream has delivered
its bytes, at which point file times are restored.
*/
class ReceiveSession
{
public:
  enum class State
  {
    Receiving,
    Complete,
    Failed
  };

  ReceiveSession(Manifest manifest, std::filesystem::path stagingDir);
  ReceiveSession(const ReceiveSession &) = delete;
  ReceiveSession &operator=(const ReceiveSession &) = delete;
  ~ReceiveSession();

  const Manifest &manifest() const
  {
    return m_manifest;
  }
  const std::filesystem::path &stagingDir() const
  {
    return m_stagingDir;
  }

  State state() const;
  std::string error() const;
  uint64_t received() const
  {
    return m_received.load(std::memory_order_relaxed);
  }
  uint64_t total() const
  {
    return m_total;
  }
  std::chrono::steady_clock::time_point started() const
  {
    return m_started;
  }

  //! Staged top-level files and folders, valid once Complete.
  std::vector<std::filesystem::path> topLevelPaths() const;

  //! Called on whichever thread changes the state to Complete or Failed.
  void setObserver(std::function<void(const ReceiveSession &)> observer);

  //! Receive stream \c index of \c count whose header was already read.
  void receive(BulkChannel &channel, unsigned index, unsigned count);

  //! Mark the transfer failed and abort every stream still running.
  void fail(const std::string &reason);

private:
  void claimStream(unsigned index, unsigned count);
  void prepare();
  void streamFinished();
  void finish();

  const Manifest m_manifest;
  const std::filesystem::path m_stagingDir;
  const std::vector<Unit> m_units;
  const uint64_t m_total;
  const std::chrono::steady_clock::time_point m_started;
  std::atomic<uint64_t> m_received = 0;

  mutable std::mutex m_mutex;
  State m_state = State::Receiving;
  std::string m_error;
  unsigned m_streamCount = 0;
  unsigned m_streamsDone = 0;
  std::vector<bool> m_claimed;
  std::vector<BulkChannel *> m_active;
  std::function<void(const ReceiveSession &)> m_observer;

  std::once_flag m_prepareOnce;
  std::string m_prepareError;
};

} // namespace aether::transfer
