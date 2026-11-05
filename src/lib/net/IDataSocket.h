/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "io/IStream.h"
#include "net/BulkChannel.h"
#include "net/ISocket.h"

#include <memory>
#include <string>

//! Data stream socket interface
/*!
This interface defines the methods common to all network sockets that
represent a full-duplex data stream.
*/
class IDataSocket : public ISocket, public aether::IStream
{
public:
  class ConnectionFailedInfo
  {
  public:
    explicit ConnectionFailedInfo(const char *what) : m_what(what)
    {
      // do nothing
    }
    std::string m_what;
  };

  explicit IDataSocket([[maybe_unused]] const IEventQueue *events)
  {
    // do nothing
  }

  //! @name manipulators
  //@{

  //! Connect socket
  /*!
  Attempt to connect to a remote endpoint.  This returns immediately
  and sends a connected event when successful or a connection failed
  event when it fails.  The stream acts as if shutdown for input and
  output until the stream connects.
  */
  virtual void connect(const NetworkAddress &) = 0;

  //@}

  // ISocket overrides
  void close() override;
  void *getEventTarget() const override;

  virtual bool isFatal() const = 0;

  //! Hand the connection to a blocking BulkChannel
  /*!
  Flushes pending output, takes the socket out of the multiplexer and moves
  the descriptor, any TLS state and already buffered input into the returned
  channel. The socket is left disconnected and should be deleted afterwards.
  Must not be called from the multiplexer thread. Returns null when the
  socket is not connected or does not support detaching.
  */
  virtual std::unique_ptr<BulkChannel> detachBulkChannel()
  {
    return nullptr;
  }
};
