/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

struct ssl_st;

class BulkChannelError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

//! Blocking byte channel for bulk data
/*!
A connection taken out of the socket multiplexer so a dedicated thread can
move large payloads with plain blocking reads and writes. A channel must only
be read or written by one thread at a time; \c cancel() may be called from any
thread to unblock a pending call.
*/
class BulkChannel
{
public:
  virtual ~BulkChannel() = default;

  //! Read up to \c n bytes; blocks until some arrive. Returns 0 at end of stream.
  virtual size_t read(void *buffer, size_t n) = 0;

  //! Write all \c n bytes, blocking as needed.
  virtual void write(const void *buffer, size_t n) = 0;

  //! Abort the connection and wake any blocked reader or writer.
  virtual void cancel() = 0;

  //! Read exactly \c n bytes or throw BulkChannelError on early end of stream.
  void readFully(void *buffer, size_t n);
};

//! BulkChannel over a connected TCP socket, optionally wrapped in TLS
class SocketBulkChannel final : public BulkChannel
{
public:
#if defined(_WIN32)
  using NativeSocket = uintptr_t;
#else
  using NativeSocket = int;
#endif

  //! Takes over \c ssl (may be null) and calls \c closer to release the socket.
  /*!
  \c pending holds bytes already read from the peer that must be returned
  before anything new is received from the socket.
  */
  SocketBulkChannel(NativeSocket socket, ssl_st *ssl, std::string pending, std::function<void()> closer);
  SocketBulkChannel(const SocketBulkChannel &) = delete;
  SocketBulkChannel &operator=(const SocketBulkChannel &) = delete;
  ~SocketBulkChannel() override;

  size_t read(void *buffer, size_t n) override;
  void write(const void *buffer, size_t n) override;
  void cancel() override;

private:
  NativeSocket m_socket;
  ssl_st *m_ssl;
  std::string m_pending;
  size_t m_pendingOffset = 0;
  std::function<void()> m_closer;
  std::atomic<bool> m_cancelled = false;
};
