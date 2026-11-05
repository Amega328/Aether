/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "PortalClipboardTests.h"

#include "aether/ClipboardFiles.h"
#include "aether/TransferManifest.h"
#include "platform/PortalClipboard.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using aether::PortalClipboard;
using aether::clipboard::fromUriList;
using aether::clipboard::packFiles;
using aether::clipboard::toFileUri;
using aether::clipboard::unpackFiles;

namespace {

void writeFile(const fs::path &path, const std::string &content)
{
  fs::create_directories(path.parent_path());
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string readFile(const fs::path &path)
{
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

const PortalClipboard::SupportedMime &mime(const char *name)
{
  const auto *entry = PortalClipboard::findSupportedMime(name);
  Q_ASSERT(entry);
  return *entry;
}

std::vector<fs::path> pathsIn(const QByteArray &list)
{
  return fromUriList(std::string_view(list.constData(), static_cast<size_t>(list.size())));
}

QByteArray samplePayload(const fs::path &source)
{
  writeFile(source / "docs" / "note.txt", "note");
  writeFile(source / "photo.bin", "bits");
  return QByteArray::fromStdString(packFiles({source / "docs", source / "photo.bin"}));
}

} // namespace

void PortalClipboardTests::initTestCase()
{
  m_log.setFilter(LogLevel::Level::Verbose);
  QVERIFY(m_dir.isValid());
  qputenv("XDG_CACHE_HOME", QFile::encodeName(m_dir.filePath("cache")));
}

void PortalClipboardTests::mimeTableMapsFiles()
{
  QCOMPARE(mime("text/uri-list").format, IClipboard::Format::Files);
  QCOMPARE(mime(PortalClipboard::kGnomeCopiedFilesMime).format, IClipboard::Format::Files);

  const char *const offered[] = {"text/plain", "text/uri-list", nullptr};
  const auto *picked = PortalClipboard::pickSupportedMime(offered);
  QVERIFY(picked);
  QCOMPARE(picked->format, IClipboard::Format::Files);
}

void PortalClipboardTests::filesBecomeUriList()
{
  const auto payload = samplePayload(fs::path(m_dir.path().toStdWString()) / "uriList");

  const auto list = PortalClipboard::encodeFormat(mime("text/uri-list"), payload);
  QVERIFY(list.endsWith("\r\n"));

  const auto paths = pathsIn(list);
  QCOMPARE(paths.size(), 2);
  QCOMPARE(paths[0].filename().generic_string(), std::string("docs"));
  QCOMPARE(readFile(paths[0] / "note.txt"), "note");
  QCOMPARE(paths[1].filename().generic_string(), std::string("photo.bin"));
  QCOMPARE(readFile(paths[1]), "bits");

  const auto cacheRoot = m_dir.filePath("cache").toStdString();
  QVERIFY(paths[0].generic_string().starts_with(cacheRoot));
}

void PortalClipboardTests::filesBecomeGnomeCopiedFiles()
{
  const auto payload = samplePayload(fs::path(m_dir.path().toStdWString()) / "gnome");

  const auto list = PortalClipboard::encodeFormat(mime(PortalClipboard::kGnomeCopiedFilesMime), payload);
  QVERIFY(list.startsWith("copy\nfile://"));
  QVERIFY(!list.endsWith('\n'));
  QVERIFY(!list.contains('\r'));

  const auto paths = pathsIn(list);
  QCOMPARE(paths.size(), 2);
  QCOMPARE(readFile(paths[1]), "bits");
}

void PortalClipboardTests::stagingIsReusedAndRecreated()
{
  const auto base = fs::path(m_dir.path().toStdWString()) / "reuse";
  const auto payload = samplePayload(base / "one");

  // Each mime asks separately, but must see the same staged files.
  const auto first = PortalClipboard::encodeFormat(mime("text/uri-list"), payload);
  const auto second = PortalClipboard::encodeFormat(mime("text/uri-list"), payload);
  QVERIFY(!first.isEmpty());
  QCOMPARE(first, second);

  const auto stagingDir = pathsIn(first).front().parent_path();

  // A different clipboard gets its own directory.
  writeFile(base / "two" / "other.txt", "other");
  const auto otherPayload = QByteArray::fromStdString(packFiles({base / "two" / "other.txt"}));
  const auto other = PortalClipboard::encodeFormat(mime("text/uri-list"), otherPayload);
  const auto otherPaths = pathsIn(other);
  QCOMPARE(otherPaths.size(), 1);
  QVERIFY(otherPaths.front().parent_path() != stagingDir);
  QCOMPARE(readFile(otherPaths.front()), "other");

  // If the staging dir disappears (cache cleaned), the next request restages it.
  fs::remove_all(otherPaths.front().parent_path());
  const auto again = pathsIn(PortalClipboard::encodeFormat(mime("text/uri-list"), otherPayload));
  QCOMPARE(again.size(), 1);
  QCOMPARE(readFile(again.front()), "other");
}

void PortalClipboardTests::uriListPacksFiles()
{
  const auto base = fs::path(m_dir.path().toStdWString()) / "pack";
  const auto source = base / "source";
  writeFile(source / "my folder" / "inner file.txt", "inner");
  writeFile(source / "top.txt", "top");

  const auto list =
      QByteArray::fromStdString(toFileUri(source / "my folder") + "/\r\n" + toFileUri(source / "top.txt") + "\r\n");
  const auto packed = PortalClipboard::decodeFormat(mime("text/uri-list"), list, 1024 * 1024);
  QVERIFY(!packed.isEmpty());

  const auto staging = base / "staging";
  const auto staged = unpackFiles(packed.toStdString(), staging);
  QCOMPARE(staged.size(), 2);
  QCOMPARE(readFile(staging / "my folder" / "inner file.txt"), "inner");
  QCOMPARE(readFile(staging / "top.txt"), "top");

  // gnome-copied-files bodies decode the same way
  const auto gnome = "copy\n" + QByteArray::fromStdString(toFileUri(source / "top.txt"));
  const auto gnomePacked = PortalClipboard::decodeFormat(mime(PortalClipboard::kGnomeCopiedFilesMime), gnome, 1024);
  QVERIFY(!gnomePacked.isEmpty());
}

void PortalClipboardTests::packRespectsSizeLimit()
{
  const auto source = fs::path(m_dir.path().toStdWString()) / "limit";
  writeFile(source / "big.bin", std::string(4096, 'x'));

  const auto list = QByteArray::fromStdString(toFileUri(source / "big.bin") + "\r\n");
  // Over the inline limit the clipboard carries a transfer manifest instead of the contents.
  const auto overLimit = PortalClipboard::decodeFormat(mime("text/uri-list"), list, 1024);
  QVERIFY(!overLimit.isEmpty());
  QVERIFY(aether::transfer::isManifestPayload(overLimit.toStdString()));
  QVERIFY(overLimit.size() < 512);

  const auto inlined = PortalClipboard::decodeFormat(mime("text/uri-list"), list, 1024 * 1024);
  QVERIFY(!inlined.isEmpty());
  QVERIFY(!aether::transfer::isManifestPayload(inlined.toStdString()));
  QVERIFY(inlined.size() > 4096);
}

void PortalClipboardTests::invalidInputYieldsNothing()
{
  QVERIFY(PortalClipboard::encodeFormat(mime("text/uri-list"), "this is not a files payload").isEmpty());
  QVERIFY(PortalClipboard::encodeFormat(mime("text/uri-list"), QByteArray{}).isEmpty());

  QVERIFY(PortalClipboard::decodeFormat(mime("text/uri-list"), "https://example.com/a.txt\r\n", 1024).isEmpty());
  QVERIFY(PortalClipboard::decodeFormat(mime("text/uri-list"), "file:///does/not/exist.txt\r\n", 1024).isEmpty());
  QVERIFY(PortalClipboard::decodeFormat(mime("text/uri-list"), QByteArray{}, 1024).isEmpty());
}

void PortalClipboardTests::textPassesThrough()
{
  QCOMPARE(PortalClipboard::encodeFormat(mime("text/plain"), "hello"), QByteArray("hello"));
  QCOMPARE(PortalClipboard::decodeFormat(mime("text/plain"), "hello", 1024), QByteArray("hello"));
}

QTEST_MAIN(PortalClipboardTests)
