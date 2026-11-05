/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/Arch.h"
#include "base/Log.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>

class TransferConnectorTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();

  void pullsFilesThroughRealSockets_data();
  void pullsFilesThroughRealSockets();
  void pushesFilesThroughRealSockets_data();
  void pushesFilesThroughRealSockets();
  void serverHangingUpFailsThePull();
  void refusedConnectionFailsThePull();

private:
  QTemporaryDir m_dir;
  std::unique_ptr<QObject> m_coreIpc;
  Arch m_arch;
  Log m_log;
};
