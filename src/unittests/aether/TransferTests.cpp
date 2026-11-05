/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "TransferTests.h"

#include "../net/TlsSocketPair.h"
#include "TransferTestSupport.h"
#include "aether/TransferStream.h"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace fs = std::filesystem;
using namespace aether::testing;
using namespace aether::transfer;

namespace {

struct TransferOutcome
{
  std::vector<std::string> errors;

  const char *firstError() const
  {
    return errors.empty() ? "" : errors.front().c_str();
  }
};

using ChannelFactory = std::function<ChannelPair()>;

TransferOutcome runTransfer(
    const Source &source, ReceiveSession &session, unsigned streams, const ChannelFactory &makeChannels = makePipePair
)
{
  TransferOutcome outcome;
  std::mutex errorMutex;
  std::vector<std::thread> threads;

  const auto record = [&](const char *who, const std::exception &e) {
    std::scoped_lock lock{errorMutex};
    outcome.errors.push_back(std::string(who) + ": " + e.what());
  };

  for (unsigned k = 0; k < streams; ++k) {
    auto pair = makeChannels();
    threads.emplace_back([&, k, channel = std::move(pair.first)] {
      try {
        sendStream(*channel, source, k, streams, k == 0);
      } catch (const std::exception &e) {
        record("sender", e);
      }
    });
    threads.emplace_back([&, channel = std::move(pair.second)] {
      try {
        const auto header = readStreamHeader(*channel);
        session.receive(*channel, header.index, header.count);
      } catch (const std::exception &e) {
        record("receiver", e);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  return outcome;
}

fs::path testRoot(QTemporaryDir &dir, const char *name)
{
  return fs::path(dir.path().toStdWString()) / name;
}

} // namespace

void TransferTests::initTestCase()
{
  m_log.setFilter(LogLevel::Level::Verbose);
}

void TransferTests::tokenHexRoundTrip()
{
  Token token{};
  for (size_t i = 0; i < token.size(); ++i) {
    token[i] = static_cast<uint8_t>(i * 17 + 3);
  }
  const auto hex = tokenToHex(token);
  QCOMPARE(hex.size(), size_t{32});
  QVERIFY(tokenFromHex(hex) == token);

  QVERIFY(!tokenFromHex("").has_value());
  QVERIFY(!tokenFromHex(hex.substr(2)).has_value());
  QVERIFY(!tokenFromHex(std::string(32, 'g')).has_value());
  QVERIFY(!tokenFromHex(std::string(32, 'A')).has_value());
}

void TransferTests::manifestRoundTrip()
{
  const auto root = testRoot(m_dir, "manifestRoundTrip");
  writePatternFile(root / "docs" / "a.txt", 100, 1);
  writePatternFile(root / "docs" / "sub" / "b.bin", 5000, 2);
  fs::create_directories(root / "docs" / "empty");
  writePatternFile(root / "loose.dat", 42, 3);

  const auto source = buildSource({root / "docs", root / "loose.dat"});
  QCOMPARE(source.manifest.entries.size(), size_t{6});
  QCOMPARE(source.paths.size(), source.manifest.entries.size());
  QCOMPARE(source.manifest.totalBytes(), uint64_t{5142});

  const auto payload = encodeManifest(source.manifest);
  QVERIFY(isManifestPayload(payload));

  const auto decoded = decodeManifest(payload);
  QVERIFY(decoded.token == source.manifest.token);
  QCOMPARE(decoded.entries.size(), source.manifest.entries.size());
  for (size_t i = 0; i < decoded.entries.size(); ++i) {
    QCOMPARE(decoded.entries[i].name, source.manifest.entries[i].name);
    QCOMPARE(decoded.entries[i].size, source.manifest.entries[i].size);
    QCOMPARE(decoded.entries[i].directory, source.manifest.entries[i].directory);
    QCOMPARE(decoded.entries[i].modified, source.manifest.entries[i].modified);
  }

  const auto names = topLevelNames(decoded);
  QCOMPARE(names.size(), size_t{2});
  QCOMPARE(names[0], std::string("docs"));
  QCOMPARE(names[1], std::string("loose.dat"));
}

void TransferTests::manifestCompressesLargeListings()
{
  Manifest manifest;
  for (int i = 0; i < 3000; ++i) {
    Entry entry;
    entry.name = "project/src/module" + std::to_string(i / 100) + "/file" + std::to_string(i) + ".cpp";
    entry.size = 1000 + i;
    entry.modified = 1700000000 + i;
    manifest.entries.push_back(entry);
  }
  manifest.token[0] = 7;

  const auto payload = encodeManifest(manifest);
  size_t rawEstimate = 0;
  for (const auto &entry : manifest.entries) {
    rawEstimate += entry.name.size() + 19;
  }
  QVERIFY2(payload.size() < rawEstimate / 2, "manifest should compress well");

  const auto decoded = decodeManifest(payload);
  QCOMPARE(decoded.entries.size(), manifest.entries.size());
  QCOMPARE(decoded.entries.back().name, manifest.entries.back().name);
}

void TransferTests::manifestRejectsMalformed()
{
  Manifest manifest;
  manifest.entries.push_back({"ok.txt", 10, 0, false});
  const auto good = encodeManifest(manifest);
  QVERIFY(!decodeManifest(good).entries.empty());

  const auto rejects = [](const std::string &payload) {
    try {
      decodeManifest(payload);
    } catch (const TransferError &) {
      return true;
    }
    return false;
  };

  QVERIFY(rejects(""));
  QVERIFY(rejects(good.substr(0, good.size() - 3)));
  QVERIFY(rejects(good + "x"));

  auto badMagic = good;
  badMagic[0] = 'X';
  QVERIFY(rejects(badMagic));

  for (const char *name : {"../escape.txt", "/abs.txt", "a/../../b", "C:evil", "back\\slash", "star*.txt", ""}) {
    Manifest bad;
    bad.entries.push_back({name, 1, 0, false});
    QVERIFY2(rejects(encodeManifest(bad)), name);
  }

  Manifest duplicate;
  duplicate.entries.push_back({"same.txt", 1, 0, false});
  duplicate.entries.push_back({"same.txt", 2, 0, false});
  QVERIFY(rejects(encodeManifest(duplicate)));

  Manifest directoryWithContent;
  directoryWithContent.entries.push_back({"dir", 5, 0, true});
  QVERIFY(rejects(encodeManifest(directoryWithContent)));

  Manifest absurd;
  absurd.entries.push_back({"huge.bin", kMaxTotalBytes + 1, 0, false});
  QVERIFY(rejects(encodeManifest(absurd)));
}

void TransferTests::tokenIsStableAndDistinct()
{
  const auto root = testRoot(m_dir, "tokens");
  writePatternFile(root / "one.txt", 10, 1);
  writePatternFile(root / "two.txt", 20, 2);

  const auto first = buildSource({root / "one.txt"});
  const auto again = buildSource({root / "one.txt"});
  const auto other = buildSource({root / "two.txt"});
  QVERIFY(first.manifest.token == again.manifest.token);
  QVERIFY(first.manifest.token != other.manifest.token);

  writePatternFile(root / "one.txt", 11, 1);
  const auto changed = buildSource({root / "one.txt"});
  QVERIFY(changed.manifest.token != first.manifest.token);
}

void TransferTests::topLevelNameClashesAreMadeUnique()
{
  const auto root = testRoot(m_dir, "clash");
  writePatternFile(root / "a" / "report.txt", 5, 1);
  writePatternFile(root / "b" / "report.txt", 6, 2);
  writePatternFile(root / "c" / "report.txt", 7, 3);

  const auto source = buildSource({root / "a" / "report.txt", root / "b" / "report.txt", root / "c" / "report.txt"});
  QCOMPARE(source.manifest.entries.size(), size_t{3});
  QCOMPARE(source.manifest.entries[0].name, std::string("report.txt"));
  QCOMPARE(source.manifest.entries[1].name, std::string("report (2).txt"));
  QCOMPARE(source.manifest.entries[2].name, std::string("report (3).txt"));
}

void TransferTests::buildSourceListsNestedTree()
{
  const auto root = testRoot(m_dir, "tree");
  writePatternFile(root / "photos" / "a.jpg", 10, 1);
  writePatternFile(root / "photos" / "2026" / "b.jpg", 20, 2);
  writePatternFile(root / "photos" / "2026" / "deep" / "c.jpg", 30, 3);
  fs::create_directories(root / "photos" / "empty");

  // Distinct times per entry, so a time taken from the wrong file shows up.
  int64_t stamp = 1'700'000'000;
  std::map<std::string, int64_t> expectedTimes;
  for (const auto *name :
       {"photos/2026/deep/c.jpg", "photos/2026/deep", "photos/2026/b.jpg", "photos/2026", "photos/a.jpg",
        "photos/empty", "photos"}) {
    stamp += 3600;
    fs::last_write_time(root / name, fromUnixSeconds(stamp));
    expectedTimes[name] = stamp;
  }

  // A trailing separator must not change the names.
  const auto source = buildSource({root / "photos" / ""});
  std::map<std::string, Entry> byName;
  for (const auto &entry : source.manifest.entries) {
    byName[entry.name] = entry;
  }

  QCOMPARE(byName.size(), expectedTimes.size());
  for (const auto &[name, time] : expectedTimes) {
    QVERIFY2(byName.contains(name), name.c_str());
    QCOMPARE(byName[name].modified, time);
  }
  QVERIFY(byName["photos"].directory);
  QVERIFY(byName["photos/empty"].directory);
  QCOMPARE(byName["photos/2026/deep/c.jpg"].size, uint64_t{30});
  QCOMPARE(source.manifest.totalBytes(), uint64_t{60});
  QCOMPARE(source.paths.size(), source.manifest.entries.size());
  for (size_t i = 0; i < source.paths.size(); ++i) {
    QVERIFY(fs::equivalent(source.paths[i], root / fs::path(source.manifest.entries[i].name)));
  }
}

void TransferTests::unitsCoverEveryByteOnce()
{
  for (const unsigned streams : {1U, 2U, 3U, 4U, 7U}) {
    Manifest manifest;
    manifest.entries.push_back({"empty", 0, 0, false});
    manifest.entries.push_back({"dir", 0, 0, true});
    manifest.entries.push_back({"one", 1, 0, false});
    manifest.entries.push_back({"exact", kUnitBytes, 0, false});
    manifest.entries.push_back({"over", kUnitBytes + 1, 0, false});
    manifest.entries.push_back({"big", kUnitBytes * 5 + 123, 0, false});

    const auto units = layoutUnits(manifest);
    QCOMPARE(unitBytes(units), manifest.totalBytes());

    std::vector<uint64_t> covered(manifest.entries.size(), 0);
    size_t unitCount = 0;
    for (unsigned k = 0; k < streams; ++k) {
      for (const auto &unit : streamUnits(units, k, streams)) {
        QVERIFY(unit.length > 0 && unit.length <= kUnitBytes);
        QVERIFY(unit.offset % kUnitBytes == 0);
        covered[unit.entry] += unit.length;
        ++unitCount;
      }
    }
    QCOMPARE(unitCount, units.size());
    for (size_t i = 0; i < covered.size(); ++i) {
      QCOMPARE(covered[i], manifest.entries[i].size);
    }
  }
}

void TransferTests::streamCountPolicy()
{
  QCOMPARE(streamCountFor(0), 1U);
  QCOMPARE(streamCountFor(1 << 20), 1U);
  QCOMPARE(streamCountFor(64ull << 20), 2U);
  QCOMPARE(streamCountFor(10ull << 30), kDefaultStreams);

  Manifest few;
  few.entries.push_back({"one-unit.bin", kUnitBytes, 0, false});
  QCOMPARE(plannedStreams(few), 1U);

  Manifest many;
  many.entries.push_back({"large.bin", 1ull << 30, 0, false});
  QCOMPARE(plannedStreams(many), kDefaultStreams);

  QCOMPARE(plannedStreams(Manifest{}), 1U);
}

namespace {

void buildSampleTree(const fs::path &root)
{
  writePatternFile(root / "media" / "movie.bin", kUnitBytes * 2 + 12345, 11);
  writePatternFile(root / "media" / "clip.bin", kUnitBytes + 7, 12);
  writePatternFile(root / "media" / "small" / "a.txt", 100, 13);
  writePatternFile(root / "media" / "small" / "b.txt", 1, 14);
  writePatternFile(root / "media" / "zero.dat", 0, 15);
  fs::create_directories(root / "media" / "nothing");
  writePatternFile(root / "notes.txt", 2048, 16);
}

void verifyTree(const fs::path &sourceRoot, const fs::path &stagingDir)
{
  for (const auto &entry : fs::recursive_directory_iterator(sourceRoot)) {
    const auto relative = fs::relative(entry.path(), sourceRoot);
    const auto copy = stagingDir / relative;
    QVERIFY2(fs::exists(copy), qPrintable(QString::fromStdString(copy.string())));
    if (entry.is_directory()) {
      QVERIFY(fs::is_directory(copy));
    } else {
      QVERIFY2(filesEqual(entry.path(), copy), qPrintable(QString::fromStdString(relative.string())));
      QCOMPARE(fs::file_size(copy), fs::file_size(entry.path()));
    }
  }
}

} // namespace

void TransferTests::transferWithOneStream()
{
  const auto root = testRoot(m_dir, "oneStream");
  buildSampleTree(root / "src");
  const auto source = buildSource({root / "src" / "media", root / "src" / "notes.txt"});

  ReceiveSession session(source.manifest, root / "staging");
  const auto outcome = runTransfer(source, session, 1);

  QVERIFY2(outcome.errors.empty(), outcome.firstError());
  QVERIFY2(session.state() == ReceiveSession::State::Complete, session.error().c_str());
  QCOMPARE(session.received(), source.manifest.totalBytes());
  verifyTree(root / "src", root / "staging");

  const auto topLevel = session.topLevelPaths();
  QCOMPARE(topLevel.size(), size_t{2});
  QCOMPARE(topLevel[0].filename().string(), std::string("media"));
}

void TransferTests::transferWithManyStreams()
{
  const auto root = testRoot(m_dir, "manyStreams");
  buildSampleTree(root / "src");
  const auto source = buildSource({root / "src" / "media", root / "src" / "notes.txt"});

  for (const unsigned streams : {2U, 3U, 4U}) {
    const auto staging = root / ("staging" + std::to_string(streams));
    ReceiveSession session(source.manifest, staging);
    const auto outcome = runTransfer(source, session, streams);

    QVERIFY2(outcome.errors.empty(), outcome.firstError());
    QVERIFY2(session.state() == ReceiveSession::State::Complete, session.error().c_str());
    verifyTree(root / "src", staging);

    for (const auto &entry : source.manifest.entries) {
      if (entry.modified > 0) {
        const auto restored = toUnixSeconds(fs::last_write_time(pathForEntry(staging, entry.name)));
        QVERIFY2(restored == entry.modified, entry.name.c_str());
      }
    }
  }
}

void TransferTests::transferOfEmptySelection()
{
  const auto root = testRoot(m_dir, "emptySelection");
  fs::create_directories(root / "src" / "only" / "folders");
  writePatternFile(root / "src" / "only" / "zero.txt", 0, 1);

  const auto source = buildSource({root / "src" / "only"});
  QCOMPARE(source.manifest.totalBytes(), uint64_t{0});

  ReceiveSession session(source.manifest, root / "staging");
  const auto outcome = runTransfer(source, session, 1);
  QVERIFY2(outcome.errors.empty(), outcome.firstError());
  QVERIFY2(session.state() == ReceiveSession::State::Complete, session.error().c_str());
  QVERIFY(fs::is_directory(root / "staging" / "only" / "folders"));
  QVERIFY(fs::exists(root / "staging" / "only" / "zero.txt"));
}

void TransferTests::senderFailsWhenFileShrinks()
{
  const auto root = testRoot(m_dir, "shrinks");
  writePatternFile(root / "src" / "growing.bin", kUnitBytes * 2, 5);
  const auto source = buildSource({root / "src" / "growing.bin"});

  fs::resize_file(root / "src" / "growing.bin", 1000);

  ReceiveSession session(source.manifest, root / "staging");
  const auto outcome = runTransfer(source, session, 1);

  QVERIFY(!outcome.errors.empty());
  QVERIFY(session.state() == ReceiveSession::State::Failed);
  QVERIFY(!fs::exists(root / "staging"));
}

void TransferTests::receiverRejectsDuplicateStream()
{
  const auto root = testRoot(m_dir, "duplicate");
  writePatternFile(root / "src" / "f.bin", 100, 1);
  const auto source = buildSource({root / "src" / "f.bin"});

  ReceiveSession session(source.manifest, root / "staging");
  auto first = makePipePair();
  std::thread sender([&] { sendStream(*first.first, source, 0, 1, false); });
  const auto header = readStreamHeader(*first.second);
  session.receive(*first.second, header.index, header.count);
  sender.join();
  QVERIFY(session.state() == ReceiveSession::State::Complete);

  auto second = makePipePair();
  QVERIFY_EXCEPTION_THROWN(session.receive(*second.second, 0, 1), TransferError);

  ReceiveSession mismatch(source.manifest, root / "staging2");
  auto third = makePipePair();
  std::thread secondSender([&] { sendStream(*third.first, source, 0, 2, false); });
  const auto secondHeader = readStreamHeader(*third.second);
  mismatch.receive(*third.second, secondHeader.index, secondHeader.count);
  secondSender.join();

  auto fourth = makePipePair();
  QVERIFY_EXCEPTION_THROWN(mismatch.receive(*fourth.second, 1, 3), TransferError);
}

void TransferTests::headerRejectsGarbage()
{
  {
    auto pair = makePipePair();
    pair.first->write("not a transfer stream at all......", 34);
    pair.first.reset();
    QVERIFY_EXCEPTION_THROWN(readStreamHeader(*pair.second), TransferError);
  }
  {
    auto pair = makePipePair();
    Token token{};
    writeStreamHeader(*pair.first, token, 5, 3, nullptr);
    pair.first.reset();
    QVERIFY_EXCEPTION_THROWN(readStreamHeader(*pair.second), TransferError);
  }
  {
    auto pair = makePipePair();
    pair.first->write("AFS2", 4);
    pair.first.reset();
    QVERIFY_EXCEPTION_THROWN(readStreamHeader(*pair.second), BulkChannelError);
  }
}

void TransferTests::headerCarriesManifest()
{
  Manifest manifest;
  manifest.token[3] = 9;
  manifest.entries.push_back({"x.bin", 10, 0, false});
  const auto payload = encodeManifest(manifest);

  auto pair = makePipePair();
  writeStreamHeader(*pair.first, manifest.token, 0, 1, &payload);
  const auto header = readStreamHeader(*pair.second);
  QVERIFY(header.manifest.has_value());
  QCOMPARE(header.manifest->entries.size(), size_t{1});
  QVERIFY(header.token == manifest.token);

  Token other = manifest.token;
  other[0] ^= 0xff;
  auto mismatched = makePipePair();
  writeStreamHeader(*mismatched.first, other, 0, 1, &payload);
  QVERIFY_EXCEPTION_THROWN(readStreamHeader(*mismatched.second), TransferError);
}

void TransferTests::benchmarkTlsTransfer()
{
#if defined(_WIN32)
  QSKIP("socket pair helpers are POSIX only");
#else
  const int megabytes = qEnvironmentVariableIntValue("AETHER_TRANSFER_BENCH_MB");
  if (megabytes <= 0) {
    QSKIP("set AETHER_TRANSFER_BENCH_MB to run the throughput benchmark");
  }

  const QString certPath = m_dir.filePath("bench.pem");
  aether::generatePemSelfSignedCert(certPath, 2048);

  const auto root = testRoot(m_dir, "bench");
  const uint64_t bytes = static_cast<uint64_t>(megabytes) * 1024 * 1024;
  writePatternFile(root / "src" / "big.bin", bytes, 21);
  const auto source = buildSource({root / "src" / "big.bin"});

  for (const unsigned streams : {1U, 2U, 4U, 8U}) {
    const auto staging = root / ("staging" + std::to_string(streams));
    ReceiveSession session(source.manifest, staging);

    const auto start = std::chrono::steady_clock::now();
    const auto outcome = runTransfer(source, session, streams, [&] { return aether::testing::makeTlsPair(certPath); });
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    QVERIFY2(outcome.errors.empty(), outcome.firstError());
    QVERIFY2(session.state() == ReceiveSession::State::Complete, session.error().c_str());
    QCOMPARE(fs::file_size(staging / "big.bin"), bytes);
    qInfo().nospace() << streams << " stream(s): " << megabytes << " MiB in " << seconds
                      << " s = " << (megabytes / seconds) << " MiB/s";
    fs::remove_all(staging);
  }
#endif
}

QTEST_MAIN(TransferTests)
