/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "aether/IClipboard.h"

#include <mutex>
#include <optional>
#include <string>

//! Memory buffer clipboard
/*!
This class implements a clipboard that stores data in memory.
*/
class Clipboard : public IClipboard
{
public:
  Clipboard();
  ~Clipboard() override = default;

  //! @name manipulators
  //@{

  //! Unmarshall clipboard data
  /*!
  Extract marshalled clipboard data and store it in this clipboard.
  Sets the clipboard time to \c time.
  */
  void unmarshall(const std::string &data, Time time);

  //@}
  //! @name accessors
  //@{

  //! Marshall clipboard data
  /*!
  Merge this clipboard's data into a single buffer that can be later
  unmarshalled to restore the clipboard and return the buffer.
  */
  std::string marshall() const;

  //@}

  // IClipboard overrides
  bool empty() final;
  void add(Format, const std::string &data) override;
  bool open(Time) const final;
  void close() const override;
  Time getTime() const override;
  bool has(Format) const override;
  std::string get(Format) const override;

private:
  mutable bool m_open = false;
  mutable std::mutex m_mutex;
  mutable Time m_time;
  bool m_owner = false;
  Time m_timeOwned;
  bool m_added[static_cast<int>(Format::TotalFormats)] = {false, false, false};
  std::string m_data[static_cast<int>(Format::TotalFormats)] = {"", "", ""};
};

namespace aether::clipboard {

//! The Files payload of \c clipboard, if it holds one.
/*!
Clipboards must be open to be read, so this opens \c clipboard for the duration of the call.
Call it on a closed clipboard.
*/
std::optional<std::string> readFilesPayload(const IClipboard &clipboard);

//! Copy the formats of \c src into \c dst, keeping \c src's time.
/*!
An empty Files payload (a selection still being listed) is left out. With
\c textOnly only Text and HTML are copied: what a primary selection, which
Windows does not have, should carry instead of a second copy of big images
and file lists. Both clipboards must be closed.
*/
void copyFormats(const IClipboard &src, IClipboard &dst, bool textOnly = false);

} // namespace aether::clipboard
