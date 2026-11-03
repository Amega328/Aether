/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTest>

class EiClipboardTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void sameThreadMayReopen();
  void secondThreadWaitsForClose();
  void readersNeverSeeAPartialCopy();

private:
  Log m_log;
};
