/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/PortalClipboard.h"

#include "aether/ClipboardFiles.h"
#include "base/Log.h"
#include "platform/EiClipboard.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <poll.h>
#include <unistd.h>
#include <utility>

#include <QBuffer>
#include <QByteArrayList>
#include <QDataStream>
#include <QFile>
#include <QImage>
#include <QList>
#include <QPair>
#include <QSet>
#include <QStandardPaths>
#include <QVarLengthArray>
#include <QtEndian>

namespace aether {

static constexpr int kBmpSignatureSize = 2;
static constexpr quint32 kBmpFileHeaderSize = 14;
static constexpr quint32 kMinDibHeaderSize = 12;

namespace {

namespace fs = std::filesystem;

// Pasted files are copied out of the staging dir, so it only needs to outlive
// a paste in progress.
constexpr auto kStagingMaxAge = std::chrono::hours(1);

struct StagedFiles
{
  size_t hash = 0;
  size_t size = 0;
  fs::path dir;
  std::vector<fs::path> topLevel;
};

fs::path stagingRoot()
{
  auto cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
  if (cache.isEmpty())
    return fs::temp_directory_path() / "Aether" / "clipboard";
  return fs::path(cache.toStdString()) / "Aether" / "clipboard";
}

void pruneStaging(const fs::path &root, bool all)
{
  std::error_code ec;
  const auto now = fs::file_time_type::clock::now();
  for (const auto &entry : fs::directory_iterator(root, ec)) {
    std::error_code entryEc;
    const auto modified = entry.last_write_time(entryEc);
    if (!all && !entryEc && now - modified < kStagingMaxAge)
      continue;
    fs::remove_all(entry.path(), entryEc);
  }
}

// Files received from the server only exist as a packed payload. File managers
// need real paths, so unpack on the first transfer request and reuse the result
// while the same payload stays on the clipboard (each mime asks separately).
std::vector<fs::path> stageFiles(const QByteArray &payload)
{
  static std::mutex mutex;
  static StagedFiles current;
  static uint32_t counter = 0;
  static bool firstUse = true;

  std::scoped_lock lock{mutex};

  const std::string_view view(payload.constData(), static_cast<size_t>(payload.size()));
  const auto hash = std::hash<std::string_view>{}(view);

  std::error_code ec;
  if (!current.dir.empty() && current.hash == hash && current.size == view.size() && fs::exists(current.dir, ec))
    return current.topLevel;

  const auto root = stagingRoot();
  pruneStaging(root, std::exchange(firstUse, false));

  const auto dir = root / ("clip-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
  try {
    current = {hash, view.size(), dir, aether::clipboard::unpackFiles(view, dir)};
  } catch (const std::exception &e) {
    LOG_WARN("failed to stage clipboard files: %s", e.what());
    fs::remove_all(dir, ec);
    current = {};
    return {};
  }
  return current.topLevel;
}

} // namespace

QByteArray PortalClipboard::formatMimeTypes(const char *const *mimeTypes)
{
  if (!mimeTypes || !mimeTypes[0])
    return QByteArrayLiteral("(none)");

  QByteArrayList parts;
  for (int i = 0; mimeTypes[i]; ++i)
    parts.append(mimeTypes[i]);
  return parts.join(", ");
}

const PortalClipboard::SupportedMime *PortalClipboard::findSupportedMime(const char *mime)
{
  if (!mime)
    return nullptr;

  for (const auto &entry : kSupportedMimes) {
    if (g_strcmp0(mime, entry.mime) == 0)
      return &entry;
  }

  return nullptr;
}

const PortalClipboard::SupportedMime *PortalClipboard::pickSupportedMime(const char *const *available)
{
  if (!available)
    return nullptr;

  for (const auto &entry : kSupportedMimes) {
    if (g_strv_contains(available, entry.mime))
      return &entry;
  }

  return nullptr;
}

QByteArray PortalClipboard::dibToBmp(const QByteArray &dib)
{
  if (dib.size() < static_cast<qint64>(sizeof(quint32)))
    return {};

  quint32 headerSize;
  std::memcpy(&headerSize, dib.constData(), sizeof(headerSize));
  headerSize = qFromLittleEndian(headerSize);
  if (headerSize < kMinDibHeaderSize || headerSize > static_cast<quint32>(dib.size()))
    return {};

  const auto fileSize = static_cast<quint32>(kBmpFileHeaderSize + dib.size());
  const quint32 pixelOffset = kBmpFileHeaderSize + headerSize;

  QByteArray bmp;
  QDataStream ds(&bmp, QIODevice::WriteOnly);
  ds.setByteOrder(QDataStream::LittleEndian);
  ds.writeRawData("BM", kBmpSignatureSize);
  ds << fileSize;
  ds << quint32(0);
  ds << pixelOffset;
  ds.writeRawData(dib.constData(), static_cast<int>(dib.size()));
  return bmp;
}

QByteArray PortalClipboard::bmpToDib(const QByteArray &bmp)
{
  if (bmp.size() < kBmpFileHeaderSize)
    return {};

  return bmp.mid(kBmpFileHeaderSize);
}

QByteArray PortalClipboard::encodeFormat(const SupportedMime &entry, const QByteArray &data)
{
  if (data.isEmpty())
    return {};

  if (entry.format == IClipboard::Format::Files) {
    const auto staged = stageFiles(data);
    if (staged.empty())
      return {};

    const bool gnome = g_strcmp0(entry.mime, kGnomeCopiedFilesMime) == 0;
    QByteArray list = gnome ? QByteArrayLiteral("copy") : QByteArray{};
    for (const auto &path : staged) {
      const auto uri = QByteArray::fromStdString(aether::clipboard::toFileUri(path));
      list += gnome ? "\n" + uri : uri + "\r\n";
    }
    return list;
  }

  if (entry.format == IClipboard::Format::Bitmap) {
    const auto bmpFile = dibToBmp(data);
    if (bmpFile.isEmpty()) {
      LOG_WARN("clipboard bitmap data is malformed");
      return {};
    }

    QImage image;
    if (!image.loadFromData(bmpFile, "BMP")) {
      LOG_WARN("failed to decode clipboard bitmap");
      return {};
    }

    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    if (!image.save(&buf, "PNG")) {
      LOG_WARN("failed to encode clipboard image as png");
      return {};
    }

    return png;
  }
  return data;
}

QByteArray PortalClipboard::decodeFormat(const SupportedMime &entry, const QByteArray &bytes, qint64 maxBytes)
{
  if (bytes.isEmpty())
    return {};

  if (entry.format == IClipboard::Format::Files) {
    const auto paths = aether::clipboard::fromUriList(std::string_view(bytes.constData(), bytes.size()));
    if (paths.empty()) {
      LOG_DEBUG("clipboard uri list has no local files");
      return {};
    }

    try {
      const auto limit = std::min<uint64_t>(static_cast<uint64_t>(maxBytes), aether::clipboard::kMaxPackBytes);
      return QByteArray::fromStdString(aether::clipboard::packFiles(paths, limit));
    } catch (const std::exception &e) {
      LOG_WARN("failed to pack clipboard files: %s", e.what());
      return {};
    }
  }

  if (entry.format == IClipboard::Format::Bitmap) {
    QImage image;
    if (!image.loadFromData(bytes, "PNG")) {
      LOG_WARN("failed to decode clipboard png");
      return {};
    }

    QByteArray bmp;
    QBuffer buf(&bmp);
    buf.open(QIODevice::WriteOnly);
    if (!image.save(&buf, "BMP")) {
      LOG_WARN("failed to encode clipboard image as bmp");
      return {};
    }

    return bmpToDib(bmp);
  }
  return bytes;
}

QByteArray PortalClipboard::readSelectionBytes(XdpSession *session, const char *mime, qint64 maxBytes)
{
  const int fd = xdp_session_selection_read(session, mime);
  if (fd < 0) {
    LOG_ERR("failed to read clipboard selection: invalid fd");
    return {};
  }

  QFile pipe;
  if (!pipe.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
    LOG_WARN("failed to wrap clipboard pipe");
    ::close(fd);
    return {};
  }

  QByteArray contents;
  contents.reserve(std::min<qint64>(maxBytes, kChunkBytes));
  while (contents.size() < maxBytes) {
    pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, kReadTimeoutMs) <= 0)
      break;

    const auto chunk = pipe.read(std::min<qint64>(kChunkBytes, maxBytes - contents.size()));
    if (chunk.isEmpty())
      break;

    contents.append(chunk);
  }
  return contents;
}

void PortalClipboard::claimOwnership(EiClipboard *cache, XdpSession *session)
{
  if (!cache || !session)
    return;

  cache->open(0);
  QVarLengthArray<const char *, std::size(kSupportedMimes) + 1> mimeTypes;
  for (const auto &entry : kSupportedMimes) {
    if (cache->has(entry.format))
      mimeTypes.append(entry.mime);
  }
  cache->close();

  if (mimeTypes.isEmpty()) {
    LOG_DEBUG("clipboard cache empty, nothing to claim");
    return;
  }
  mimeTypes.append(nullptr);

  LOG_DEBUG("claiming clipboard, mimes: %s", formatMimeTypes(mimeTypes.data()).constData());
  xdp_session_set_selection(session, mimeTypes.data());
}

void PortalClipboard::serveSelectionTransfer(EiClipboard *cache, XdpSession *session, const char *mime, uint32_t serial)
{
  LOG_DEBUG("clipboard selection transfer requested, mime: %s, serial: %u", mime, serial);

  const auto *requested = findSupportedMime(mime);
  if (!requested || !cache) {
    LOG_DEBUG("rejecting clipboard selection, unsupported mime: %s", mime);
    xdp_session_selection_write_done(session, serial, false);
    return;
  }

  cache->open(0);
  QByteArray raw;
  const bool hasFormat = cache->has(requested->format);
  if (hasFormat)
    raw = QByteArray::fromStdString(cache->get(requested->format));
  cache->close();

  const auto data = encodeFormat(*requested, raw);
  if (data.isEmpty()) {
    LOG_DEBUG("clipboard has no data for mime: %s", mime);
    xdp_session_selection_write_done(session, serial, false);
    return;
  }

  const int fd = xdp_session_selection_write(session, serial);
  if (fd < 0) {
    LOG_WARN("failed to open clipboard selection write fd");
    xdp_session_selection_write_done(session, serial, false);
    return;
  }

  QFile pipe;
  if (!pipe.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
    LOG_WARN("failed to wrap clipboard pipe");
    ::close(fd);
    xdp_session_selection_write_done(session, serial, false);
    return;
  }

  const char *buf = data.constData();
  qint64 total = data.size();
  qint64 written = 0;
  while (written < total) {
    pollfd pfd{fd, POLLOUT, 0};
    if (poll(&pfd, 1, kWriteTimeoutMs) <= 0) {
      LOG_ERR("timed out writing clipboard selection");
      xdp_session_selection_write_done(session, serial, false);
      return;
    }

    qint64 n = pipe.write(buf + written, total - written);
    if (n <= 0) {
      LOG_ERR("clipboard pipe write returned %lld", static_cast<long long>(n));
      xdp_session_selection_write_done(session, serial, false);
      return;
    }
    written += n;
  }

  xdp_session_selection_write_done(session, serial, true);
  LOG_DEBUG("clipboard selection transfer complete, bytes: %lld", static_cast<long long>(written));
}

bool PortalClipboard::readSelectionIntoCache(
    EiClipboard *cache, XdpSession *session, const char *const *mimeTypes, qint64 maxBytes
)
{
  if (!cache || !session || !mimeTypes || !mimeTypes[0])
    return false;

  if (!pickSupportedMime(mimeTypes)) {
    LOG_DEBUG("clipboard no supported mime types: %s", formatMimeTypes(mimeTypes).constData());
    return false;
  }

  QList<QPair<IClipboard::Format, QByteArray>> reads;
  QSet<IClipboard::Format> seen;
  for (const auto &entry : kSupportedMimes) {
    if (seen.contains(entry.format))
      continue;
    if (!g_strv_contains(mimeTypes, entry.mime))
      continue;

    auto bytes = readSelectionBytes(session, entry.mime, maxBytes);
    if (bytes.isEmpty()) {
      LOG_DEBUG("clipboard read returned no data for mime: %s", entry.mime);
      continue;
    }

    if (entry.format == IClipboard::Format::Text) {
      while (bytes.endsWith('\0'))
        bytes.chop(1);
      bytes.replace("\r\n", "\n");
    }

    auto data = decodeFormat(entry, bytes, maxBytes);
    if (data.isEmpty())
      continue;

    reads.append({entry.format, std::move(data)});
    seen.insert(entry.format);
  }

  if (reads.isEmpty()) {
    LOG_DEBUG("clipboard read produced no data, leaving existing clipboard intact");
    return false;
  }

  cache->open(0);
  cache->empty();
  for (const auto &[format, data] : reads)
    cache->add(format, data.toStdString());
  cache->close();

  LOG_DEBUG("clipboard read local selection, formats: %lld", static_cast<long long>(reads.size()));
  return true;
}

} // namespace aether
