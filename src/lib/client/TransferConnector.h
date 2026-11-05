/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "aether/PacketStreamFilter.h"
#include "aether/TransferHub.h"
#include "net/NetworkAddress.h"
#include "net/SecurityLevel.h"

#include <memory>
#include <vector>

class Event;
class EventQueueTimer;
class IDataSocket;
class IEventQueue;
class ISocketFactory;

//! Opens large file transfer data connections to the server
/*!
Registers itself as the TransferHub's dialer. For each requested stream it
connects to the server (same TLS level as the main connection), answers the
server hello with an `aetherxfer:` client name, then detaches the socket into
a BulkChannel and hands it to the hub, which runs it on a worker thread.
*/
class TransferConnector
{
public:
  TransferConnector(
      IEventQueue *events, std::unique_ptr<ISocketFactory> socketFactory, const NetworkAddress &serverAddress,
      bool secure
  );
  TransferConnector(const TransferConnector &) = delete;
  TransferConnector &operator=(const TransferConnector &) = delete;
  ~TransferConnector();

  void start();
  void stop();

private:
  struct Conn
  {
    IDataSocket *socket = nullptr;
    //! Frames the handshake in packets, like the main connection; released before the socket is detached
    std::unique_ptr<PacketStreamFilter> stream;
    EventQueueTimer *timer = nullptr;
    aether::transfer::StreamRequest request;
  };

  void dial(const aether::transfer::StreamRequest &request);
  void handleConnected(Conn *conn);
  void handleFailed(Conn *conn, const Event &event);
  void handleHello(Conn *conn);
  void removeConn(Conn *conn);

  static constexpr double kHelloTimeout = 10.0;

  IEventQueue *m_events;
  std::unique_ptr<ISocketFactory> m_socketFactory;
  NetworkAddress m_serverAddress;
  SecurityLevel m_securityLevel;
  std::vector<std::unique_ptr<Conn>> m_conns;
};
