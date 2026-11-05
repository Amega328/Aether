/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "aether/TransferHub.h"

#include "aether/ProtocolTypes.h"
#include "base/Log.h"
#include "io/StreamFilter.h"
#include "net/IDataSocket.h"

#include <QStandardPaths>
#include <QString>

#include <algorithm>
#include <cstring>

namespace fs = std::filesystem;

namespace aether::transfer {

namespace {

fs::path toPath(const QString &text)
{
#if defined(_WIN32)
  return fs::path(text.toStdWString());
#else
  return fs::path(text.toStdString());
#endif
}

std::set<std::string> normalized(const std::vector<fs::path> &paths)
{
  std::set<std::string> result;
  for (const auto &path : paths) {
    result.insert(path.lexically_normal().generic_string());
  }
  return result;
}

std::optional<unsigned> parseNumber(std::string_view text)
{
  if (text.empty() || text.size() > 3) {
    return std::nullopt;
  }
  unsigned value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * 10 + static_cast<unsigned>(c - '0');
  }
  return value;
}

void removeTrees(const std::vector<fs::path> &directories)
{
  for (const auto &directory : directories) {
    std::error_code ec;
    fs::remove_all(directory, ec);
  }
}

} // namespace

std::string makeHelloName(const StreamRequest &request)
{
  return std::string(kTransferNamePrefix) + (request.role == Role::Pull ? "pull" : "push") + ":" +
         tokenToHex(request.token) + ":" + std::to_string(request.index) + ":" + std::to_string(request.count);
}

std::optional<StreamRequest> parseHelloName(std::string_view name)
{
  const std::string_view prefix(kTransferNamePrefix);
  if (name.substr(0, prefix.size()) != prefix) {
    return std::nullopt;
  }
  name.remove_prefix(prefix.size());

  std::vector<std::string_view> parts;
  while (true) {
    const auto colon = name.find(':');
    parts.push_back(name.substr(0, colon));
    if (colon == std::string_view::npos) {
      break;
    }
    name.remove_prefix(colon + 1);
  }
  if (parts.size() != 4) {
    return std::nullopt;
  }

  StreamRequest request;
  if (parts[0] == "pull") {
    request.role = Role::Pull;
  } else if (parts[0] == "push") {
    request.role = Role::Push;
  } else {
    return std::nullopt;
  }

  const auto token = tokenFromHex(parts[1]);
  const auto index = parseNumber(parts[2]);
  const auto count = parseNumber(parts[3]);
  if (!token || !index || !count || *count == 0 || *count > kMaxStreams || *index >= *count) {
    return std::nullopt;
  }
  request.token = *token;
  request.index = *index;
  request.count = *count;
  return request;
}

TransferHub::TransferHub(fs::path stagingRoot) : m_stagingRoot(std::move(stagingRoot))
{
}

TransferHub::~TransferHub()
{
  shutdown();
}

fs::path TransferHub::defaultStagingRoot()
{
  const auto cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
  if (cache.isEmpty()) {
    return fs::temp_directory_path() / "AetherTransfer";
  }
  return toPath(cache) / "Aether" / "transfer";
}

TransferHub &TransferHub::instance()
{
  static TransferHub hub = [] {
    // Staged files belong to the process that received them; anything left
    // over is from a previous run.
    const auto root = defaultStagingRoot();
    std::error_code ec;
    fs::remove_all(root, ec);
    return TransferHub(root);
  }();
  return hub;
}

void TransferHub::setDialer(Dialer dialer)
{
  std::scoped_lock lock{m_mutex};
  m_dialer = std::move(dialer);
}

void TransferHub::setListener(Listener listener)
{
  std::scoped_lock lock{m_listenerMutex};
  m_listener = std::move(listener);
}

std::shared_ptr<const Source> TransferHub::findSource(const Token &token) const
{
  std::shared_ptr<ReceiveSession> relay;
  {
    std::scoped_lock lock{m_mutex};
    for (const auto &source : m_sources) {
      if (source->manifest.token == token) {
        return source;
      }
    }
    for (const auto &session : m_sessions) {
      if (session->manifest().token == token && session->state() == ReceiveSession::State::Complete) {
        relay = session;
        break;
      }
    }
  }
  if (!relay) {
    return nullptr;
  }

  // Files received from one peer can be passed on to another.
  auto source = std::make_shared<Source>();
  source->manifest = relay->manifest();
  for (const auto &entry : source->manifest.entries) {
    source->paths.push_back(pathForEntry(relay->stagingDir(), entry.name));
  }
  return source;
}

std::shared_ptr<ReceiveSession> TransferHub::findSession(const Token &token) const
{
  std::scoped_lock lock{m_mutex};
  for (const auto &session : m_sessions) {
    if (session->manifest().token == token) {
      return session;
    }
  }
  return nullptr;
}

std::optional<std::string> TransferHub::recognizeStaged(const std::vector<fs::path> &paths) const
{
  const auto wanted = normalized(paths);
  std::scoped_lock lock{m_mutex};
  for (const auto &session : m_sessions) {
    if (session->state() == ReceiveSession::State::Complete && normalized(session->topLevelPaths()) == wanted) {
      return encodeManifest(session->manifest());
    }
  }
  return std::nullopt;
}

std::string TransferHub::offer(Source source)
{
  auto payload = encodeManifest(source.manifest);
  auto shared = std::make_shared<const Source>(std::move(source));

  std::scoped_lock lock{m_mutex};
  m_sources.remove_if([&](const auto &existing) { return existing->manifest.token == shared->manifest.token; });
  m_sources.push_front(std::move(shared));
  while (m_sources.size() > kMaxSources) {
    m_sources.pop_back();
  }
  return payload;
}

void TransferHub::announceSent(const Manifest &manifest)
{
  if (!findSource(manifest.token)) {
    return;
  }

  Dialer dialer;
  {
    std::scoped_lock lock{m_mutex};
    dialer = m_dialer;
  }
  if (!dialer) {
    return;
  }

  const unsigned count = plannedStreams(manifest);
  LOG_INFO(
      "sending %zu file entries (%llu bytes) to the server on %u stream(s)", manifest.entries.size(),
      static_cast<unsigned long long>(manifest.totalBytes()), count
  );
  for (unsigned index = 0; index < count; ++index) {
    dialer({Role::Push, manifest.token, index, count});
  }
}

Progress TransferHub::progressOf(const ReceiveSession &session) const
{
  Progress progress;
  progress.state = session.state();
  progress.received = session.received();
  progress.total = session.total();
  progress.directory = session.stagingDir();
  if (progress.state == ReceiveSession::State::Complete) {
    progress.paths = session.topLevelPaths();
  } else if (progress.state == ReceiveSession::State::Failed) {
    progress.error = session.error();
  }
  return progress;
}

std::optional<Progress> TransferHub::status(const Token &token) const
{
  if (const auto session = findSession(token)) {
    return progressOf(*session);
  }
  if (const auto source = findSource(token)) {
    Progress progress;
    progress.state = ReceiveSession::State::Complete;
    progress.total = progress.received = source->manifest.totalBytes();
    for (size_t i = 0; i < source->manifest.entries.size(); ++i) {
      if (source->manifest.entries[i].name.find('/') == std::string::npos) {
        progress.paths.push_back(source->paths[i]);
      }
    }
    return progress;
  }
  return std::nullopt;
}

std::shared_ptr<ReceiveSession> TransferHub::createSessionLocked(const Manifest &manifest)
{
  // One directory per attempt: a retry must never share files with a failed or
  // evicted predecessor that is still being cleaned up.
  const auto directory = m_stagingRoot / (tokenToHex(manifest.token) + "-" + std::to_string(++m_sessionCounter));
  auto session = std::make_shared<ReceiveSession>(manifest, directory);
  session->setObserver([this](const ReceiveSession &finished) { onSessionFinished(finished); });
  m_sessions.push_front(session);
  m_sessionCreated.notify_all();
  return session;
}

std::vector<std::shared_ptr<ReceiveSession>> TransferHub::trimSessionsLocked()
{
  std::vector<std::shared_ptr<ReceiveSession>> evicted;
  while (m_sessions.size() > kMaxSessions) {
    evicted.push_back(m_sessions.back());
    m_sessions.pop_back();
  }
  return evicted;
}

void TransferHub::evict(const std::vector<std::shared_ptr<ReceiveSession>> &sessions)
{
  // Runs without the lock: failing a session aborts its streams and deleting
  // a staged tree can take a while.
  std::vector<fs::path> doomed;
  for (const auto &session : sessions) {
    switch (session->state()) {
    case ReceiveSession::State::Receiving:
      session->fail("superseded by a newer transfer");
      break;
    case ReceiveSession::State::Complete:
      doomed.push_back(session->stagingDir());
      break;
    case ReceiveSession::State::Failed:
      break; // already removed its files
    }
  }
  if (!doomed.empty()) {
    spawn([doomed = std::move(doomed)] { removeTrees(doomed); });
  }
}

Progress TransferHub::request(const Manifest &manifest)
{
  if (const auto local = status(manifest.token); local && !findSession(manifest.token)) {
    return *local;
  }

  std::shared_ptr<ReceiveSession> session;
  std::vector<std::shared_ptr<ReceiveSession>> evicted;
  Dialer dialer;
  bool created = false;
  {
    std::scoped_lock lock{m_mutex};
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
      if ((*it)->manifest().token != manifest.token) {
        continue;
      }
      if ((*it)->state() == ReceiveSession::State::Failed) {
        evicted.push_back(*it);
        m_sessions.erase(it);
      } else {
        session = *it;
      }
      break;
    }

    if (!session) {
      session = createSessionLocked(manifest);
      created = true;
      dialer = m_dialer;
    }

    auto trimmed = trimSessionsLocked();
    evicted.insert(evicted.end(), trimmed.begin(), trimmed.end());
  }
  evict(evicted);

  if (created) {
    const unsigned count = plannedStreams(manifest);
    LOG_INFO(
        "receiving %zu file entries (%llu bytes) on %u stream(s)", manifest.entries.size(),
        static_cast<unsigned long long>(manifest.totalBytes()), count
    );
    if (dialer) {
      for (unsigned index = 0; index < count; ++index) {
        dialer({Role::Pull, manifest.token, index, count});
      }
    }
  }
  return progressOf(*session);
}

std::shared_ptr<ReceiveSession> TransferHub::sessionForIncoming(const StreamHeader &header)
{
  const auto find = [&]() -> std::shared_ptr<ReceiveSession> {
    for (const auto &session : m_sessions) {
      if (session->manifest().token == header.token) {
        return session;
      }
    }
    return nullptr;
  };

  std::unique_lock lock{m_mutex};
  if (auto session = find()) {
    return session;
  }
  if (header.manifest) {
    auto session = createSessionLocked(*header.manifest);
    LOG_INFO(
        "receiving %zu file entries (%llu bytes) from the client on %u stream(s)", header.manifest->entries.size(),
        static_cast<unsigned long long>(header.manifest->totalBytes()), header.count
    );

    const auto evicted = trimSessionsLocked();
    lock.unlock();
    evict(evicted);
    return session;
  }

  m_sessionCreated.wait_for(lock, kSessionWait, [&] { return m_stopping || find() != nullptr; });
  return find();
}

void TransferHub::onSessionFinished(const ReceiveSession &session)
{
  if (session.state() == ReceiveSession::State::Complete) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - session.started()).count();
    const double mebibytes = static_cast<double>(session.total()) / (1024.0 * 1024.0);
    LOG_INFO(
        "file transfer complete: %zu entries, %.1f MiB in %.1f s (%.0f MiB/s)", session.manifest().entries.size(),
        mebibytes, seconds, seconds > 0 ? mebibytes / seconds : 0.0
    );
  } else {
    LOG_WARN("file transfer failed: %s", session.error().c_str());
  }

  std::scoped_lock lock{m_listenerMutex};
  if (m_listener) {
    m_listener(session.manifest().token);
  }
}

void TransferHub::serve(std::unique_ptr<BulkChannel> channel, const StreamRequest &request, Side side)
{
  std::shared_ptr<BulkChannel> shared = std::move(channel);
  spawn([this, shared = std::move(shared), request, side] { runStream(shared, request, side); });
}

void TransferHub::streamFailed(const StreamRequest &request, const std::string &reason)
{
  if (request.role != Role::Pull) {
    return;
  }
  if (const auto session = findSession(request.token)) {
    session->fail("data connection failed: " + reason);
  }
}

bool TransferHub::serveConnection(IStream *stream, const StreamRequest &request, Side side)
{
  const auto *filter = dynamic_cast<StreamFilter *>(stream);
  auto *socket = dynamic_cast<IDataSocket *>(filter != nullptr ? filter->getStream() : stream);
  auto channel = socket != nullptr ? socket->detachBulkChannel() : nullptr;
  if (!channel) {
    return false;
  }
  serve(std::move(channel), request, side);
  return true;
}

void TransferHub::runStream(std::shared_ptr<BulkChannel> channel, StreamRequest request, Side side)
{
  BulkChannel *raw = channel.get();
  {
    std::scoped_lock lock{m_mutex};
    if (m_stopping) {
      return;
    }
    m_channels.insert(raw);
  }

  const bool sending = (request.role == Role::Push) == (side == Side::Client);
  try {
    // The server's handshake layers may have buffered bytes the client sent right
    // after its hello, so a pushing client holds back until the server says go.
    if (request.role == Role::Push) {
      if (side == Side::Server) {
        const uint8_t go = kAck;
        channel->write(&go, 1);
      } else {
        uint8_t go = 0;
        channel->readFully(&go, 1);
        if (go != kAck) {
          throw TransferError("server refused the transfer");
        }
      }
    }

    if (sending) {
      const auto source = findSource(request.token);
      if (!source) {
        throw TransferError("no files are offered for this transfer");
      }
      sendStream(*channel, *source, request.index, request.count, request.role == Role::Push && request.index == 0);
    } else {
      const auto header = readStreamHeader(*channel);
      if (header.token != request.token || header.index != request.index || header.count != request.count) {
        throw TransferError("transfer stream does not match its request");
      }
      const auto session = side == Side::Client ? findSession(request.token) : sessionForIncoming(header);
      if (!session) {
        throw TransferError("no transfer is waiting for this stream");
      }
      session->receive(*channel, header.index, header.count);
    }
  } catch (const std::exception &e) {
    LOG_WARN("file transfer stream %u of %u failed: %s", request.index + 1, request.count, e.what());
    if (!sending) {
      if (const auto session = findSession(request.token)) {
        session->fail(e.what());
      }
    }
  }

  std::scoped_lock lock{m_mutex};
  m_channels.erase(raw);
}

void TransferHub::spawn(std::function<void()> job)
{
  std::scoped_lock lock{m_mutex};
  reapWorkers();

  auto worker = std::make_unique<Worker>();
  auto *raw = worker.get();
  worker->thread = std::thread([raw, job = std::move(job)] {
    job();
    raw->done = true;
  });
  m_workers.push_back(std::move(worker));
}

void TransferHub::reapWorkers()
{
  for (auto it = m_workers.begin(); it != m_workers.end();) {
    if ((*it)->done) {
      (*it)->thread.join();
      it = m_workers.erase(it);
    } else {
      ++it;
    }
  }
}

void TransferHub::shutdown()
{
  std::list<std::unique_ptr<Worker>> workers;
  std::vector<std::shared_ptr<ReceiveSession>> sessions;
  {
    std::scoped_lock lock{m_mutex};
    m_stopping = true;
    for (auto *channel : m_channels) {
      channel->cancel();
    }
    sessions.assign(m_sessions.begin(), m_sessions.end());
    workers = std::move(m_workers);
    m_workers.clear();
  }
  m_sessionCreated.notify_all();

  for (const auto &session : sessions) {
    if (session->state() == ReceiveSession::State::Receiving) {
      session->fail("transfer cancelled");
    }
  }
  for (auto &worker : workers) {
    worker->thread.join();
  }

  std::scoped_lock lock{m_mutex};
  m_stopping = false;
}

} // namespace aether::transfer
