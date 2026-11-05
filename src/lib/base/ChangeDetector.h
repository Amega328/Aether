/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <utility>

//! Remembers a value and reports when a new one differs
/*!
For state that is polled often, such as on every mouse move, so it can be
logged when it changes instead of on every poll.
*/
template <typename T> class ChangeDetector
{
public:
  explicit ChangeDetector(T initial = T{}) : m_value(std::move(initial))
  {
  }

  //! Store \c value; true when it differs from the stored one.
  bool update(const T &value)
  {
    if (value == m_value) {
      return false;
    }
    m_value = value;
    return true;
  }

  const T &value() const
  {
    return m_value;
  }

private:
  T m_value;
};
