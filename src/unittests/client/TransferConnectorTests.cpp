/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "TransferConnectorTests.h"

#include "../aether/TransferTestSupport.h"
#include "aether/PacketStreamFilter.h"
#include "aether/ProtocolTypes.h"
#include "aether/ProtocolUtil.h"
#include "aether/TransferHub.h"
#include "aether/ipc/CoreIpcServer.h"
#include "base/EventQueue.h"
#include "base/EventTypes.h"
#include "client/TransferConnector.h"
#include "common/Settings.h"
#include "net/FingerprintDatabase.h"
#include "net/IDataSocket.h"
#include "net/IListenSocket.h"
#include "net/NetworkAddress.h"
#include "net/SecureUtils.h"
#include "net/SocketException.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPSocketFactory.h"

#include <QDir>

#include <openssl/pem.h>
#include <openssl/x509.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace fs = std::filesystem;
using namespace aether::testing;
using namespace aether::transfer;

namespace {

//! Answers data connections the way ClientListener and ClientProxyUnknown do
class LoopbackServer
{
public:
  LoopbackServer(EventQueue &events, SocketMultiplexer &multiplexer, TransferHub &hub, bool secure)
      : m_events(events),
        m_hub(hub),
        m_secure(secure)
  {
    const TCPSocketFactory factory(&events, &multiplexer);
    const auto level = secure ? SecurityLevel::PeerAuth : SecurityLevel::PlainText;
    for (int port = 41200; port < 41300 && !m_listener; ++port) {
      try {
        std::unique_ptr<IListenSocket> candidate(factory.createListen(IArchNetwork::AddressFamily::INet, level));
        m_address = NetworkAddress("127.0.0.1", port);
        m_address.resolve();
        candidate->bind(m_address);
        m_listener = std::move(candidate);
      } catch (const SocketAddressInUseException &) {
      }
    }
    if (m_listener) {
      m_events.addHandler(EventTypes::ListenSocketConnecting, m_listener.get(), [this](const Event &) { accept(); });
    }
  }

  ~LoopbackServer()
  {
    for (const auto &filter : m_filters) {
      m_events.removeHandlers(filter->getEventTarget());
    }
    m_filters.clear();
    for (const auto &socket : m_sockets) {
      m_events.removeHandlers(socket->getEventTarget());
    }
    m_sockets.clear();
    stopListening();
  }

  bool listening() const
  {
    return m_listener != nullptr;
  }

  const NetworkAddress &address() const
  {
    return m_address;
  }

  void stopListening()
  {
    if (m_listener) {
      m_events.removeHandlers(m_listener.get());
      m_listener.reset();
    }
  }

  void hangUpOnAccept()
  {
    m_hangUp = true;
  }

  int served() const
  {
    return m_served;
  }

private:
  void accept()
  {
    std::unique_ptr<IDataSocket> socket(m_listener->accept());
    if (!socket || m_hangUp) {
      return;
    }

    auto *raw = socket.get();
    m_sockets.push_back(std::move(socket));
    if (m_secure) {
      // like ClientListener, wait for the TLS handshake before saying hello
      m_events.addHandler(EventTypes::ClientListenerAccepted, raw->getEventTarget(), [this, raw](const Event &) {
        sayHello(raw);
      });
    } else {
      sayHello(raw);
    }
  }

  void sayHello(IDataSocket *socket)
  {
    auto filter = std::make_unique<PacketStreamFilter>(&m_events, socket, false);
    auto *stream = filter.get();
    m_events.addHandler(EventTypes::StreamInputReady, stream->getEventTarget(), [this, stream](const Event &) {
      helloBack(stream);
    });
    const std::string hello = std::string("Barrier") + kMsgHelloArgs;
    ProtocolUtil::writef(stream, hello.c_str(), kProtocolMajorVersion, kProtocolMinorVersion);
    m_filters.push_back(std::move(filter));
  }

  void helloBack(PacketStreamFilter *stream)
  {
    m_events.removeHandlers(stream->getEventTarget());

    std::string protocolName;
    std::string name;
    int16_t major = 0;
    int16_t minor = 0;
    if (!ProtocolUtil::readf(stream, kMsgHelloBack, &protocolName, &major, &minor, &name)) {
      return;
    }
    const auto request = parseHelloName(name);
    if (request && m_hub.serveConnection(stream, *request, Side::Server)) {
      ++m_served;
    }
  }

  EventQueue &m_events;
  TransferHub &m_hub;
  std::unique_ptr<IListenSocket> m_listener;
  NetworkAddress m_address;
  std::vector<std::unique_ptr<IDataSocket>> m_sockets;
  std::vector<std::unique_ptr<PacketStreamFilter>> m_filters;
  std::atomic<int> m_served = 0;
  bool m_secure;
  bool m_hangUp = false;
};

//! Runs the event loop on this (Arch) thread until \c done holds or time runs out.
bool runUntil(EventQueue &events, const std::function<bool()> &done, std::chrono::seconds limit)
{
  std::atomic<bool> result = false;
  std::thread watcher([&] {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    result = done();
    events.addEvent(Event(EventTypes::Quit));
  });
  events.loop();
  watcher.join();
  return result;
}

bool hasState(const TransferHub &hub, const Token &token, ReceiveSession::State state)
{
  const auto progress = hub.status(token);
  return progress && progress->state == state;
}

bool settled(const TransferHub &hub, const Token &token)
{
  const auto progress = hub.status(token);
  return progress && progress->state != ReceiveSession::State::Receiving;
}

//! A large file and a folder of many small ones, like the selections users copy
std::vector<fs::path> buildSelection(const fs::path &root)
{
  writePatternFile(root / "large.bin", 40ull * 1024 * 1024 + 123, 7);
  for (int i = 0; i < 300; ++i) {
    const auto sub = i % 3 == 0 ? fs::path("nested") : fs::path();
    writePatternFile(root / "folder" / sub / ("file-" + std::to_string(i) + ".dat"), (i * 7919) % 20000, i);
  }
  fs::create_directories(root / "folder" / "empty");
  return {root / "large.bin", root / "folder"};
}

bool sameTree(const fs::path &expected, const fs::path &actual)
{
  if (fs::is_regular_file(expected)) {
    return filesEqual(expected, actual);
  }
  size_t count = 0;
  for (const auto &entry : fs::recursive_directory_iterator(expected)) {
    const auto relative = fs::relative(entry.path(), expected);
    if (entry.is_directory() ? !fs::is_directory(actual / relative) : !filesEqual(entry.path(), actual / relative)) {
      return false;
    }
    ++count;
  }
  size_t actualCount = 0;
  for ([[maybe_unused]] const auto &entry : fs::recursive_directory_iterator(actual)) {
    ++actualCount;
  }
  return count == actualCount;
}

bool sameSelection(const std::vector<fs::path> &expected, const std::vector<fs::path> &actual)
{
  if (expected.size() != actual.size()) {
    return false;
  }
  for (const auto &path : expected) {
    const auto match = std::find_if(actual.begin(), actual.end(), [&](const auto &candidate) {
      return candidate.filename() == path.filename();
    });
    if (match == actual.end() || !sameTree(path, *match)) {
      return false;
    }
  }
  return true;
}

fs::path testRoot(const QTemporaryDir &dir, const char *name)
{
  return fs::path(dir.path().toStdWString()) / name;
}

} // namespace

void TransferConnectorTests::initTestCase()
{
  m_arch.init();
  QVERIFY(m_dir.isValid());

#if !defined(_WIN32)
  // Settings reads these when first used: keep the TLS setup away from the real configuration.
  qputenv("XDG_CONFIG_HOME", m_dir.filePath("config").toUtf8());
  qputenv("XDG_STATE_HOME", m_dir.filePath("state").toUtf8());

  // Both ends use the same certificate and trust it, as after pairing.
  const auto certificate = Settings::value(Settings::Security::Certificate).toString();
  QVERIFY(certificate.startsWith(m_dir.path()));
  QVERIFY(QDir().mkpath(Settings::tlsDir()));
  aether::generatePemSelfSignedCert(certificate, 2048);

  BIO *bio = BIO_new_file(certificate.toUtf8().constData(), "r");
  QVERIFY(bio != nullptr);
  X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  QVERIFY(cert != nullptr);
  const auto fingerprint = aether::sslCertFingerprint(cert, QCryptographicHash::Sha256);
  X509_free(cert);

  // TLS peers report their fingerprint to the GUI through the core IPC server.
  m_coreIpc = std::make_unique<aether::core::ipc::CoreIpcServer>(nullptr);

  FingerprintDatabase trusted;
  trusted.addTrusted(fingerprint);
  QVERIFY(trusted.write(Settings::tlsTrustedServersDb()));
  QVERIFY(trusted.write(Settings::tlsTrustedClientsDb()));
#endif
}

void TransferConnectorTests::pullsFilesThroughRealSockets_data()
{
  QTest::addColumn<bool>("secure");
  QTest::newRow("plain") << false;
  QTest::newRow("tls") << true;
}

void TransferConnectorTests::pushesFilesThroughRealSockets_data()
{
  pullsFilesThroughRealSockets_data();
}

void TransferConnectorTests::pullsFilesThroughRealSockets()
{
  QFETCH(bool, secure);
#if defined(_WIN32)
  if (secure) {
    QSKIP("the TLS setup relies on XDG settings paths");
  }
#endif
  const auto root = testRoot(m_dir, secure ? "pull-tls" : "pull");
  const auto selection = buildSelection(root / "source");

  EventQueue events;
  SocketMultiplexer multiplexer;
  TransferHub clientHub(root / "client");
  TransferHub serverHub(root / "server");
  LoopbackServer server(events, multiplexer, serverHub, secure);
  QVERIFY2(server.listening(), "no free local port");
  TransferConnector connector(
      &events, std::make_unique<TCPSocketFactory>(&events, &multiplexer), server.address(), secure, &clientHub
  );
  connector.start();

  const auto manifest = decodeManifest(serverHub.offer(buildSource(selection)));
  QCOMPARE(plannedStreams(manifest), 2u);
  clientHub.request(manifest);

  QVERIFY(runUntil(events, [&] { return settled(clientHub, manifest.token); }, std::chrono::seconds(60)));
  const auto progress = clientHub.status(manifest.token);
  QVERIFY2(progress->state == ReceiveSession::State::Complete, progress->error.c_str());
  QCOMPARE(server.served(), 2);
  QVERIFY(sameSelection(selection, progress->paths));
}

void TransferConnectorTests::pushesFilesThroughRealSockets()
{
  QFETCH(bool, secure);
#if defined(_WIN32)
  if (secure) {
    QSKIP("the TLS setup relies on XDG settings paths");
  }
#endif
  const auto root = testRoot(m_dir, secure ? "push-tls" : "push");
  const auto selection = buildSelection(root / "source");

  EventQueue events;
  SocketMultiplexer multiplexer;
  TransferHub clientHub(root / "client");
  TransferHub serverHub(root / "server");
  LoopbackServer server(events, multiplexer, serverHub, secure);
  QVERIFY2(server.listening(), "no free local port");
  TransferConnector connector(
      &events, std::make_unique<TCPSocketFactory>(&events, &multiplexer), server.address(), secure, &clientHub
  );
  connector.start();

  const auto manifest = decodeManifest(clientHub.offer(buildSource(selection)));
  clientHub.announceSent(manifest);

  QVERIFY(runUntil(events, [&] { return settled(serverHub, manifest.token); }, std::chrono::seconds(60)));
  const auto progress = serverHub.status(manifest.token);
  QVERIFY2(progress->state == ReceiveSession::State::Complete, progress->error.c_str());
  QCOMPARE(server.served(), 2);
  QVERIFY(sameSelection(selection, progress->paths));
}

void TransferConnectorTests::serverHangingUpFailsThePull()
{
  const auto root = testRoot(m_dir, "hang-up");
  writePatternFile(root / "source" / "file.bin", 1024, 1);

  EventQueue events;
  SocketMultiplexer multiplexer;
  TransferHub clientHub(root / "client");
  TransferHub serverHub(root / "server");
  LoopbackServer server(events, multiplexer, serverHub, false);
  QVERIFY2(server.listening(), "no free local port");
  server.hangUpOnAccept();
  TransferConnector connector(
      &events, std::make_unique<TCPSocketFactory>(&events, &multiplexer), server.address(), false, &clientHub
  );
  connector.start();

  const auto manifest = decodeManifest(serverHub.offer(buildSource({root / "source" / "file.bin"})));
  clientHub.request(manifest);

  // Well before the hello timeout: hanging up itself must fail the transfer.
  QVERIFY(runUntil(events, [&] { return settled(clientHub, manifest.token); }, std::chrono::seconds(5)));
  QVERIFY(hasState(clientHub, manifest.token, ReceiveSession::State::Failed));

  // A later request for the same files starts over instead of waiting on the dead attempt.
  QCOMPARE(clientHub.request(manifest).state, ReceiveSession::State::Receiving);
}

void TransferConnectorTests::refusedConnectionFailsThePull()
{
  const auto root = testRoot(m_dir, "refused");
  writePatternFile(root / "source" / "file.bin", 1024, 2);

  EventQueue events;
  SocketMultiplexer multiplexer;
  TransferHub clientHub(root / "client");
  TransferHub serverHub(root / "server");
  LoopbackServer server(events, multiplexer, serverHub, false);
  QVERIFY2(server.listening(), "no free local port");
  server.stopListening();
  TransferConnector connector(
      &events, std::make_unique<TCPSocketFactory>(&events, &multiplexer), server.address(), false, &clientHub
  );
  connector.start();

  const auto manifest = decodeManifest(serverHub.offer(buildSource({root / "source" / "file.bin"})));
  clientHub.request(manifest);

  QVERIFY(runUntil(events, [&] { return settled(clientHub, manifest.token); }, std::chrono::seconds(5)));
  const auto progress = clientHub.status(manifest.token);
  QCOMPARE(progress->state, ReceiveSession::State::Failed);
  QVERIFY2(progress->error.find("data connection failed") != std::string::npos, progress->error.c_str());
}

QTEST_MAIN(TransferConnectorTests)
