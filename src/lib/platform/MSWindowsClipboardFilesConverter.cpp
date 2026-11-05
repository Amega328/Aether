/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsClipboardFilesConverter.h"

#include "aether/ClipboardFiles.h"
#include "base/Log.h"

#include <shlobj.h>

#include <cstdlib>
#include <filesystem>

IClipboard::Format MSWindowsClipboardFilesConverter::getFormat() const
{
  return IClipboard::Format::Files;
}

UINT MSWindowsClipboardFilesConverter::getWin32Format() const
{
  return CF_HDROP;
}

HANDLE MSWindowsClipboardFilesConverter::fromIClipboard(const std::string &data) const
{
  // Staged under %TEMP%\AetherClipboard\<pid>-<counter>. Stale dirs are
  // cleaned on process start so pastes still in progress aren't cut off.
  static const auto stagingBase = std::filesystem::temp_directory_path() / "AetherClipboard";
  static uint32_t counter = 0;
  static const bool cleaned = [] {
    std::error_code ec;
    if (!std::filesystem::exists(stagingBase, ec))
      return true;
    for (const auto &entry : std::filesystem::directory_iterator(stagingBase, ec)) {
      std::filesystem::remove_all(entry.path(), ec);
      ec.clear();
    }
    return true;
  }();
  (void)cleaned;

  const auto stagingDir =
      stagingBase / ("clip-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++));

  std::vector<std::filesystem::path> staged;
  try {
    staged = aether::clipboard::materializeFiles(data, stagingDir);
  } catch (const std::exception &e) {
    LOG_WARN("failed to unpack clipboard files: %s", e.what());
    std::error_code ec;
    std::filesystem::remove_all(stagingDir, ec);
    return nullptr;
  }

  if (staged.empty())
    return nullptr;

  std::wstring paths;
  for (const auto &path : staged) {
    paths += path.wstring();
    paths += L'\0';
  }
  paths += L'\0';

  const SIZE_T bytes = sizeof(DROPFILES) + (paths.size() + 1) * sizeof(wchar_t);
  HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, bytes);
  if (handle == nullptr)
    return nullptr;

  auto *drop = static_cast<DROPFILES *>(GlobalLock(handle));
  if (drop == nullptr) {
    GlobalFree(handle);
    return nullptr;
  }
  drop->pFiles = sizeof(DROPFILES);
  drop->pt.x = 0;
  drop->pt.y = 0;
  drop->fNC = FALSE;
  drop->fWide = TRUE;
  memcpy(
      reinterpret_cast<wchar_t *>(drop) + (sizeof(DROPFILES) / sizeof(wchar_t)), paths.c_str(),
      paths.size() * sizeof(wchar_t)
  );
  GlobalUnlock(handle);

  return handle;
}

std::string MSWindowsClipboardFilesConverter::toIClipboard(HANDLE data) const
{
  auto *drop = static_cast<DROPFILES *>(GlobalLock(data));
  if (drop == nullptr)
    return {};

  std::vector<std::filesystem::path> paths;
  if (drop->fWide) {
    const auto *index = reinterpret_cast<const wchar_t *>(reinterpret_cast<const char *>(drop) + drop->pFiles);
    while (*index != L'\0') {
      paths.emplace_back(index);
      index += wcslen(index) + 1;
    }
  } else {
    const auto *index = reinterpret_cast<const char *>(drop) + drop->pFiles;
    while (*index != '\0') {
      paths.emplace_back(index);
      index += strlen(index) + 1;
    }
  }
  GlobalUnlock(data);

  try {
    return aether::clipboard::offerFiles(paths);
  } catch (const std::exception &e) {
    LOG_WARN("failed to pack clipboard files: %s", e.what());
    return {};
  }
}
