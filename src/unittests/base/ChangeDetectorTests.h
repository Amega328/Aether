/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class ChangeDetectorTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void reportsOnlyChanges();
};
