/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClipboardFilesTests.h"

#include "aether/ClipboardFiles.h"

#include <fstream>

namespace fs = std::filesystem;
using aether::clipboard::ClipboardFilesError;
using aether::clipboard::packFiles;
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

} // namespace

void ClipboardFilesTests::initTestCase()
{
  m_log.setFilter(LogLevel::Level::Verbose);
}

void ClipboardFilesTests::roundTrip()
{
  const auto base = fs::path(m_dir.path().toStdWString()) / "roundTrip";
  const auto source = base / "source";
  const auto staging = base / "staging";

  writeFile(source / "hello.txt", "hello world");
  const auto packed = packFiles({source / "hello.txt"});
  QVERIFY(!packed.empty());

  const auto staged = unpackFiles(packed, staging);
  QCOMPARE(staged.size(), 1);
  QCOMPARE(readFile(staged.front()), "hello world");
}

void ClipboardFilesTests::directoryTree()
{
  const auto base = fs::path(m_dir.path().toStdWString()) / "tree";
  const auto source = base / "source";
  const auto staging = base / "staging";

  writeFile(source / "folder" / "sub" / "a.txt", "a");
  writeFile(source / "folder" / "b.txt", "bb");

  const auto packed = packFiles({source / "folder"});
  QVERIFY(!packed.empty());

  const auto staged = unpackFiles(packed, staging);
  QCOMPARE(staged.size(), 1);
  QCOMPARE(readFile(staging / "folder" / "sub" / "a.txt"), "a");
  QCOMPARE(readFile(staging / "folder" / "b.txt"), "bb");
}

void ClipboardFilesTests::emptyInput()
{
  QCOMPARE(packFiles({}), std::string{});
}

void ClipboardFilesTests::unsafeNames()
{
  const auto staging = fs::path(m_dir.path().toStdWString()) / "unsafe";

  // count=1, flags=0 (file), name="../evil.txt", size=0
  std::string evil;
  auto u32 = [&evil](uint32_t v) {
    for (int i = 3; i >= 0; --i)
      evil += static_cast<char>((v >> (i * 8)) & 0xff);
  };
  u32(1);
  evil += '\0';
  const std::string name = "../evil.txt";
  u32(static_cast<uint32_t>(name.size()));
  evil += name;
  u32(0);
  u32(0);

  QVERIFY_EXCEPTION_THROWN(unpackFiles(evil, staging), ClipboardFilesError);
  QVERIFY(!fs::exists(staging.parent_path() / "evil.txt"));
}

void ClipboardFilesTests::truncatedData()
{
  const auto base = fs::path(m_dir.path().toStdWString()) / "trunc";
  const auto source = base / "source";

  writeFile(source / "f.txt", "0123456789");
  const auto packed = packFiles({source / "f.txt"});
  QVERIFY(packed.size() > 4);

  const auto staging = base / "staging";
  QVERIFY_EXCEPTION_THROWN(
      unpackFiles(std::string_view(packed).substr(0, packed.size() - 5), staging), ClipboardFilesError
  );
}

void ClipboardFilesTests::sizeLimit()
{
  const auto base = fs::path(m_dir.path().toStdWString()) / "limit";
  const auto source = base / "source";

  writeFile(source / "big.bin", std::string(1024, 'x'));
  QVERIFY_EXCEPTION_THROWN(packFiles({source / "big.bin"}, 512), ClipboardFilesError);
}

void ClipboardFilesTests::fileUriEncoding()
{
  using aether::clipboard::fromUriList;
  using aether::clipboard::toFileUri;

  QCOMPARE(toFileUri("/home/u/a.txt"), "file:///home/u/a.txt");
  QCOMPARE(toFileUri("/home/u/my file #1 (100%).txt"), "file:///home/u/my%20file%20%231%20%28100%25%29.txt");
  QCOMPARE(toFileUri("/tmp/\xC3\xA9.txt"), "file:///tmp/%C3%A9.txt");

  for (const char *path : {"/home/u/my file #1 (100%).txt", "/tmp/\xC3\xA9.txt", "/a/b+c&d=e.txt"}) {
    const auto parsed = fromUriList(toFileUri(path) + "\r\n");
    QCOMPARE(parsed.size(), 1);
    QCOMPARE(parsed.front().generic_string(), std::string(path));
  }
}

void ClipboardFilesTests::uriListParsing()
{
  using aether::clipboard::fromUriList;

  const auto crlf = fromUriList("file:///a/one.txt\r\nfile:///a/two%20x.txt\r\n");
  QCOMPARE(crlf.size(), 2);
  QCOMPARE(crlf[1].generic_string(), std::string("/a/two x.txt"));

  QCOMPARE(fromUriList("file:///a/one.txt\nfile:///a/two.txt").size(), 2);

  // gnome-copied-files: the action line is not a URI and must be ignored
  const auto gnome = fromUriList("copy\nfile:///a/one.txt\nfile:///a/dir");
  QCOMPARE(gnome.size(), 2);
  QCOMPARE(gnome[1].filename().generic_string(), std::string("dir"));

  // directory URIs often end in a slash; filename() must not come out empty
  const auto dir = fromUriList("file:///a/dir/\r\n");
  QCOMPARE(dir.size(), 1);
  QCOMPARE(dir.front().filename().generic_string(), std::string("dir"));

  QCOMPARE(fromUriList("FILE:///a/upper.txt").size(), 1);
  QCOMPARE(fromUriList("file:/a/single-slash.txt").size(), 1);
  QCOMPARE(fromUriList("file://localhost/a/host.txt").size(), 1);
  QVERIFY(fromUriList("").empty());
}

void ClipboardFilesTests::uriListSkipsNonLocal()
{
  using aether::clipboard::fromUriList;

  QVERIFY(fromUriList("# comment\r\n\r\n").empty());
  QVERIFY(fromUriList("https://example.com/a.txt\r\n").empty());
  QVERIFY(fromUriList("file://otherhost/share/a.txt\r\n").empty());
  QVERIFY(fromUriList("file:///a/bad%2.txt\r\n").empty());
  QVERIFY(fromUriList("file:///a/bad%zz.txt\r\n").empty());
  QVERIFY(fromUriList("file:///a/nul%00.txt\r\n").empty());
  QVERIFY(fromUriList("file:///a/trailing%").empty());
  QVERIFY(fromUriList("file://").empty());
  QVERIFY(fromUriList("relative/path.txt").empty());

  // good lines survive their bad neighbours
  QCOMPARE(fromUriList("https://example.com/\r\nfile:///a/ok.txt\r\nfile://otherhost/x\r\n").size(), 1);
}

void ClipboardFilesTests::uriListFeedsPack()
{
  using aether::clipboard::fromUriList;
  using aether::clipboard::toFileUri;

  const auto base = fs::path(m_dir.path().toStdWString()) / "uriPack";
  const auto source = base / "source";
  const auto staging = base / "staging";

  writeFile(source / "my folder" / "inner file.txt", "inner");
  writeFile(source / "top.txt", "top");

  // A trailing slash on the directory URI is how some file managers write it.
  const auto list = toFileUri(source / "my folder") + "/\r\n" + toFileUri(source / "top.txt") + "\r\n";
  const auto packed = packFiles(fromUriList(list));
  QVERIFY(!packed.empty());

  const auto staged = unpackFiles(packed, staging);
  QCOMPARE(staged.size(), 2);
  QCOMPARE(readFile(staging / "my folder" / "inner file.txt"), "inner");
  QCOMPARE(readFile(staging / "top.txt"), "top");
}

QTEST_MAIN(ClipboardFilesTests)
