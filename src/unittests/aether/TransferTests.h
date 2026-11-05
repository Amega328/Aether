/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTemporaryDir>
#include <QTest>

class TransferTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();

  void tokenHexRoundTrip();
  void manifestRoundTrip();
  void manifestCompressesLargeListings();
  void manifestRejectsMalformed();
  void tokenIsStableAndDistinct();
  void topLevelNameClashesAreMadeUnique();
  void unitsCoverEveryByteOnce();
  void streamCountPolicy();

  void transferWithOneStream();
  void transferWithManyStreams();
  void transferOfEmptySelection();
  void senderFailsWhenFileShrinks();
  void receiverRejectsDuplicateStream();
  void headerRejectsGarbage();
  void headerCarriesManifest();

  void benchmarkTlsTransfer();

private:
  QTemporaryDir m_dir;
  Log m_log;
};
