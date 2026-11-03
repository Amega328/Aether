/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTemporaryDir>
#include <QTest>

class PortalClipboardTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void mimeTableMapsFiles();
  void filesBecomeUriList();
  void filesBecomeGnomeCopiedFiles();
  void stagingIsReusedAndRecreated();
  void uriListPacksFiles();
  void packRespectsSizeLimit();
  void invalidInputYieldsNothing();
  void textPassesThrough();

private:
  QTemporaryDir m_dir;
  Log m_log;
};
