/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ThreadCancelTests.h"

#include "arch/ArchException.h"
#include "base/FunctionJob.h"
#include "mt/Thread.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <thread>

namespace {

struct LoopState
{
  std::atomic<bool> started = false;
  std::atomic<bool> unwoundByCancel = false;
  std::atomic<bool> otherException = false;
};

// Like the socket multiplexer's service loop: poll for cancellation until stopped.
void pollUntilCancelled(void *arg)
{
  auto *state = static_cast<LoopState *>(arg);
  state->started = true;
  try {
    for (;;) {
      Thread::testCancel();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } catch (ThreadCancelException &) {
    state->unwoundByCancel = true;
    throw;
  } catch (const std::exception &) {
    // On Windows this used to be std::system_error ("resource deadlock would occur"),
    // which escaped the thread and aborted the process on shutdown.
    state->otherException = true;
  }
}

} // namespace

void ThreadCancelTests::initTestCase()
{
  m_arch.init();
}

void ThreadCancelTests::cancelledThreadUnwindsCleanly()
{
  LoopState state;
  Thread thread(new FunctionJob(&pollUntilCancelled, &state));

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!state.started && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  QVERIFY(state.started);

  thread.cancel();
  QVERIFY(thread.wait(5.0));
  QVERIFY(state.unwoundByCancel);
  QVERIFY(!state.otherException);
}

QTEST_MAIN(ThreadCancelTests)
