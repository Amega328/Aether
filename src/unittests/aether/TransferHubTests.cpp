/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "TransferHubTests.h"

#include "TransferTestSupport.h"
#include "aether/TransferHub.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;
using namespace aether::testing;
using namespace aether::transfer;

namespace {

fs::path testRoot(QTemporaryDir &dir, const char *name)
{
  return fs::path(dir.path().toStdWString()) / name;
}

template <typename Predicate> bool waitUntil(Predicate predicate, std::chrono::seconds limit = std::chrono::seconds(20))
{
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

bool waitForState(const TransferHub &hub, const Token &token, ReceiveSession::State state)
{
  return waitUntil([&] {
    const auto progress = hub.status(token);
    return progress && progress->state == state;
  });
}

//! Connects a client hub to a server hub through in-memory pipes
void wireHubs(TransferHub &client, TransferHub &server, std::atomic<int> *dials = nullptr)
{
  client.setDialer([&client, &server, dials](const StreamRequest &request) {
    if (dials != nullptr) {
      ++*dials;
    }
    auto pair = makePipePair();
    client.serve(std::move(pair.first), request, Side::Client);
    server.serve(std::move(pair.second), request, Side::Server);
  });
}

void buildTree(const fs::path &root)
{
  writePatternFile(root / "media" / "movie.bin", kUnitBytes * 3 + 999, 31);
  writePatternFile(root / "media" / "small" / "a.txt", 64, 32);
  writePatternFile(root / "media" / "small" / "b.txt", 65, 33);
  fs::create_directories(root / "media" / "empty");
  writePatternFile(root / "readme.txt", 1000, 34);
}

void verifyTree(const fs::path &sourceRoot, const fs::path &stagedRoot)
{
  for (const auto &entry : fs::recursive_directory_iterator(sourceRoot)) {
    const auto copy = stagedRoot / fs::relative(entry.path(), sourceRoot);
    QVERIFY2(fs::exists(copy), copy.string().c_str());
    if (!entry.is_directory()) {
      QVERIFY2(filesEqual(entry.path(), copy), copy.string().c_str());
    }
  }
}

} // namespace

void TransferHubTests::initTestCase()
{
  m_log.setFilter(LogLevel::Level::Verbose);
}

void TransferHubTests::helloNameRoundTrip()
{
  StreamRequest request;
  request.role = Role::Push;
  request.token[0] = 0xab;
  request.token[15] = 0x01;
  request.index = 2;
  request.count = 4;

  const auto name = makeHelloName(request);
  QVERIFY(name.rfind("aetherxfer:push:", 0) == 0);

  const auto parsed = parseHelloName(name);
  QVERIFY(parsed.has_value());
  QVERIFY(parsed->role == Role::Push);
  QVERIFY(parsed->token == request.token);
  QCOMPARE(parsed->index, 2U);
  QCOMPARE(parsed->count, 4U);
}

void TransferHubTests::helloNameRejectsMalformed()
{
  const std::string good = "aetherxfer:pull:00112233445566778899aabbccddeeff:0:4";
  QVERIFY(parseHelloName(good).has_value());

  for (const std::string &bad : {
           std::string("aetherfwd:3000"),
           std::string("aetherxfer:"),
           std::string("aetherxfer:sideways:00112233445566778899aabbccddeeff:0:4"),
           std::string("aetherxfer:pull:00112233445566778899aabbccddeef:0:4"),
           std::string("aetherxfer:pull:00112233445566778899aabbccddeeff:4:4"),
           std::string("aetherxfer:pull:00112233445566778899aabbccddeeff:0:0"),
           std::string("aetherxfer:pull:00112233445566778899aabbccddeeff:0:99"),
           std::string("aetherxfer:pull:00112233445566778899aabbccddeeff:x:4"),
           std::string("aetherxfer:pull:00112233445566778899aabbccddeeff:0:4:extra"),
           std::string("aetherxfer:pull:00112233445566778899AABBCCDDEEFF:0:4"),
       }) {
    QVERIFY2(!parseHelloName(bad).has_value(), bad.c_str());
  }
}

void TransferHubTests::pullsFilesTheServerOffered()
{
  const auto root = testRoot(m_dir, "pull");
  buildTree(root / "src");

  TransferHub server(root / "server-staging");
  TransferHub client(root / "client-staging");
  wireHubs(client, server);

  std::atomic<int> finished = 0;
  client.setListener([&](const Token &) { ++finished; });

  const auto payload = server.offer(buildSource({root / "src" / "media", root / "src" / "readme.txt"}));
  const auto manifest = decodeManifest(payload);

  const auto first = client.request(manifest);
  QVERIFY(first.state != ReceiveSession::State::Failed);
  QVERIFY(waitForState(client, manifest.token, ReceiveSession::State::Complete));
  QVERIFY(waitUntil([&] { return finished == 1; }));

  const auto done = *client.status(manifest.token);
  QCOMPARE(done.paths.size(), size_t{2});
  QCOMPARE(done.received, manifest.totalBytes());
  verifyTree(root / "src", done.directory);

  client.setListener({});
  client.shutdown();
  server.shutdown();
}

void TransferHubTests::pushesFilesTheClientOffered()
{
  const auto root = testRoot(m_dir, "push");
  buildTree(root / "src");

  TransferHub client(root / "client-staging");
  TransferHub server(root / "server-staging");
  wireHubs(client, server);

  const auto payload = client.offer(buildSource({root / "src" / "media", root / "src" / "readme.txt"}));
  const auto manifest = decodeManifest(payload);

  server.request(manifest);
  client.announceSent(manifest);

  QVERIFY(waitForState(server, manifest.token, ReceiveSession::State::Complete));
  verifyTree(root / "src", server.status(manifest.token)->directory);

  client.shutdown();
  server.shutdown();
}

void TransferHubTests::pushCreatesTheSessionFromTheHeader()
{
  const auto root = testRoot(m_dir, "pushHeader");
  buildTree(root / "src");

  TransferHub client(root / "client-staging");
  TransferHub server(root / "server-staging");
  wireHubs(client, server);

  const auto manifest =
      decodeManifest(client.offer(buildSource({root / "src" / "media", root / "src" / "readme.txt"})));

  client.announceSent(manifest);

  QVERIFY(waitForState(server, manifest.token, ReceiveSession::State::Complete));
  verifyTree(root / "src", server.status(manifest.token)->directory);

  client.shutdown();
  server.shutdown();
}

void TransferHubTests::relaysReceivedFilesToAnotherClient()
{
  const auto root = testRoot(m_dir, "relay");
  buildTree(root / "src");

  TransferHub clientA(root / "a-staging");
  TransferHub server(root / "server-staging");
  TransferHub clientB(root / "b-staging");
  wireHubs(clientA, server);
  wireHubs(clientB, server);

  const auto manifest = decodeManifest(clientA.offer(buildSource({root / "src" / "media"})));
  clientA.announceSent(manifest);
  QVERIFY(waitForState(server, manifest.token, ReceiveSession::State::Complete));

  clientB.request(manifest);
  QVERIFY(waitForState(clientB, manifest.token, ReceiveSession::State::Complete));
  verifyTree(root / "src" / "media", clientB.status(manifest.token)->directory / "media");

  clientA.shutdown();
  clientB.shutdown();
  server.shutdown();
}

void TransferHubTests::ownedManifestNeedsNoTransfer()
{
  const auto root = testRoot(m_dir, "owned");
  buildTree(root / "src");

  TransferHub client(root / "client-staging");
  TransferHub server(root / "server-staging");
  std::atomic<int> dials = 0;
  wireHubs(client, server, &dials);

  const auto manifest = decodeManifest(client.offer(buildSource({root / "src" / "media"})));
  const auto progress = client.request(manifest);

  QVERIFY(progress.state == ReceiveSession::State::Complete);
  QCOMPARE(dials.load(), 0);
  QCOMPARE(progress.paths.size(), size_t{1});
  QCOMPARE(progress.paths.front(), root / "src" / "media");
  QVERIFY(!fs::exists(root / "client-staging"));
}

void TransferHubTests::recognizesStagedFiles()
{
  const auto root = testRoot(m_dir, "recognize");
  buildTree(root / "src");

  TransferHub server(root / "server-staging");
  TransferHub client(root / "client-staging");
  wireHubs(client, server);

  const auto payload = server.offer(buildSource({root / "src" / "media", root / "src" / "readme.txt"}));
  const auto manifest = decodeManifest(payload);
  client.request(manifest);
  QVERIFY(waitForState(client, manifest.token, ReceiveSession::State::Complete));

  const auto staged = client.status(manifest.token)->paths;
  const auto again = client.recognizeStaged(staged);
  QVERIFY(again.has_value());
  QCOMPARE(*again, payload);

  QVERIFY(!client.recognizeStaged({staged.front()}).has_value());
  QVERIFY(!client.recognizeStaged({root / "src" / "readme.txt"}).has_value());

  client.shutdown();
  server.shutdown();
}

void TransferHubTests::retriesAfterFailure()
{
  const auto root = testRoot(m_dir, "retry");
  buildTree(root / "src");

  TransferHub server(root / "server-staging");
  TransferHub client(root / "client-staging");
  wireHubs(client, server);

  const auto source = buildSource({root / "src" / "media"});
  const auto manifest = decodeManifest(encodeManifest(source.manifest));

  client.request(manifest);
  QVERIFY(waitForState(client, manifest.token, ReceiveSession::State::Failed));
  QVERIFY(!client.status(manifest.token)->error.empty());

  server.offer(source);
  client.request(manifest);
  QVERIFY(waitForState(client, manifest.token, ReceiveSession::State::Complete));
  verifyTree(root / "src" / "media", client.status(manifest.token)->directory / "media");

  client.shutdown();
  server.shutdown();
}

void TransferHubTests::evictsOldTransfers()
{
  const auto root = testRoot(m_dir, "evict");
  TransferHub server(root / "server-staging");
  TransferHub client(root / "client-staging");
  wireHubs(client, server);

  std::vector<Manifest> manifests;
  std::vector<fs::path> directories;
  for (int i = 0; i < 3; ++i) {
    const auto file = root / "src" / ("file" + std::to_string(i) + ".bin");
    writePatternFile(file, 5000 + static_cast<uint64_t>(i), static_cast<uint32_t>(40 + i));
    manifests.push_back(decodeManifest(server.offer(buildSource({file}))));

    client.request(manifests.back());
    QVERIFY(waitForState(client, manifests.back().token, ReceiveSession::State::Complete));
    directories.push_back(client.status(manifests.back().token)->directory);
  }

  QVERIFY(!client.status(manifests[0].token).has_value());
  QVERIFY(client.status(manifests[1].token).has_value());
  QVERIFY(client.status(manifests[2].token).has_value());
  QVERIFY(waitUntil([&] { return !fs::exists(directories[0]); }));
  QVERIFY(fs::exists(directories[1]));
  QVERIFY(fs::exists(directories[2]));

  client.shutdown();
  server.shutdown();
}

void TransferHubTests::shutdownCancelsRunningTransfers()
{
  const auto root = testRoot(m_dir, "shutdown");
  writePatternFile(root / "src" / "big.bin", kUnitBytes * 6, 51);

  TransferHub server(root / "server-staging");
  TransferHub client(root / "client-staging");

  // The server end of every connection is never served, so the client stays blocked.
  std::vector<std::unique_ptr<BulkChannel>> parked;
  std::mutex parkedMutex;
  client.setDialer([&](const StreamRequest &request) {
    auto pair = makePipePair();
    client.serve(std::move(pair.first), request, Side::Client);
    std::scoped_lock lock{parkedMutex};
    parked.push_back(std::move(pair.second));
  });

  const auto manifest = decodeManifest(server.offer(buildSource({root / "src" / "big.bin"})));
  client.request(manifest);
  QVERIFY(!waitUntil(
      [&] { return client.status(manifest.token)->state != ReceiveSession::State::Receiving; }, std::chrono::seconds(1)
  ));

  const auto start = std::chrono::steady_clock::now();
  client.shutdown();
  QVERIFY2(std::chrono::steady_clock::now() - start < std::chrono::seconds(5), "shutdown did not abort the transfer");
  QVERIFY(client.status(manifest.token)->state == ReceiveSession::State::Failed);
  server.shutdown();
}

QTEST_MAIN(TransferHubTests)
