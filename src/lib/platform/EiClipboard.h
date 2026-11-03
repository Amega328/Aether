/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Red Hat, Inc.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "aether/ClipboardTypes.h"
#include "aether/IClipboard.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace aether {

//! EI/Portal clipboard implementation
/*!
This class implements a clipboard for EI (libei) and portal-based input capture.
It stores clipboard data in memory for the specified clipboard ID.
*/
class EiClipboard : public IClipboard
{
public:
  explicit EiClipboard(ClipboardID id);
  ~EiClipboard() override = default;

  //! Get clipboard ID
  ClipboardID getID() const
  {
    return m_id;
  }

  //! @name IClipboard overrides
  //@{
  bool empty() override;
  void add(Format, const std::string &data) override;
  //! Open the clipboard, waiting if another thread has it open.
  /*!
  The portal thread (serving pastes) and the event thread (storing data from
  the server) share one cache, so an open()/close() session is exclusive
  across threads. The thread holding it may call open() again.
  */
  bool open(Time) const override;
  void close() const override;
  Time getTime() const override;
  bool has(Format) const override;
  std::string get(Format) const override;
  //@}

private:
  // Sessions are in-memory copies and last milliseconds; this only bounds the
  // damage if a close() is ever missed.
  static constexpr std::chrono::seconds kOpenTimeout{5};

  ClipboardID m_id;
  mutable bool m_open = false;
  mutable std::thread::id m_openThread;
  mutable Time m_time = 0;
  mutable std::mutex m_mutex;
  mutable std::condition_variable m_closed;
  bool m_owner = false;
  Time m_timeOwned = 0;
  bool m_added[static_cast<int>(Format::TotalFormats)] = {false, false, false};
  std::string m_data[static_cast<int>(Format::TotalFormats)] = {"", "", ""};
};

} // namespace aether
