/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "aether/RandomAccessFile.h"

#include "aether/TransferWire.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace aether::transfer {

namespace {

constexpr size_t kMaxIoBytes = 1u << 30;

#if defined(_WIN32)
std::string describeError()
{
  return "windows error " + std::to_string(GetLastError());
}
#else
std::string describeError()
{
  return std::strerror(errno);
}
#endif

} // namespace

#if defined(_WIN32)

RandomAccessFile::RandomAccessFile(const std::filesystem::path &path, Mode mode)
    : m_path(path),
      m_handle(INVALID_HANDLE_VALUE)
{
  const bool write = mode == Mode::Write;
  m_handle = CreateFileW(
      path.c_str(), write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      write ? OPEN_ALWAYS : OPEN_EXISTING, write ? FILE_ATTRIBUTE_NORMAL : FILE_FLAG_SEQUENTIAL_SCAN, nullptr
  );
  if (m_handle == INVALID_HANDLE_VALUE) {
    throw TransferError("cannot open " + path.string() + ": " + describeError());
  }
}

RandomAccessFile::~RandomAccessFile()
{
  if (m_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(m_handle);
  }
}

size_t RandomAccessFile::readAt(uint64_t offset, void *buffer, size_t n)
{
  auto *out = static_cast<char *>(buffer);
  size_t total = 0;
  while (total < n) {
    OVERLAPPED position{};
    const uint64_t at = offset + total;
    position.Offset = static_cast<DWORD>(at & 0xffffffff);
    position.OffsetHigh = static_cast<DWORD>(at >> 32);

    DWORD got = 0;
    const auto want = static_cast<DWORD>(std::min(n - total, kMaxIoBytes));
    if (!ReadFile(m_handle, out + total, want, &got, &position)) {
      if (GetLastError() == ERROR_HANDLE_EOF) {
        break;
      }
      throw TransferError("cannot read " + m_path.string() + ": " + describeError());
    }
    if (got == 0) {
      break;
    }
    total += got;
  }
  return total;
}

void RandomAccessFile::writeAt(uint64_t offset, const void *buffer, size_t n)
{
  const auto *in = static_cast<const char *>(buffer);
  size_t total = 0;
  while (total < n) {
    OVERLAPPED position{};
    const uint64_t at = offset + total;
    position.Offset = static_cast<DWORD>(at & 0xffffffff);
    position.OffsetHigh = static_cast<DWORD>(at >> 32);

    DWORD wrote = 0;
    const auto want = static_cast<DWORD>(std::min(n - total, kMaxIoBytes));
    if (!WriteFile(m_handle, in + total, want, &wrote, &position) || wrote == 0) {
      throw TransferError("cannot write " + m_path.string() + ": " + describeError());
    }
    total += wrote;
  }
}

void RandomAccessFile::resize(uint64_t size)
{
  LARGE_INTEGER target;
  target.QuadPart = static_cast<LONGLONG>(size);
  if (!SetFilePointerEx(m_handle, target, nullptr, FILE_BEGIN) || !SetEndOfFile(m_handle)) {
    throw TransferError("cannot size " + m_path.string() + ": " + describeError());
  }
}

#else

RandomAccessFile::RandomAccessFile(const std::filesystem::path &path, Mode mode) : m_path(path), m_fd(-1)
{
  const int flags = mode == Mode::Write ? (O_WRONLY | O_CREAT | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC);
  m_fd = ::open(path.c_str(), flags, 0644);
  if (m_fd < 0) {
    throw TransferError("cannot open " + path.string() + ": " + describeError());
  }
#if defined(POSIX_FADV_SEQUENTIAL)
  if (mode == Mode::ReadSequential) {
    posix_fadvise(m_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
  }
#endif
}

RandomAccessFile::~RandomAccessFile()
{
  if (m_fd >= 0) {
    ::close(m_fd);
  }
}

size_t RandomAccessFile::readAt(uint64_t offset, void *buffer, size_t n)
{
  auto *out = static_cast<char *>(buffer);
  size_t total = 0;
  while (total < n) {
    const ssize_t got =
        ::pread(m_fd, out + total, std::min(n - total, kMaxIoBytes), static_cast<off_t>(offset + total));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw TransferError("cannot read " + m_path.string() + ": " + describeError());
    }
    if (got == 0) {
      break;
    }
    total += static_cast<size_t>(got);
  }
  return total;
}

void RandomAccessFile::writeAt(uint64_t offset, const void *buffer, size_t n)
{
  const auto *in = static_cast<const char *>(buffer);
  size_t total = 0;
  while (total < n) {
    const ssize_t wrote =
        ::pwrite(m_fd, in + total, std::min(n - total, kMaxIoBytes), static_cast<off_t>(offset + total));
    if (wrote < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw TransferError("cannot write " + m_path.string() + ": " + describeError());
    }
    total += static_cast<size_t>(wrote);
  }
}

void RandomAccessFile::resize(uint64_t size)
{
  if (::ftruncate(m_fd, static_cast<off_t>(size)) != 0) {
    throw TransferError("cannot size " + m_path.string() + ": " + describeError());
  }
}

#endif

} // namespace aether::transfer
