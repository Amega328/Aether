/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "EiClipboardTests.h"

#include "platform/EiClipboard.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using aether::EiClipboard;
using Format = IClipboard::Format;

void EiClipboardTests::initTestCase()
{
  m_log.setFilter(LogLevel::Level::Verbose);
}

void EiClipboardTests::sameThreadMayReopen()
{
  EiClipboard clip(kClipboardClipboard);
  QVERIFY(clip.open(0));
  QVERIFY(clip.open(0));
  QVERIFY(clip.empty());
  clip.add(Format::Text, "hello");
  clip.close();

  QVERIFY(clip.open(0));
  QVERIFY(clip.has(Format::Text));
  QCOMPARE(clip.get(Format::Text), std::string("hello"));
  clip.close();
}

void EiClipboardTests::secondThreadWaitsForClose()
{
  EiClipboard clip(kClipboardClipboard);
  QVERIFY(clip.open(0));
  QVERIFY(clip.empty());

  std::atomic<bool> opened = false;
  auto other = std::async(std::launch::async, [&] {
    clip.open(1);
    opened = true;
    const bool hasText = clip.has(Format::Text);
    clip.close();
    return hasText;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  QVERIFY2(!opened, "second thread must wait while the first holds the clipboard open");

  clip.add(Format::Text, "hello");
  clip.close();

  QVERIFY2(other.get(), "second thread must see everything the first session stored");
}

void EiClipboardTests::readersNeverSeeAPartialCopy()
{
  // Mirrors the portal glib thread (serving pastes) racing the event thread
  // (storing data from the server via IClipboard::copy): neither may observe
  // or break the other's empty()/add() sequence.
  EiClipboard clip(kClipboardClipboard);
  EiClipboard incoming(kClipboardClipboard);
  const std::string payload(1024 * 1024, 'x');

  QVERIFY(incoming.open(0));
  QVERIFY(incoming.empty());
  incoming.add(Format::Files, payload);
  incoming.close();
  QVERIFY(IClipboard::copy(&clip, &incoming));

  std::atomic<bool> done = false;
  std::thread writer([&] {
    for (int i = 0; i < 200; ++i)
      IClipboard::copy(&clip, &incoming);
    done = true;
  });

  int bad = 0;
  while (!done) {
    clip.open(0);
    if (!clip.has(Format::Files) || clip.get(Format::Files) != payload)
      ++bad;
    clip.close();
  }
  writer.join();

  QCOMPARE(bad, 0);

  QVERIFY(clip.open(0));
  QVERIFY(clip.has(Format::Files));
  clip.close();
}

QTEST_MAIN(EiClipboardTests)
