/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTemporaryDir>
#include <QTest>

class TransferHubTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();

  void helloNameRoundTrip();
  void helloNameRejectsMalformed();
  void pullsFilesTheServerOffered();
  void pushesFilesTheClientOffered();
  void pushCreatesTheSessionFromTheHeader();
  void relaysReceivedFilesToAnotherClient();
  void ownedManifestNeedsNoTransfer();
  void recognizesStagedFiles();
  void retriesAfterFailure();
  void evictsOldTransfers();
  void shutdownCancelsRunningTransfers();

private:
  QTemporaryDir m_dir;
  Log m_log;
};
