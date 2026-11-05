/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "arch/Arch.h"
#include "base/Log.h"

#include <QTemporaryDir>
#include <QTest>

class BulkChannelTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();

  void plainRoundTrip();
  void plainServesPendingBytesFirst();
  void plainEndOfStreamReadsZero();
  void plainReadFullyFailsOnEarlyEnd();
  void plainCancelUnblocksReader();
  void tlsRoundTrip();
  void tlsLargeTransfer();
  void tlsCancelUnblocksReader();
  void detachesLiveTcpSockets();

private:
  QTemporaryDir m_dir;
  QString m_certPath;
  Arch m_arch;
  Log m_log;
};
