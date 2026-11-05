/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Rimora Studio
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/ClientProxyUnknown.h"

#include "base/IEventQueue.h"
#include "base/Log.h"
#include "aether/AetherException.h"
#include "aether/ProtocolTypes.h"
#include "aether/ProtocolUtil.h"
#include "io/IStream.h"
#include "server/ClientProxy1_0.h"
#include "server/ClientProxy1_1.h"
#include "server/ClientProxy1_2.h"
#include "server/ClientProxy1_3.h"
#include "server/ClientProxy1_4.h"
#include "server/ClientProxy1_5.h"
#include "server/ClientProxy1_6.h"
#include "server/ClientProxy1_7.h"
#include "server/ClientProxy1_8.h"
#include "aether/TransferHub.h"
#include "server/PortForwardSession.h"
#include "server/Server.h"

#include <algorithm>
#include <cstring>

//
// ClientProxyUnknown
//

ClientProxyUnknown::ClientProxyUnknown(aether::IStream *stream, double timeout, Server *server, IEventQueue *events)
    : m_stream(stream),
      m_server(server),
      m_events(events)
{
  assert(m_server != nullptr);

  m_events->addHandler(EventTypes::Timer, this, [this](const auto &) { handleTimeout(); });
  m_timer = m_events->newOneShotTimer(timeout, this);
  addStreamHandlers();

  const auto protocol = m_server->protocolString();
  const auto helloMessage = protocol + kMsgHelloArgs;

  LOG_INFO("saying hello as %s, protocol v%d.%d", protocol.c_str(), kProtocolMajorVersion, kProtocolMinorVersion);
  ProtocolUtil::writef(m_stream, helloMessage.c_str(), kProtocolMajorVersion, kProtocolMinorVersion);
}

ClientProxyUnknown::~ClientProxyUnknown()
{
  removeHandlers();
  removeTimer();
  delete m_stream;
  delete m_proxy;
  delete m_forwardSession;
}

ClientProxy *ClientProxyUnknown::orphanClientProxy()
{
  if (m_ready) {
    removeHandlers();
    ClientProxy *proxy = m_proxy;
    m_proxy = nullptr;
    return proxy;
  } else {
    return nullptr;
  }
}

void ClientProxyUnknown::sendSuccess()
{
  m_ready = true;
  removeTimer();
  m_events->addEvent(Event(EventTypes::ClientProxyUnknownSuccess, this));
}

void ClientProxyUnknown::sendFailure()
{
  delete m_proxy;
  m_proxy = nullptr;
  delete m_forwardSession;
  m_forwardSession = nullptr;
  m_ready = false;
  removeHandlers();
  removeTimer();
  m_events->addEvent(Event(EventTypes::ClientProxyUnknownFailure, this));
}

void ClientProxyUnknown::addStreamHandlers()
{
  assert(m_stream != nullptr);
  m_events->addHandler(EventTypes::StreamInputReady, m_stream->getEventTarget(), [this](const auto &) {
    handleData();
  });
  m_events->addHandler(EventTypes::StreamOutputError, m_stream->getEventTarget(), [this](const auto &) {
    handleWriteError();
  });
  m_events->addHandler(EventTypes::StreamInputShutdown, m_stream->getEventTarget(), [this](const auto &) {
    handleDisconnect();
  });
  m_events->addHandler(EventTypes::StreamInputFormatError, m_stream->getEventTarget(), [this](const auto &) {
    handleDisconnect();
  });
  m_events->addHandler(EventTypes::StreamOutputShutdown, m_stream->getEventTarget(), [this](const auto &) {
    handleWriteError();
  });
}

void ClientProxyUnknown::addProxyHandlers()
{
  assert(m_proxy != nullptr);

  m_events->addHandler(EventTypes::ClientProxyReady, m_proxy, [this](const auto &) { sendSuccess(); });
  m_events->addHandler(EventTypes::ClientProxyDisconnected, m_proxy, [this](const auto &) { handleDisconnect(); });
}

void ClientProxyUnknown::removeHandlers()
{
  using enum EventTypes;
  if (m_stream != nullptr) {
    m_events->removeHandler(StreamInputReady, m_stream->getEventTarget());
    m_events->removeHandler(StreamOutputError, m_stream->getEventTarget());
    m_events->removeHandler(StreamInputShutdown, m_stream->getEventTarget());
    m_events->removeHandler(StreamInputFormatError, m_stream->getEventTarget());
    m_events->removeHandler(StreamOutputShutdown, m_stream->getEventTarget());
  }
  if (m_proxy != nullptr) {
    m_events->removeHandler(ClientProxyReady, m_proxy);
    m_events->removeHandler(ClientProxyDisconnected, m_proxy);
  }
}

void ClientProxyUnknown::removeTimer()
{
  if (m_timer != nullptr) {
    m_events->deleteTimer(m_timer);
    m_events->removeHandler(EventTypes::Timer, this);
    m_timer = nullptr;
  }
}

void ClientProxyUnknown::initProxy(const std::string &name, int major, int minor)
{
  if (major == 1) {
    switch (minor) {
    case 0:
      m_proxy = new ClientProxy1_0(name, m_stream, m_events);
      break;

    case 1:
      m_proxy = new ClientProxy1_1(name, m_stream, m_events);
      break;

    case 2:
      m_proxy = new ClientProxy1_2(name, m_stream, m_events);
      break;

    case 3:
      m_proxy = new ClientProxy1_3(name, m_stream, m_events);
      break;

    case 4:
      m_proxy = new ClientProxy1_4(name, m_stream, m_server, m_events);
      break;

    case 5:
      m_proxy = new ClientProxy1_5(name, m_stream, m_server, m_events);
      break;

    case 6:
      m_proxy = new ClientProxy1_6(name, m_stream, m_server, m_events);
      break;

    case 7:
      m_proxy = new ClientProxy1_7(name, m_stream, m_server, m_events);
      break;

    case 8:
      m_proxy = new ClientProxy1_8(name, m_stream, m_server, m_events);
      break;

    default:
      break;
    }
  }

  // hangup (with error) if version isn't supported
  if (m_proxy == nullptr) {
    throw IncompatibleClientException(major, minor);
  }
}

void ClientProxyUnknown::handleData()
{
  LOG_VERBOSE("parsing hello reply");

  std::string name("<unknown>");

  try {
    // limit the maximum length of the hello
    if (uint32_t n = m_stream->getSize(); n > kMaxHelloLength) {
      LOG_VERBOSE("hello reply too long");
      throw BadClientException();
    }

    // parse the reply to hello
    int16_t major;
    int16_t minor;
    if (std::string protocolName; !ProtocolUtil::readf(m_stream, kMsgHelloBack, &protocolName, &major, &minor, &name)) {
      throw BadClientException();
    }

    // disallow invalid version numbers
    if (major <= 0 || minor < 0) {
      throw IncompatibleClientException(major, minor);
    }

    // remove stream event handlers.  the proxy we're about to create
    // may install its own handlers and we don't want to accidentally
    // remove those later.
    removeHandlers();

    // a client name of "aetherfwd:<port>" requests a raw tunnel to a
    // whitelisted localhost port instead of a screen-sharing session
    if (name.compare(0, std::strlen(kPortShareNamePrefix), kPortShareNamePrefix) == 0) {
      int port = 0;
      try {
        port = std::stoi(name.substr(std::strlen(kPortShareNamePrefix)));
      } catch (...) {
        port = 0;
      }

      bool allowed = port > 0 && port <= 65535;
      if (allowed) {
        const auto sharedPorts = m_server->sharedPorts();
        allowed = std::find(sharedPorts.begin(), sharedPorts.end(), static_cast<uint16_t>(port)) != sharedPorts.end();
      }
      if (!allowed) {
        LOG_WARN("rejected port share request \"%s\"", name.c_str());
        sendFailure();
        return;
      }

      removeTimer();
      m_forwardSession = new PortForwardSession(
          m_stream, static_cast<uint16_t>(port), m_server, m_events, [this] { sendFailure(); }
      );
      m_stream = nullptr;
      return;
    }

    // a client name of "aetherxfer:..." is a data connection of a large file
    // transfer: take the socket out of the event loop and give it to a worker
    if (name.compare(0, std::strlen(kTransferNamePrefix), kTransferNamePrefix) == 0) {
      const auto request = aether::transfer::parseHelloName(name);
      if (!request || !m_server->clipboardSharingEnabled()) {
        LOG_WARN("rejected file transfer request \"%s\"", name.c_str());
        sendFailure();
        return;
      }

      removeTimer();
      if (!aether::transfer::TransferHub::instance().serveConnection(
              m_stream, *request, aether::transfer::Side::Server
          )) {
        LOG_WARN("could not take over the file transfer connection");
        sendFailure();
        return;
      }

      LOG_DEBUG("serving file transfer stream %u of %u", request->index + 1, request->count);
      // nothing else uses this connection now; let the listener discard the placeholder
      sendFailure();
      return;
    }

    // create client proxy for highest version supported by the client
    initProxy(name, major, minor);

    // the proxy is created and now proxy now owns the stream
    LOG_VERBOSE("created proxy for client \"%s\" version %d.%d", name.c_str(), major, minor);
    m_stream = nullptr;

    // wait until the proxy signals that it's ready or has disconnected
    addProxyHandlers();
    return;
  } catch (IncompatibleClientException &e) {
    // client is incompatible
    LOG_WARN("client \"%s\" has incompatible version %d.%d)", name.c_str(), e.getMajor(), e.getMinor());
    ProtocolUtil::writef(m_stream, kMsgEIncompatible, kProtocolMajorVersion, kProtocolMinorVersion);
  } catch (BadClientException &) {
    // client not behaving
    LOG_WARN("protocol error from client \"%s\"", name.c_str());
    ProtocolUtil::writef(m_stream, kMsgEBad);
  } catch (BaseException &e) {
    // misc error
    LOG_WARN("error communicating with client \"%s\": %s", name.c_str(), e.what());
  }
  sendFailure();
}

void ClientProxyUnknown::handleWriteError()
{
  LOG_INFO("error communicating with new client");
  sendFailure();
}

void ClientProxyUnknown::handleTimeout()
{
  LOG_INFO("new client is unresponsive");
  sendFailure();
}

void ClientProxyUnknown::handleDisconnect()
{
  LOG_INFO("new client disconnected");
  sendFailure();
}
