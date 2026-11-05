/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "aether/TransferStream.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace aether {
class IStream;
}

namespace aether::transfer {

//! Direction of a data connection, named after what the dialing client does.
enum class Role
{
  Pull, //!< client receives files the server holds
  Push  //!< client sends files to the server
};

enum class Side
{
  Client, //!< dialed the connection
  Server  //!< accepted it
};

//! One data connection of a transfer, as announced in the hello-back name
struct StreamRequest
{
  Role role = Role::Pull;
  Token token{};
  unsigned index = 0;
  unsigned count = 1;
};

//! "aetherxfer:<pull|push>:<token>:<index>:<count>"
std::string makeHelloName(const StreamRequest &request);
std::optional<StreamRequest> parseHelloName(std::string_view name);

struct Progress
{
  ReceiveSession::State state = ReceiveSession::State::Receiving;
  uint64_t received = 0;
  uint64_t total = 0;
  std::vector<std::filesystem::path> paths; //!< top-level files and folders once Complete
  std::filesystem::path directory;          //!< where the files are being staged
  std::string error;
};

//! Process-wide coordinator of large file transfers
/*!
Holds the files this machine has offered, the transfers it is receiving and
the worker threads moving their bytes. Connections are dialed and accepted
elsewhere (the client's TransferConnector, the server's ClientProxyUnknown);
this class only needs the resulting BulkChannel.

All methods are thread safe. The dialer and the listener run on whichever
thread calls into the hub or finishes a transfer, and must not block.
*/
class TransferHub
{
public:
  //! Asked to open one data connection; the client wires this to its connector.
  using Dialer = std::function<void(const StreamRequest &)>;
  //! Told when a transfer completes or fails.
  using Listener = std::function<void(const Token &)>;

  explicit TransferHub(std::filesystem::path stagingRoot);
  TransferHub(const TransferHub &) = delete;
  TransferHub &operator=(const TransferHub &) = delete;
  ~TransferHub();

  static TransferHub &instance();
  static std::filesystem::path defaultStagingRoot();

  void setDialer(Dialer dialer);
  void setListener(Listener listener);

  //! Clipboard payload if \c paths are exactly what a finished transfer staged.
  std::optional<std::string> recognizeStaged(const std::vector<std::filesystem::path> &paths) const;

  //! Make \c source available to peers and return its manifest payload.
  std::string offer(Source source);

  //! The manifest was sent to the server; push the bytes (client only).
  void announceSent(const Manifest &manifest);

  //! A manifest arrived from the peer: start (or look up) receiving it.
  Progress request(const Manifest &manifest);
  std::optional<Progress> status(const Token &token) const;

  //! Run one data connection. Takes ownership of the channel.
  void serve(std::unique_ptr<BulkChannel> channel, const StreamRequest &request, Side side);

  //! A data connection could not be opened; a pull it belonged to fails so a later request retries.
  void streamFailed(const StreamRequest &request, const std::string &reason);

  //! Take the socket under a finished handshake (a StreamFilter over an IDataSocket) and serve it.
  /*!
  Call from the event loop thread once the handshake has consumed all input.
  The stream and socket objects stay with the caller, emptied; delete them as usual.
  */
  bool serveConnection(IStream *stream, const StreamRequest &request, Side side);

  //! Cancel everything and wait for the workers. The hub stays usable afterwards.
  void shutdown();

private:
  struct Worker
  {
    std::thread thread;
    std::atomic<bool> done = false;
  };

  std::shared_ptr<const Source> findSource(const Token &token) const;
  std::shared_ptr<ReceiveSession> findSession(const Token &token) const;
  std::shared_ptr<ReceiveSession> createSessionLocked(const Manifest &manifest);
  std::shared_ptr<ReceiveSession> sessionForIncoming(const StreamHeader &header);
  Progress progressOf(const ReceiveSession &session) const;
  std::vector<std::shared_ptr<ReceiveSession>> trimSessionsLocked();
  void evict(const std::vector<std::shared_ptr<ReceiveSession>> &sessions);
  void onSessionFinished(const ReceiveSession &session);
  void runStream(std::shared_ptr<BulkChannel> channel, StreamRequest request, Side side);
  void spawn(std::function<void()> job);
  void reapWorkers();

  static constexpr size_t kMaxSources = 8;
  static constexpr size_t kMaxSessions = 2;
  static constexpr auto kSessionWait = std::chrono::seconds(15);

  const std::filesystem::path m_stagingRoot;

  mutable std::mutex m_mutex;
  std::condition_variable m_sessionCreated;
  std::list<std::shared_ptr<const Source>> m_sources;
  std::list<std::shared_ptr<ReceiveSession>> m_sessions;
  std::set<BulkChannel *> m_channels;
  std::list<std::unique_ptr<Worker>> m_workers;
  Dialer m_dialer;
  bool m_stopping = false;
  uint32_t m_sessionCounter = 0;

  std::mutex m_listenerMutex;
  Listener m_listener;
};

} // namespace aether::transfer
