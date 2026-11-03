/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Red Hat, Inc.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/EiClipboard.h"
#include "base/Log.h"

namespace aether {

EiClipboard::EiClipboard(ClipboardID id) : m_id(id)
{
  open(0);
  empty();
  close();
}

bool EiClipboard::empty()
{
  std::scoped_lock lock{m_mutex};
  if (!m_open) {
    LOG_WARN("cannot empty clipboard, not open");
    return false;
  }

  // Clear all data
  for (int32_t index = 0; index < static_cast<int>(Format::TotalFormats); ++index) {
    m_data[index] = "";
    m_added[index] = false;
  }

  // Save time
  m_timeOwned = m_time;

  // We're the owner now
  m_owner = true;

  return true;
}

void EiClipboard::add(Format format, const std::string &data)
{
  std::scoped_lock lock{m_mutex};
  if (!m_open) {
    LOG_WARN("cannot add to clipboard, not open");
    return;
  }

  if (!m_owner) {
    LOG_WARN("cannot add to clipboard, no owner");
    return;
  }

  const auto formatID = static_cast<int>(format);
  m_data[formatID] = data;
  m_added[formatID] = true;
}

bool EiClipboard::open(Time time) const
{
  std::unique_lock lock{m_mutex};
  const auto self = std::this_thread::get_id();
  if (m_open && m_openThread == self) {
    LOG_DEBUG("skipping clipboard open, already open");
    return true;
  }

  if (!m_closed.wait_for(lock, kOpenTimeout, [this] { return !m_open; }))
    LOG_WARN(
        "clipboard still open on another thread after %lld s, taking over", static_cast<long long>(kOpenTimeout.count())
    );

  m_open = true;
  m_openThread = self;
  m_time = time;

  return true;
}

void EiClipboard::close() const
{
  std::scoped_lock lock{m_mutex};
  if (!m_open) {
    LOG_WARN("clipboard is not open");
  }
  m_open = false;
  m_openThread = {};
  m_closed.notify_one();
}

EiClipboard::Time EiClipboard::getTime() const
{
  return m_timeOwned;
}

bool EiClipboard::has(Format format) const
{
  std::scoped_lock lock{m_mutex};
  if (!m_open) {
    LOG_WARN("cannot check for clipboard format, not open");
    return false;
  }
  return m_added[static_cast<int>(format)];
}

std::string EiClipboard::get(Format format) const
{
  std::scoped_lock lock{m_mutex};
  if (!m_open) {
    LOG_WARN("cannot get clipboard format, not open");
    return "";
  }
  return m_data[static_cast<int>(format)];
}

} // namespace aether
