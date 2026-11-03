/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "aether/IClipboard.h"

#include <QByteArray>

#include <libportal/portal.h>

namespace aether {

class EiClipboard;

class PortalClipboard
{
public:
  struct SupportedMime
  {
    const char *mime;
    IClipboard::Format format;
  };

  static constexpr const char *kGnomeCopiedFilesMime = "x-special/gnome-copied-files";

  // Listed in preference order: richer formats first. Files maps to two mimes:
  // text/uri-list for most toolkits, and gnome-copied-files for GTK file managers.
  static constexpr SupportedMime kSupportedMimes[] = {
      {"text/uri-list", IClipboard::Format::Files}, {kGnomeCopiedFilesMime, IClipboard::Format::Files},
      {"image/png", IClipboard::Format::Bitmap},    {"text/plain;charset=utf-8", IClipboard::Format::Text},
      {"text/plain", IClipboard::Format::Text},
  };

  static constexpr int kReadTimeoutMs = 200;
  static constexpr int kWriteTimeoutMs = 200;
  static constexpr qint64 kChunkBytes = 64 * 1024;

  static QByteArray formatMimeTypes(const char *const *mimeTypes);
  static const SupportedMime *findSupportedMime(const char *mime);
  static const SupportedMime *pickSupportedMime(const char *const *available);
  /// Convert cached clipboard data to the wire form of \p entry. For Files this
  /// stages the received payload on disk and returns a list of file URIs.
  static QByteArray encodeFormat(const SupportedMime &entry, const QByteArray &data);

  /// Convert bytes read from the portal into cached clipboard data. For Files
  /// this packs the files named by the URI list, up to \p maxBytes of content.
  static QByteArray decodeFormat(const SupportedMime &entry, const QByteArray &bytes, qint64 maxBytes);
  static QByteArray readSelectionBytes(XdpSession *session, const char *mime, qint64 maxBytes);

  /// Advertise the cache's formats to the portal selection.
  static void claimOwnership(EiClipboard *cache, XdpSession *session);

  /// Respond to a selection-transfer signal by writing the cache's bytes for
  /// \p mime to the portal-provided fd. Always calls write_done.
  static void serveSelectionTransfer(EiClipboard *cache, XdpSession *session, const char *mime, uint32_t serial);

  /// Read every supported format offered by the portal into the cache.
  /// Returns true if any data was deposited.
  static bool
  readSelectionIntoCache(EiClipboard *cache, XdpSession *session, const char *const *mimeTypes, qint64 maxBytes);

private:
  static QByteArray dibToBmp(const QByteArray &dib);
  static QByteArray bmpToDib(const QByteArray &bmp);
};

} // namespace aether
