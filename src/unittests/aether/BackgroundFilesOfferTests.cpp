/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "BackgroundFilesOfferTests.h"

#include "aether/BackgroundFilesOffer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using aether::clipboard::BackgroundFilesOffer;
using aether::clipboard::ClipboardFilesError;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

//! Builder that lists instantly or holds until opened, and counts its calls
class FakeBuilder
{
public:
  explicit FakeBuilder(bool startOpen) : m_open(startOpen)
  {
  }

  BackgroundFilesOffer::Builder builder()
  {
    return [this](const std::vector<fs::path> &paths) {
      ++m_calls;
      std::unique_lock lock{m_mutex};
      m_changed.wait(lock, [this] { return m_open; });
      if (m_fail) {
        throw ClipboardFilesError("unreadable path");
      }
      return "payload:" + paths.front().generic_string();
    };
  }

  void open()
  {
    {
      std::scoped_lock lock{m_mutex};
      m_open = true;
    }
    m_changed.notify_all();
  }

  void failNext()
  {
    std::scoped_lock lock{m_mutex};
    m_fail = true;
  }

  int calls() const
  {
    return m_calls;
  }

private:
  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_open;
  bool m_fail = false;
  std::atomic<int> m_calls = 0;
};

template <typename Predicate> bool waitUntil(Predicate predicate)
{
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

const std::vector<fs::path> kSelection{"/home/user/folder"};

} // namespace

void BackgroundFilesOfferTests::quickSelectionIsReturnedDirectly()
{
  FakeBuilder fake(true);
  BackgroundFilesOffer offer(fake.builder(), 2s);
  std::atomic<int> announced = 0;
  offer.setReadyCallback([&] { ++announced; });

  const auto payload = offer.offer(kSelection, 1);
  QVERIFY(payload.has_value());
  QCOMPARE(*payload, std::string("payload:/home/user/folder"));
  offer.shutdown();
  QCOMPARE(announced.load(), 0);
}

void BackgroundFilesOfferTests::slowSelectionIsDeferredAndAnnounced()
{
  FakeBuilder fake(false);
  BackgroundFilesOffer offer(fake.builder(), 300ms);
  std::atomic<int> announced = 0;
  offer.setReadyCallback([&] { ++announced; });

  const auto start = std::chrono::steady_clock::now();
  QVERIFY(!offer.offer(kSelection, 1).has_value());
  QVERIFY(std::chrono::steady_clock::now() - start < 2s);

  // A second read of the same clipboard (the selection, the next screen switch) must not wait again.
  const auto again = std::chrono::steady_clock::now();
  QVERIFY(!offer.offer(kSelection, 1).has_value());
  QVERIFY(std::chrono::steady_clock::now() - again < 150ms);

  fake.open();
  QVERIFY(waitUntil([&] { return announced == 1; }));
  const auto payload = offer.offer(kSelection, 1);
  QVERIFY(payload.has_value());
  QCOMPARE(*payload, std::string("payload:/home/user/folder"));
  QCOMPARE(fake.calls(), 1);
  QCOMPARE(announced.load(), 1);
}

void BackgroundFilesOfferTests::sameSelectionIsListedOnce()
{
  FakeBuilder fake(true);
  BackgroundFilesOffer offer(fake.builder(), 2s);
  for (int i = 0; i < 5; ++i) {
    QVERIFY(offer.offer(kSelection, 7).has_value());
  }
  QCOMPARE(fake.calls(), 1);
}

void BackgroundFilesOfferTests::newGenerationIsListedAgain()
{
  FakeBuilder fake(true);
  BackgroundFilesOffer offer(fake.builder(), 2s);
  QVERIFY(offer.offer(kSelection, 1).has_value());
  QVERIFY(offer.offer(kSelection, 2).has_value());
  QVERIFY(offer.offer({"/home/user/other"}, 2).has_value());
  QCOMPARE(fake.calls(), 3);
}

void BackgroundFilesOfferTests::listingErrorsAreReported()
{
  FakeBuilder fake(true);
  fake.failNext();
  BackgroundFilesOffer offer(fake.builder(), 2s);
  QVERIFY_THROWS_EXCEPTION(ClipboardFilesError, offer.offer(kSelection, 1));
}

void BackgroundFilesOfferTests::supersededListingIsNotAnnounced()
{
  FakeBuilder slow(false);
  BackgroundFilesOffer offer(slow.builder(), 20ms);
  std::atomic<int> announced = 0;
  offer.setReadyCallback([&] { ++announced; });

  QVERIFY(!offer.offer(kSelection, 1).has_value());
  // The user copied something else meanwhile; the old listing must not trigger a clipboard update.
  slow.open();
  QVERIFY(offer.offer({"/home/user/other"}, 2).has_value());
  QVERIFY(waitUntil([&] { return slow.calls() == 2; }));
  offer.shutdown();
  QCOMPARE(announced.load(), 0);
}

void BackgroundFilesOfferTests::shutdownSilencesPendingListing()
{
  FakeBuilder fake(false);
  BackgroundFilesOffer offer(fake.builder(), 10ms);
  std::atomic<int> announced = 0;
  offer.setReadyCallback([&] { ++announced; });
  QVERIFY(!offer.offer(kSelection, 1).has_value());

  std::thread opener([&] {
    std::this_thread::sleep_for(50ms);
    fake.open();
  });
  offer.shutdown();
  opener.join();
  QCOMPARE(announced.load(), 0);
}

QTEST_MAIN(BackgroundFilesOfferTests)
