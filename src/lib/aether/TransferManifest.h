/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "aether/TransferWire.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

//! Large file transfer: file lists and how their bytes are split across streams
/*!
A copied selection is described by a Manifest (names, sizes, times and a
random-looking token) and sent through the clipboard in place of the file
contents. The bytes follow on dedicated connections. Both ends derive the same
split of those bytes from the manifest, so streams carry no per-block framing:

  - every file with content is cut into units of kUnitBytes;
  - unit i of the manifest, in entry order, belongs to stream (i % streamCount);
  - a stream is the concatenation of its units in order.
*/
namespace aether::transfer {

using Token = std::array<uint8_t, 16>;

struct Entry
{
  std::string name; //!< '/'-separated, relative to a common root
  uint64_t size = 0;
  int64_t modified = 0; //!< seconds since the Unix epoch, 0 when unknown
  bool directory = false;
};

struct Manifest
{
  Token token{};
  std::vector<Entry> entries;

  uint64_t totalBytes() const;
};

//! A contiguous byte range of one file, carried by exactly one stream
struct Unit
{
  uint32_t entry = 0;
  uint64_t offset = 0;
  uint64_t length = 0;
};

//! A manifest together with the local paths its entries were read from
struct Source
{
  Manifest manifest;
  std::vector<std::filesystem::path> paths; //!< parallel to manifest.entries
};

constexpr uint64_t kUnitBytes = 4ull * 1024 * 1024;
constexpr unsigned kDefaultStreams = 4;
constexpr unsigned kMaxStreams = 16;
constexpr uint32_t kMaxEntries = 1'000'000;
constexpr uint64_t kMaxTotalBytes = 1ull << 50;

std::string tokenToHex(const Token &token);
std::optional<Token> tokenFromHex(std::string_view hex);

//! Describe the given files and directories (recursively) without reading them.
/*!
Names are relative to each path's parent. Top-level name clashes are made
unique. Throws TransferError for unreadable paths; an empty selection yields
a Source with no entries.
*/
Source buildSource(const std::vector<std::filesystem::path> &paths);

//! Clipboard payload form of a manifest; always starts with a distinctive magic.
std::string encodeManifest(const Manifest &manifest);

//! Whether a Files clipboard payload is a manifest rather than packed contents.
bool isManifestPayload(std::string_view payload);

//! Parse and validate an encoded manifest. Throws TransferError when malformed.
Manifest decodeManifest(std::string_view payload);

std::vector<Unit> layoutUnits(const Manifest &manifest);
std::vector<Unit> streamUnits(const std::vector<Unit> &units, unsigned stream, unsigned streamCount);
uint64_t unitBytes(const std::vector<Unit> &units);

//! How many parallel streams a transfer of this size should use.
unsigned streamCountFor(uint64_t totalBytes);

//! Streams to use for a manifest: sized by bytes, never more than it has units.
unsigned plannedStreams(const Manifest &manifest);

//! Local path for a '/'-separated entry name under \c root.
std::filesystem::path pathForEntry(const std::filesystem::path &root, const std::string &name);

//! First path component of every entry, in order, without duplicates.
std::vector<std::string> topLevelNames(const Manifest &manifest);

int64_t toUnixSeconds(std::filesystem::file_time_type time);
std::filesystem::file_time_type fromUnixSeconds(int64_t seconds);

} // namespace aether::transfer
