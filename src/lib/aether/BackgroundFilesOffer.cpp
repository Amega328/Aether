/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "aether/BackgroundFilesOffer.h"

#include "base/Log.h"

namespace aether::clipboard {

namespace {

template <typename Jobs> void joinAll(Jobs &jobs)
{
  for (auto &job : jobs) {
    if (job->thread.joinable()) {
      job->thread.join();
    }
  }
  jobs.clear();
}

} // namespace

BackgroundFilesOffer::BackgroundFilesOffer(Builder builder, std::chrono::milliseconds wait)
    : m_builder(std::move(builder)),
      m_wait(wait)
{
}

BackgroundFilesOffer::~BackgroundFilesOffer()
{
  shutdown();
}

BackgroundFilesOffer &BackgroundFilesOffer::shared()
{
  static BackgroundFilesOffer instance;
  return instance;
}

BackgroundFilesOffer::Builder BackgroundFilesOffer::defaultBuilder()
{
  return [](const std::vector<std::filesystem::path> &paths) { return offerFiles(paths); };
}

void BackgroundFilesOffer::setReadyCallback(ReadyCallback callback)
{
  std::scoped_lock lock{m_mutex};
  m_ready = std::move(callback);
}

std::optional<std::string>
BackgroundFilesOffer::offer(const std::vector<std::filesystem::path> &paths, uint64_t generation)
{
  std::list<std::unique_ptr<Job>> retired;
  std::optional<std::string> result;
  std::string error;
  {
    std::unique_lock lock{m_mutex};
    for (auto it = std::next(m_jobs.begin(), m_jobs.empty() ? 0 : 1); it != m_jobs.end();) {
      if ((*it)->done) {
        retired.splice(retired.end(), m_jobs, it++);
      } else {
        ++it;
      }
    }

    Job *job = m_jobs.empty() ? nullptr : m_jobs.front().get();
    if (job == nullptr || job->generation != generation || job->paths != paths) {
      auto fresh = std::make_unique<Job>();
      fresh->paths = paths;
      fresh->generation = generation;
      job = fresh.get();
      m_jobs.push_front(std::move(fresh));
      job->thread = std::thread([this, job] { run(job); });
      m_finished.wait_for(lock, m_wait, [job] { return job->done; });
      if (!job->done) {
        LOG_DEBUG("still listing %zu copied item(s), the clipboard follows when done", paths.size());
      }
    }

    if (!job->done) {
      job->callerGaveUp = true;
    } else if (!job->error.empty()) {
      error = job->error;
    } else {
      result = job->payload;
    }
  }

  joinAll(retired);
  if (!error.empty()) {
    throw ClipboardFilesError(error);
  }
  return result;
}

void BackgroundFilesOffer::run(Job *job)
{
  std::string payload;
  std::string error;
  try {
    payload = m_builder(job->paths);
  } catch (const std::exception &e) {
    error = e.what();
    if (error.empty()) {
      error = "unknown error";
    }
  }

  ReadyCallback notify;
  {
    std::scoped_lock lock{m_mutex};
    job->payload = std::move(payload);
    job->error = std::move(error);
    job->done = true;
    if (job->callerGaveUp && !m_jobs.empty() && m_jobs.front().get() == job) {
      notify = m_ready;
    }
  }
  m_finished.notify_all();
  if (notify) {
    notify();
  }
}

void BackgroundFilesOffer::shutdown()
{
  std::list<std::unique_ptr<Job>> jobs;
  {
    std::scoped_lock lock{m_mutex};
    m_ready = {};
    jobs.swap(m_jobs);
  }
  joinAll(jobs);
}

} // namespace aether::clipboard
