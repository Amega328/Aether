/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "net/BulkChannel.h"
#include "net/SecureUtils.h"

#include <QString>

#include <memory>
#include <string>
#include <thread>
#include <utility>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <openssl/ssl.h>

namespace aether::testing {

using ChannelPair = std::pair<std::unique_ptr<BulkChannel>, std::unique_ptr<BulkChannel>>;

#if !defined(_WIN32)

inline ChannelPair makeSocketPair(const std::string &pendingForFirst = {}, const std::string &pendingForSecond = {})
{
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return {};
  }
  return {
      std::make_unique<SocketBulkChannel>(fds[0], nullptr, pendingForFirst, [fd = fds[0]] { ::close(fd); }),
      std::make_unique<SocketBulkChannel>(fds[1], nullptr, pendingForSecond, [fd = fds[1]] { ::close(fd); })
  };
}

//! Two TLS channels talking over a local socket pair, using the certificate at \c certPath.
inline ChannelPair makeTlsPair(const QString &certPath)
{
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return {};
  }

  const auto path = certPath.toStdString();
  SSL_CTX *serverContext = SSL_CTX_new(TLS_server_method());
  SSL_CTX_use_certificate_file(serverContext, path.c_str(), SSL_FILETYPE_PEM);
  SSL_CTX_use_PrivateKey_file(serverContext, path.c_str(), SSL_FILETYPE_PEM);
  SSL_CTX_set_min_proto_version(serverContext, TLS1_2_VERSION);
  SSL_CTX *clientContext = SSL_CTX_new(TLS_client_method());
  SSL_CTX_set_min_proto_version(clientContext, TLS1_2_VERSION);

  SSL *server = SSL_new(serverContext);
  SSL *client = SSL_new(clientContext);
  SSL_CTX_free(serverContext);
  SSL_CTX_free(clientContext);
  SSL_set_fd(server, fds[0]);
  SSL_set_fd(client, fds[1]);

  int acceptResult = 0;
  std::thread acceptor([&] { acceptResult = SSL_accept(server); });
  const int connectResult = SSL_connect(client);
  acceptor.join();

  if (acceptResult != 1 || connectResult != 1) {
    SSL_free(server);
    SSL_free(client);
    ::close(fds[0]);
    ::close(fds[1]);
    return {};
  }

  return {
      std::make_unique<SocketBulkChannel>(fds[0], server, std::string{}, [fd = fds[0]] { ::close(fd); }),
      std::make_unique<SocketBulkChannel>(fds[1], client, std::string{}, [fd = fds[1]] { ::close(fd); })
  };
}

#endif

} // namespace aether::testing
