/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace aether::transfer {

//! File opened for positioned reads or writes
/*!
Reads and writes carry an explicit offset and never touch a shared file
position, so several threads may use separate handles of one file at once.
All failures throw TransferError.
*/
class RandomAccessFile
{
public:
  enum class Mode
  {
    ReadSequential, //!< existing file, hinted for sequential scans
    Write           //!< create if missing, keep existing content
  };

  RandomAccessFile(const std::filesystem::path &path, Mode mode);
  RandomAccessFile(const RandomAccessFile &) = delete;
  RandomAccessFile &operator=(const RandomAccessFile &) = delete;
  ~RandomAccessFile();

  //! Read up to \c n bytes at \c offset; fewer only at end of file.
  size_t readAt(uint64_t offset, void *buffer, size_t n);

  //! Write all \c n bytes at \c offset.
  void writeAt(uint64_t offset, const void *buffer, size_t n);

  void resize(uint64_t size);

private:
  std::filesystem::path m_path;
#if defined(_WIN32)
  void *m_handle;
#else
  int m_fd;
#endif
};

} // namespace aether::transfer
