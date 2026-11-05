/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "BulkChannelTests.h"

#include "TlsSocketPair.h"
#include "base/EventQueue.h"
#include "base/EventTypes.h"
#include "net/NetworkAddress.h"
#include "net/SocketException.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPListenSocket.h"
#include "net/TCPSocket.h"

#include <chrono>
#include <functional>
#include <thread>

using namespace aether::testing;

namespace {

std::string patternBytes(size_t size)
{
  std::string data(size, '\0');
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<char>((i * 2654435761u) >> 24);
  }
  return data;
}

} // namespace

void BulkChannelTests::initTestCase()
{
  m_arch.init();
#if defined(_WIN32)
  QSKIP("socket pair helpers are POSIX only");
#endif
  m_certPath = m_dir.filePath("tls.pem");
  aether::generatePemSelfSignedCert(m_certPath, 2048);
}

void BulkChannelTests::plainRoundTrip()
{
#if !defined(_WIN32)
  auto [a, b] = makeSocketPair();
  QVERIFY(a && b);

  const auto data = patternBytes(3 * 1024 * 1024 + 17);
  std::thread writer([&] { a->write(data.data(), data.size()); });

  std::string received(data.size(), '\0');
  b->readFully(received.data(), received.size());
  writer.join();
  QVERIFY(received == data);

  const char reply = 'k';
  b->write(&reply, 1);
  char got = 0;
  a->readFully(&got, 1);
  QCOMPARE(got, 'k');
#endif
}

void BulkChannelTests::plainServesPendingBytesFirst()
{
#if !defined(_WIN32)
  auto [a, b] = makeSocketPair({}, "early-bytes");
  QVERIFY(a && b);

  a->write("-then-wire", 10);

  std::string received(21, '\0');
  size_t got = 0;
  while (got < received.size()) {
    got += b->read(received.data() + got, 4);
  }
  QCOMPARE(received, std::string("early-bytes-then-wire"));
#endif
}

void BulkChannelTests::plainEndOfStreamReadsZero()
{
#if !defined(_WIN32)
  auto [a, b] = makeSocketPair();
  QVERIFY(a && b);
  a->write("tail", 4);
  a.reset();

  char buffer[16];
  QCOMPARE(b->read(buffer, sizeof(buffer)), size_t{4});
  QCOMPARE(b->read(buffer, sizeof(buffer)), size_t{0});
#endif
}

void BulkChannelTests::plainReadFullyFailsOnEarlyEnd()
{
#if !defined(_WIN32)
  auto [a, b] = makeSocketPair();
  QVERIFY(a && b);
  a->write("short", 5);
  a.reset();

  char buffer[16];
  QVERIFY_EXCEPTION_THROWN(b->readFully(buffer, sizeof(buffer)), BulkChannelError);
#endif
}

void BulkChannelTests::plainCancelUnblocksReader()
{
#if !defined(_WIN32)
  auto [a, b] = makeSocketPair();
  QVERIFY(a && b);

  std::thread reader([&] {
    char buffer[8];
    try {
      b->read(buffer, sizeof(buffer));
    } catch (const BulkChannelError &) {
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto start = std::chrono::steady_clock::now();
  b->cancel();
  reader.join();
  QVERIFY2(std::chrono::steady_clock::now() - start < std::chrono::seconds(5), "cancel did not wake the reader");

  char buffer[8];
  QVERIFY_EXCEPTION_THROWN(b->read(buffer, sizeof(buffer)), BulkChannelError);
  QVERIFY_EXCEPTION_THROWN(b->write("x", 1), BulkChannelError);
#endif
}

void BulkChannelTests::tlsRoundTrip()
{
#if !defined(_WIN32)
  auto [a, b] = makeTlsPair(m_certPath);
  QVERIFY2(a && b, "tls handshake failed");

  a->write("hello over tls", 14);
  char buffer[32] = {};
  b->readFully(buffer, 14);
  QCOMPARE(std::string(buffer, 14), std::string("hello over tls"));

  b->write("pong", 4);
  a->readFully(buffer, 4);
  QCOMPARE(std::string(buffer, 4), std::string("pong"));

  a.reset();
  QCOMPARE(b->read(buffer, sizeof(buffer)), size_t{0});
#endif
}

void BulkChannelTests::tlsLargeTransfer()
{
#if !defined(_WIN32)
  auto [a, b] = makeTlsPair(m_certPath);
  QVERIFY2(a && b, "tls handshake failed");

  const auto data = patternBytes(24 * 1024 * 1024);
  std::thread writer([&] { a->write(data.data(), data.size()); });

  std::string received(data.size(), '\0');
  for (size_t got = 0; got < received.size();) {
    const size_t chunk = std::min<size_t>(1024 * 1024, received.size() - got);
    b->readFully(received.data() + got, chunk);
    got += chunk;
  }
  writer.join();
  QVERIFY(received == data);
#endif
}

void BulkChannelTests::tlsCancelUnblocksReader()
{
#if !defined(_WIN32)
  auto [a, b] = makeTlsPair(m_certPath);
  QVERIFY2(a && b, "tls handshake failed");

  std::thread reader([&] {
    char buffer[8];
    try {
      while (b->read(buffer, sizeof(buffer)) > 0) {
      }
    } catch (const BulkChannelError &) {
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto start = std::chrono::steady_clock::now();
  b->cancel();
  reader.join();
  QVERIFY2(std::chrono::steady_clock::now() - start < std::chrono::seconds(5), "cancel did not wake the reader");
#endif
}

void BulkChannelTests::detachesLiveTcpSockets()
{
  EventQueue events;
  SocketMultiplexer multiplexer;

  std::unique_ptr<TCPListenSocket> listener;
  NetworkAddress address;
  for (int port = 41000; port < 41100 && !listener; ++port) {
    try {
      auto candidate = std::make_unique<TCPListenSocket>(&events, &multiplexer, IArchNetwork::AddressFamily::INet);
      address = NetworkAddress("127.0.0.1", port);
      address.resolve();
      candidate->bind(address);
      listener = std::move(candidate);
    } catch (const SocketAddressInUseException &) {
    }
  }
  QVERIFY2(listener, "no free local port");

  std::atomic<IDataSocket *> accepted = nullptr;
  events.addHandler(EventTypes::ListenSocketConnecting, listener.get(), [&](const Event &) {
    accepted = listener->accept().release();
  });

  auto client = std::make_unique<TCPSocket>(&events, &multiplexer);
  std::atomic<bool> connected = false;
  events.addHandler(EventTypes::DataSocketConnected, client->getEventTarget(), [&](const Event &) {
    connected = true;
  });

  // The event loop has to run on this (Arch) thread; the scenario drives it from another.
  std::vector<std::string> failures;
  const auto expect = [&failures](bool condition, const char *what) {
    if (!condition) {
      failures.emplace_back(what);
    }
    return condition;
  };
  const auto waitFor = [](const std::function<bool()> &done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
  };

  std::unique_ptr<IDataSocket> server;
  std::thread scenario([&] {
    client->connect(address);
    if (!expect(waitFor([&] { return connected && accepted != nullptr; }), "connection was not established")) {
      events.addEvent(Event(EventTypes::Quit));
      return;
    }
    server.reset(accepted.load());

    // Bytes the peer sent before the detach are already in the input buffer.
    client->write("0123456789", 10);
    expect(waitFor([&] { return server->getSize() == 10; }), "early bytes did not arrive");

    auto serverChannel = server->detachBulkChannel();
    auto clientChannel = client->detachBulkChannel();
    if (expect(serverChannel && clientChannel, "detach failed")) {
      try {
        char early[10];
        serverChannel->readFully(early, sizeof(early));
        expect(std::string(early, 10) == "0123456789", "buffered bytes were lost");

        const auto data = patternBytes(8 * 1024 * 1024);
        std::thread writer([&] { clientChannel->write(data.data(), data.size()); });
        std::string received(data.size(), '\0');
        serverChannel->readFully(received.data(), received.size());
        writer.join();
        expect(received == data, "bulk data was corrupted");

        const char reply = 'r';
        serverChannel->write(&reply, 1);
        char got = 0;
        clientChannel->readFully(&got, 1);
        expect(got == 'r', "reply was lost");
      } catch (const std::exception &e) {
        failures.emplace_back(e.what());
      }
    }
    events.addEvent(Event(EventTypes::Quit));
  });

  events.loop();
  scenario.join();

  // The emptied socket objects must be safe to delete.
  events.removeHandlers(client->getEventTarget());
  if (server) {
    events.removeHandlers(server->getEventTarget());
  }
  client.reset();
  server.reset();
  events.removeHandlers(listener.get());
  listener.reset();

  for (const auto &failure : failures) {
    QFAIL(failure.c_str());
  }
}

QTEST_MAIN(BulkChannelTests)
