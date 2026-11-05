/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "client/TransferConnector.h"

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
    IEventQueue *events, std::unique_ptr<ISocketFactory> socketFactory, const NetworkAddress &serverAddress, bool secure
)
    : m_events(events),
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
  TransferHub::instance().setDialer([this](const StreamRequest &request) { dial(request); });
}

void TransferConnector::stop()
{
  TransferHub::instance().setDialer({});
  while (!m_conns.empty()) {
    removeConn(m_conns.back().get());
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
  m_events->addHandler(EventTypes::SocketDisconnected, target, [this, c](const auto &) { removeConn(c); });
  m_events->addHandler(EventTypes::StreamInputReady, target, [this, c](const auto &) { handleHello(c); });

  c->timer = m_events->newOneShotTimer(kHelloTimeout, nullptr);
  m_events->addHandler(EventTypes::Timer, c->timer, [this, c](const auto &) {
    LOG_WARN("file transfer: timed out waiting for the server hello");
    removeConn(c);
  });

  try {
    c->socket->connect(m_serverAddress);
  } catch (BaseException &e) {
    LOG_WARN("file transfer: connect failed: %s", e.what());
    removeConn(c);
  }
}

void TransferConnector::handleConnected(Conn *conn)
{
  auto *target = conn->socket->getEventTarget();
  m_events->removeHandler(EventTypes::DataSocketConnected, target);
  m_events->removeHandler(EventTypes::DataSocketSecureConnected, target);
  m_events->removeHandler(EventTypes::DataSocketConnectionFailed, target);

  if (conn->socket->isReady()) {
    m_events->addEvent(Event(EventTypes::StreamInputReady, target));
  }
}

void TransferConnector::handleFailed(Conn *conn, const Event &event)
{
  auto *info = static_cast<IDataSocket::ConnectionFailedInfo *>(event.getData());
  LOG_WARN("file transfer: connect failed: %s", info ? info->m_what.c_str() : "unknown");
  delete info;
  removeConn(conn);
}

void TransferConnector::handleHello(Conn *conn)
{
  int16_t serverMajor;
  int16_t serverMinor;
  std::string protocolName;

  if (!ProtocolUtil::readf(conn->socket, kMsgHello, &protocolName, &serverMajor, &serverMinor) ||
      networkProtocolFromString(QString::fromStdString(protocolName)) == NetworkProtocol::Unknown ||
      serverMajor != kProtocolMajorVersion) {
    LOG_WARN("file transfer: bad hello from server");
    removeConn(conn);
    return;
  }

  const int16_t helloBackMinor = serverMinor < kProtocolMinorVersion ? serverMinor : kProtocolMinorVersion;
  std::string name = aether::transfer::makeHelloName(conn->request);
  const std::string helloBackMessage = protocolName + kMsgHelloBackArgs;

  try {
    ProtocolUtil::writef(conn->socket, helloBackMessage.c_str(), kProtocolMajorVersion, helloBackMinor, &name);
  } catch (BaseException &e) {
    LOG_WARN("file transfer: hello-back write failed: %s", e.what());
    removeConn(conn);
    return;
  }

  auto channel = conn->socket->detachBulkChannel();
  if (!channel) {
    LOG_WARN("file transfer: could not take over the connection");
    removeConn(conn);
    return;
  }

  const auto request = conn->request;
  removeConn(conn);
  TransferHub::instance().serve(std::move(channel), request, Side::Client);
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
  if (conn->socket != nullptr) {
    m_events->removeHandlers(conn->socket->getEventTarget());
    delete conn->socket;
  }
  m_conns.erase(it);
}
