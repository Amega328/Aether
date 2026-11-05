/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

//! Clipboard file transfer packing
/*!
IClipboard::Format::Files payload layout (all integers big-endian):

  u32  entry count
  per entry:
    u8   flags (bit 0 = directory)
    u32  name length
    name bytes (UTF-8, '/'-separated, relative to a common root)
    u64  content size (0 for directories)
    size bytes of file content
*/
namespace aether::clipboard {

class ClipboardFilesError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

// Safety cap; the active clipboard size limit applies on top of this.
constexpr uint64_t kMaxPackBytes = 512ull * 1024 * 1024;

//! Pack the given files/directories into a Files clipboard payload.
/*!
Directories are packed recursively. Names are stored relative to each
root path's parent, so copying "D:\docs\report.txt" produces "report.txt".
Throws ClipboardFilesError when a path is unreadable or the total would
exceed maxBytes. Returns an empty string for an empty path list.
*/
std::string packFiles(const std::vector<std::filesystem::path> &paths, uint64_t maxBytes = kMaxPackBytes);

//! Unpack a Files clipboard payload into a staging directory.
/*!
Entry names are sanitized; anything escaping stagingDir throws
ClipboardFilesError. Returns the top-level paths created under
stagingDir, in payload order.
*/
std::vector<std::filesystem::path> unpackFiles(std::string_view data, const std::filesystem::path &stagingDir);

// Selections up to this size are packed into the clipboard itself, which also
// keeps them working with peers that predate large file transfer.
constexpr uint64_t kInlineBytes = 1ull * 1024 * 1024;

//! Files clipboard payload for a copied selection.
/*!
Small selections are packed inline. Anything larger (or exceeding
\c inlineLimit) is registered with the TransferHub and described by a
compact manifest; the bytes travel on separate connections. Files that a
finished transfer staged on this machine map back to their original manifest.
Throws ClipboardFilesError when a path is unreadable. Empty for no files.
*/
std::string offerFiles(const std::vector<std::filesystem::path> &paths, uint64_t inlineLimit = kInlineBytes);

//! Payload naming files that already exist on this machine (internal, never sent to peers).
std::string encodeLocalPaths(const std::vector<std::filesystem::path> &paths);
bool isLocalPathsPayload(std::string_view payload);
std::vector<std::filesystem::path> decodeLocalPaths(std::string_view payload);

//! Local top-level paths for a Files payload ready to hand to the OS.
/*!
Inline payloads are unpacked into \c stagingDir; local-path payloads are
returned as they are. A transfer manifest throws, since its bytes have to be
received first.
*/
std::vector<std::filesystem::path> materializeFiles(std::string_view payload, const std::filesystem::path &stagingDir);

//! Whether an entry name is safe to create under a staging directory.
/*!
Rejects empty or oversized names, absolute paths, drive prefixes, ".."
components, control characters and characters Windows filesystems refuse.
*/
bool isSafeEntryName(const std::string &name);

//! Percent-encoded "file://" URI for an absolute path.
std::string toFileUri(const std::filesystem::path &absolutePath);

//! Local paths named by a text/uri-list body.
/*!
Lines may end in CR, LF or CRLF. Comments, non-file schemes, remote hosts and
malformed URIs are skipped, so a gnome-copied-files body ("copy" or "cut" on
the first line) parses the same way. Trailing slashes are dropped so a
directory URI yields a path with a usable filename().
*/
std::vector<std::filesystem::path> fromUriList(std::string_view uriList);

} // namespace aether::clipboard
