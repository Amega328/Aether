/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "client/TransferConnector.h"

#include "aether/PacketStreamFilter.h"
#include "aether/ProtocolTypes.h"
#include "aether/ProtocolUtil.h"
#include "arch/Arch.h"
#include "base/BaseException.h"
#include "base/Event.h"
#include "base/EventTypes.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "common/NetworkProtocol.h"
#include "net/IDataSocket.h"
#include "net/ISocketFactory.h"

#include <QString>

#include <algorithm>

using aether::transfer::Side;
using aether::transfer::StreamRequest;
using aether::transfer::TransferHub;

TransferConnector::TransferConnector(
    IEventQueue *events, std::unique_ptr<ISocketFactory> socketFactory, const NetworkAddress &serverAddress,
    bool secure, TransferHub *hub
)
    : m_events(events),
      m_hub(hub != nullptr ? *hub : TransferHub::instance()),
      m_socketFactory(std::move(socketFactory)),
      m_serverAddress(serverAddress),
      m_securityLevel(secure ? SecurityLevel::PeerAuth : SecurityLevel::PlainText)
{
  assert(m_events != nullptr);
  assert(m_socketFactory != nullptr);
}

TransferConnector::~TransferConnector()
{
  stop();
}

void TransferConnector::start()
{
  try {
    m_serverAddress.resolve();
  } catch (BaseException &e) {
    LOG_WARN("file transfer: cannot resolve server address: %s", e.what());
  }
  m_hub.setDialer([this](const StreamRequest &request) { dial(request); });
}

void TransferConnector::stop()
{
  m_hub.setDialer({});
  while (!m_conns.empty()) {
    fail(m_conns.back().get(), "disconnected from the server");
  }
}

void TransferConnector::dial(const StreamRequest &request)
{
  auto conn = std::make_unique<Conn>();
  conn->request = request;

  try {
    conn->socket = m_socketFactory->create(ARCH->getAddrFamily(m_serverAddress.getAddress()), m_securityLevel);
  } catch (BaseException &e) {
    LOG_WARN("file transfer: cannot create socket: %s", e.what());
    m_hub.streamFailed(request, "cannot create a socket");
    return;
  }

  auto *c = conn.get();
  m_conns.push_back(std::move(conn));

  auto *target = c->socket->getEventTarget();
  const auto connectedType = m_securityLevel == SecurityLevel::PlainText ? EventTypes::DataSocketConnected
                                                                         : EventTypes::DataSocketSecureConnected;
  m_events->addHandler(connectedType, target, [this, c](const auto &) { handleConnected(c); });
  m_events->addHandler(EventTypes::DataSocketConnectionFailed, target, [this, c](const auto &e) {
    handleFailed(c, e);
  });
  m_events->addHandler(EventTypes::SocketDisconnected, target, [this, c](const auto &) {
    fail(c, "connection closed");
  });

  c->timer = m_events->newOneShotTimer(kHelloTimeout, nullptr);
  m_events->addHandler(EventTypes::Timer, c->timer, [this, c](const auto &) {
    fail(c, "timed out waiting for the server hello");
  });

  try {
    c->socket->connect(m_serverAddress);
  } catch (BaseException &e) {
    fail(c, std::string("connect failed: ") + e.what());
  }
}

void TransferConnector::handleConnected(Conn *conn)
{
  auto *target = conn->socket->getEventTarget();
  m_events->removeHandler(EventTypes::DataSocketConnected, target);
  m_events->removeHandler(EventTypes::DataSocketSecureConnected, target);
  m_events->removeHandler(EventTypes::DataSocketConnectionFailed, target);

  // The server speaks the normal packet-framed handshake, so read its hello the way Client does.
  // The filter takes over every event of the socket and re-sends them as its own.
  conn->stream = std::make_unique<PacketStreamFilter>(m_events, conn->socket, false);
  auto *streamTarget = conn->stream->getEventTarget();
  m_events->addHandler(EventTypes::StreamInputReady, streamTarget, [this, conn](const auto &) { handleHello(conn); });
  for (const auto type :
       {EventTypes::SocketDisconnected, EventTypes::StreamInputShutdown, EventTypes::StreamInputFormatError,
        EventTypes::StreamOutputError}) {
    m_events->addHandler(type, streamTarget, [this, conn](const auto &) {
      fail(conn, "server closed the connection during the handshake");
    });
  }

  if (conn->socket->isReady()) {
    m_events->addEvent(Event(EventTypes::StreamInputReady, target));
  }
}

void TransferConnector::handleFailed(Conn *conn, const Event &event)
{
  auto *info = static_cast<IDataSocket::ConnectionFailedInfo *>(event.getData());
  const std::string what = info ? info->m_what : "unknown";
  delete info;
  fail(conn, "connect failed: " + what);
}

void TransferConnector::fail(Conn *conn, const std::string &reason)
{
  LOG_WARN("file transfer: %s", reason.c_str());
  const auto request = conn->request;
  removeConn(conn);
  m_hub.streamFailed(request, reason);
}

void TransferConnector::handleHello(Conn *conn)
{
  int16_t serverMajor;
  int16_t serverMinor;
  std::string protocolName;

  if (!ProtocolUtil::readf(conn->stream.get(), kMsgHello, &protocolName, &serverMajor, &serverMinor) ||
      networkProtocolFromString(QString::fromStdString(protocolName)) == NetworkProtocol::Unknown ||
      serverMajor != kProtocolMajorVersion) {
    fail(conn, "bad hello from server");
    return;
  }

  const int16_t helloBackMinor = serverMinor < kProtocolMinorVersion ? serverMinor : kProtocolMinorVersion;
  std::string name = aether::transfer::makeHelloName(conn->request);
  const std::string helloBackMessage = protocolName + kMsgHelloBackArgs;

  try {
    ProtocolUtil::writef(conn->stream.get(), helloBackMessage.c_str(), kProtocolMajorVersion, helloBackMinor, &name);
  } catch (BaseException &e) {
    fail(conn, std::string("hello-back write failed: ") + e.what());
    return;
  }

  // Nothing may stay in the filter: the server sends no data before it has our hello-back.
  conn->stream.reset();
  auto channel = conn->socket->detachBulkChannel();
  if (!channel) {
    fail(conn, "could not take over the connection");
    return;
  }

  const auto request = conn->request;
  removeConn(conn);
  m_hub.serve(std::move(channel), request, Side::Client);
}

void TransferConnector::removeConn(Conn *conn)
{
  const auto it = std::find_if(m_conns.begin(), m_conns.end(), [conn](const auto &c) { return c.get() == conn; });
  if (it == m_conns.end()) {
    return;
  }

  if (conn->timer != nullptr) {
    m_events->removeHandler(EventTypes::Timer, conn->timer);
    m_events->deleteTimer(conn->timer);
  }
  if (conn->stream != nullptr) {
    m_events->removeHandlers(conn->stream->getEventTarget());
    conn->stream.reset();
  }
  if (conn->socket != nullptr) {
    m_events->removeHandlers(conn->socket->getEventTarget());
    delete conn->socket;
  }
  m_conns.erase(it);
}
