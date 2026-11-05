/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Rimora Studio
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "aether/App.h"
#include "net/NetworkAddress.h"

#include <QList>

#include <memory>

namespace aether {
class Screen;
class ClientArgs;
} // namespace aether

class Event;
class Client;
class PortShareListener;
class TransferConnector;
class Thread;
class ISocketFactory;

class ClientApp : public App
{
public:
  explicit ClientApp(IEventQueue *events, const QString &processName = QString());
  ~ClientApp() override = default;

  //
  // IApp overrides
  //

  void parseArgs() override;
  const char *daemonName() const override;
  void loadConfig() override
  {
    // do nothing
  }
  bool loadConfig(const QString &) override
  {
    return false;
  }
  int start() override;
  int runInner(StartupFunc startup) override;
  aether::Screen *createScreen() override;
  int mainLoop() override;
  void startNode() override;

  //
  // Regular functions
  //
  aether::Screen *openClientScreen();
  void closeClientScreen(aether::Screen *screen);
  void handleClientRestart(const Event &, EventQueueTimer *vtimer);
  void scheduleClientRestart(double retryTime);
  void handleClientConnected();
  void handleClientFailed(const Event &e);
  void handleClientRefused(const Event &e);
  void handleClientDisconnected();
  Client *openClient(const std::string &name, const NetworkAddress &address, aether::Screen *screen);
  void closeClient(Client *client);
  bool startClient();
  void stopClient();
  Client *getClientPtr()
  {
    return m_client;
  }

  //
  // Static functions
  //

  static ClientApp &instance()
  {
    return (ClientApp &)App::instance();
  }

  /**
   * @brief retryTime
   * @return next retry time based on number of current retries
   */
  double retryTime() const;

private:
  ISocketFactory *getSocketFactory() const;
  NetworkAddress &getCurrentServerAddress();
  void tryNextServer();
  void startPortShare();
  void stopPortShare();
  void startTransfers();
  void stopTransfers();

  bool m_suspended = false;
  Client *m_client = nullptr;
  std::unique_ptr<PortShareListener> m_portShare;
  std::unique_ptr<TransferConnector> m_transfers;
  aether::Screen *m_clientScreen = nullptr;
  QList<NetworkAddress> m_serverAddresses;
  size_t m_currentServerIndex = 0;
  size_t m_lastServerAddressIndex = 0;
  uint m_retryCount = 0;
};
