/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/BulkChannel.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace {

constexpr int kIoTimeoutSeconds = 30;
constexpr size_t kMaxIoChunk = 1u << 30;

#if defined(_WIN32)
constexpr int kSendFlags = 0;
#elif defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// OpenSSL writes with plain write(), so a peer that went away raises SIGPIPE
// on the calling thread. Block it once per thread that moves bulk data.
void blockSigpipeOnThisThread()
{
#if !defined(_WIN32)
  thread_local const bool blocked = [] {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    return true;
  }();
  (void)blocked;
#endif
}

void configureBlocking(SocketBulkChannel::NativeSocket socket)
{
#if defined(_WIN32)
  u_long nonBlocking = 0;
  ioctlsocket(socket, FIONBIO, &nonBlocking);

  const DWORD timeoutMs = kIoTimeoutSeconds * 1000;
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));

  // Winsock's default send buffer is small enough to cap throughput on
  // links with a noticeable round trip time.
  const int sendBuffer = 2 * 1024 * 1024;
  setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&sendBuffer), sizeof(sendBuffer));
#else
  if (const int flags = fcntl(socket, F_GETFL, 0); flags != -1) {
    fcntl(socket, F_SETFL, flags & ~O_NONBLOCK);
  }

  const timeval timeout{kIoTimeoutSeconds, 0};
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

bool isInterrupted()
{
#if defined(_WIN32)
  return WSAGetLastError() == WSAEINTR;
#else
  return errno == EINTR;
#endif
}

std::string lastSocketError()
{
#if defined(_WIN32)
  return "winsock error " + std::to_string(WSAGetLastError());
#else
  return std::strerror(errno);
#endif
}

std::string tlsError(const char *what, int code)
{
  std::string message = std::string(what) + " failed";
  if (const auto queued = ERR_get_error(); queued != 0) {
    char text[256] = {};
    ERR_error_string_n(queued, text, sizeof(text));
    message += std::string(": ") + text;
  } else if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
    message += ": timed out";
  } else {
    message += ": " + lastSocketError();
  }
  return message;
}

} // namespace

void BulkChannel::readFully(void *buffer, size_t n)
{
  auto *out = static_cast<char *>(buffer);
  while (n > 0) {
    const size_t got = read(out, n);
    if (got == 0) {
      throw BulkChannelError("connection closed before all data arrived");
    }
    out += got;
    n -= got;
  }
}

SocketBulkChannel::SocketBulkChannel(
    NativeSocket socket, ssl_st *ssl, std::string pending, std::function<void()> closer
)
    : m_socket(socket),
      m_ssl(ssl),
      m_pending(std::move(pending)),
      m_closer(std::move(closer))
{
  configureBlocking(m_socket);
  if (m_ssl != nullptr) {
    // Transfers carry their own lengths and acknowledgements, so a peer closing
    // without a TLS close_notify is just end of stream.
    SSL_set_options(m_ssl, SSL_OP_IGNORE_UNEXPECTED_EOF);
  }
}

SocketBulkChannel::~SocketBulkChannel()
{
  if (m_ssl != nullptr) {
    SSL_set_quiet_shutdown(m_ssl, 1);
    SSL_shutdown(m_ssl);
    SSL_free(m_ssl);
  }
  if (m_closer) {
    try {
      m_closer();
    } catch (...) {
    }
  }
}

size_t SocketBulkChannel::read(void *buffer, size_t n)
{
  blockSigpipeOnThisThread();
  if (n == 0) {
    return 0;
  }

  if (m_pendingOffset < m_pending.size()) {
    const size_t take = std::min(n, m_pending.size() - m_pendingOffset);
    std::memcpy(buffer, m_pending.data() + m_pendingOffset, take);
    m_pendingOffset += take;
    if (m_pendingOffset == m_pending.size()) {
      m_pending = {};
      m_pendingOffset = 0;
    }
    return take;
  }

  const size_t chunk = std::min(n, kMaxIoChunk);
  for (;;) {
    if (m_cancelled) {
      throw BulkChannelError("transfer cancelled");
    }

    if (m_ssl != nullptr) {
      ERR_clear_error();
      const int got = SSL_read(m_ssl, buffer, static_cast<int>(chunk));
      if (got > 0) {
        return static_cast<size_t>(got);
      }
      const int code = SSL_get_error(m_ssl, got);
      if (code == SSL_ERROR_ZERO_RETURN || (code == SSL_ERROR_SYSCALL && got == 0 && ERR_peek_error() == 0)) {
        return 0;
      }
      throw BulkChannelError(tlsError("tls read", code));
    }

    const auto got = ::recv(m_socket, static_cast<char *>(buffer), static_cast<int>(chunk), 0);
    if (got >= 0) {
      return static_cast<size_t>(got);
    }
    if (!isInterrupted()) {
      throw BulkChannelError("socket read failed: " + lastSocketError());
    }
  }
}

void SocketBulkChannel::write(const void *buffer, size_t n)
{
  blockSigpipeOnThisThread();
  const auto *data = static_cast<const char *>(buffer);
  while (n > 0) {
    if (m_cancelled) {
      throw BulkChannelError("transfer cancelled");
    }

    const size_t chunk = std::min(n, kMaxIoChunk);
    if (m_ssl != nullptr) {
      ERR_clear_error();
      const int wrote = SSL_write(m_ssl, data, static_cast<int>(chunk));
      if (wrote <= 0) {
        throw BulkChannelError(tlsError("tls write", SSL_get_error(m_ssl, wrote)));
      }
      data += wrote;
      n -= static_cast<size_t>(wrote);
      continue;
    }

    const auto wrote = ::send(m_socket, data, static_cast<int>(chunk), kSendFlags);
    if (wrote < 0) {
      if (isInterrupted()) {
        continue;
      }
      throw BulkChannelError("socket write failed: " + lastSocketError());
    }
    data += wrote;
    n -= static_cast<size_t>(wrote);
  }
}

void SocketBulkChannel::cancel()
{
  m_cancelled = true;
#if defined(_WIN32)
  ::shutdown(m_socket, SD_BOTH);
#else
  ::shutdown(m_socket, SHUT_RDWR);
#endif
}
