/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ChangeDetectorTests.h"

#include "base/ChangeDetector.h"

#include <cstdint>

void ChangeDetectorTests::reportsOnlyChanges()
{
  // A button held at the screen edge is polled on every mouse move; only the press
  // and the release may be reported.
  ChangeDetector<uint32_t> lockingButton;
  int reports = 0;
  for (const uint32_t button : {0u, 0u, 1u, 1u, 1u, 1u, 1u, 3u, 3u, 0u, 0u}) {
    if (lockingButton.update(button)) {
      ++reports;
    }
  }
  QCOMPARE(reports, 3);
  QCOMPARE(lockingButton.value(), 0u);
}

QTEST_MAIN(ChangeDetectorTests)
