/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "aether/ClipboardFiles.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace aether::clipboard {

//! Builds Files clipboard payloads on a worker thread
/*!
Listing a large selection (tens of thousands of files) takes seconds, and
clipboards are read on the event thread, which must keep serving input and
keep-alives. offer() waits briefly for the payload; when the selection takes
longer it returns nothing, lets the caller go on without the files, and calls
the ready callback once the payload exists, so the clipboard can be read again.

A result belongs to one selection at one clipboard generation (on Windows, the
clipboard sequence number), so a changed clipboard never reuses a stale listing.
*/
class BackgroundFilesOffer
{
public:
  using Builder = std::function<std::string(const std::vector<std::filesystem::path> &)>;
  using ReadyCallback = std::function<void()>;

  static constexpr auto kDefaultWait = std::chrono::milliseconds(200);

  explicit BackgroundFilesOffer(Builder builder = defaultBuilder(), std::chrono::milliseconds wait = kDefaultWait);
  BackgroundFilesOffer(const BackgroundFilesOffer &) = delete;
  BackgroundFilesOffer &operator=(const BackgroundFilesOffer &) = delete;
  ~BackgroundFilesOffer();

  //! Instance shared by the platform clipboard code
  static BackgroundFilesOffer &shared();

  //! Called on the worker thread when a payload that offer() could not wait for is ready.
  void setReadyCallback(ReadyCallback callback);

  //! The payload, or nothing while the selection is still being listed.
  /*!
  Throws ClipboardFilesError when listing failed. Only the first call for a
  selection waits; later calls return at once.
  */
  std::optional<std::string> offer(const std::vector<std::filesystem::path> &paths, uint64_t generation);

  //! Drop the callback and results and wait for running workers.
  void shutdown();

private:
  struct Job
  {
    std::vector<std::filesystem::path> paths;
    uint64_t generation = 0;
    std::thread thread;
    bool done = false;
    bool callerGaveUp = false;
    std::string payload;
    std::string error;
  };

  static Builder defaultBuilder();
  void run(Job *job);
  void reapLocked();

  const Builder m_builder;
  const std::chrono::milliseconds m_wait;

  std::mutex m_mutex;
  std::condition_variable m_finished;
  std::list<std::unique_ptr<Job>> m_jobs; //!< front is the current selection
  ReadyCallback m_ready;
};

} // namespace aether::clipboard
