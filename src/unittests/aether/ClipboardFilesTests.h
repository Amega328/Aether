/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTemporaryDir>
#include <QTest>

class ClipboardFilesTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void roundTrip();
  void directoryTree();
  void emptyInput();
  void unsafeNames();
  void truncatedData();
  void sizeLimit();
  void fileUriEncoding();
  void uriListParsing();
  void uriListSkipsNonLocal();
  void uriListFeedsPack();

private:
  QTemporaryDir m_dir;
  Log m_log;
};
