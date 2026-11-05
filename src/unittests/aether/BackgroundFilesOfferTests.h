/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/Log.h"

#include <QTest>

class BackgroundFilesOfferTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void quickSelectionIsReturnedDirectly();
  void slowSelectionIsDeferredAndAnnounced();
  void sameSelectionIsListedOnce();
  void newGenerationIsListedAgain();
  void listingErrorsAreReported();
  void supersededListingIsNotAnnounced();
  void shutdownSilencesPendingListing();

private:
  Log m_log;
};
