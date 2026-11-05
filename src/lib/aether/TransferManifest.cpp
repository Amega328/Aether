/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "aether/TransferManifest.h"

#include "aether/ClipboardFiles.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QRandomGenerator>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <unordered_set>

namespace fs = std::filesystem;

namespace aether::transfer {

namespace {

constexpr uint32_t kManifestMagic = 0x41465831; // "AFX1"
constexpr uint8_t kFlagDirectory = 0x01;
constexpr size_t kMaxNameBytes = 512;
constexpr size_t kMaxBodyBytes = 128ull * 1024 * 1024;
constexpr size_t kCompressThreshold = 1024;

std::string toUtf8(const fs::path &path)
{
  const auto u8 = path.generic_u8string();
  return {reinterpret_cast<const char *>(u8.data()), u8.size()};
}

QByteArray processSecret()
{
  static const QByteArray secret = [] {
    QByteArray bytes(32, '\0');
    QRandomGenerator::system()->fillRange(
        reinterpret_cast<quint32 *>(bytes.data()), static_cast<qsizetype>(bytes.size() / sizeof(quint32))
    );
    return bytes;
  }();
  return secret;
}

void encodeEntries(std::string &out, const std::vector<Entry> &entries)
{
  wire::putU32(out, static_cast<uint32_t>(entries.size()));
  for (const auto &entry : entries) {
    wire::putU8(out, entry.directory ? kFlagDirectory : 0);
    wire::putU64(out, entry.size);
    wire::putU64(out, static_cast<uint64_t>(entry.modified));
    wire::putU16(out, static_cast<uint16_t>(entry.name.size()));
    out += entry.name;
  }
}

Token computeToken(const std::vector<Entry> &entries)
{
  std::string body;
  encodeEntries(body, entries);

  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(processSecret());
  hash.addData(QByteArrayView(body.data(), static_cast<qsizetype>(body.size())));
  const QByteArray digest = hash.result();

  Token token{};
  std::memcpy(token.data(), digest.constData(), token.size());
  return token;
}

std::string uniqueTopName(const std::string &name, std::set<std::string> &used)
{
  if (used.insert(name).second) {
    return name;
  }

  const auto dot = name.find_last_of('.');
  const bool hasExtension = dot != std::string::npos && dot > 0;
  const std::string stem = hasExtension ? name.substr(0, dot) : name;
  const std::string extension = hasExtension ? name.substr(dot) : std::string{};
  for (int i = 2;; ++i) {
    auto candidate = stem + " (" + std::to_string(i) + ")" + extension;
    if (used.insert(candidate).second) {
      return candidate;
    }
  }
}

int64_t modifiedSeconds(const fs::path &path)
{
  std::error_code ec;
  const auto time = fs::last_write_time(path, ec);
  return ec ? 0 : toUnixSeconds(time);
}

void addEntry(Source &source, std::string name, const fs::path &path, bool directory, uint64_t size)
{
  if (!aether::clipboard::isSafeEntryName(name)) {
    throw TransferError("unsupported file name: " + name);
  }
  if (source.manifest.entries.size() >= kMaxEntries) {
    throw TransferError("too many files to transfer");
  }

  Entry entry;
  entry.name = std::move(name);
  entry.size = directory ? 0 : size;
  entry.modified = modifiedSeconds(path);
  entry.directory = directory;
  source.manifest.entries.push_back(std::move(entry));
  source.paths.push_back(path);
}

void addTree(Source &source, const fs::path &root, const std::string &rootName)
{
  std::error_code ec;
  fs::recursive_directory_iterator it(root, ec), end;
  if (ec) {
    throw TransferError("cannot list directory: " + root.string());
  }

  for (; it != end; it.increment(ec)) {
    if (ec) {
      break;
    }
    std::error_code statusEc;
    const auto status = it->status(statusEc);
    if (statusEc) {
      continue;
    }

    const auto relative = fs::relative(it->path(), root, statusEc);
    if (statusEc) {
      throw TransferError("cannot resolve path: " + it->path().string());
    }
    auto name = rootName + "/" + toUtf8(relative);

    if (fs::is_directory(status)) {
      addEntry(source, std::move(name), it->path(), true, 0);
    } else if (fs::is_regular_file(status)) {
      const auto size = it->file_size(statusEc);
      if (statusEc) {
        throw TransferError("cannot size file: " + it->path().string());
      }
      addEntry(source, std::move(name), it->path(), false, size);
    }
  }
  if (ec) {
    throw TransferError("cannot read directory: " + root.string());
  }
}

uint32_t readBigEndian32(const char *bytes)
{
  const auto *u = reinterpret_cast<const unsigned char *>(bytes);
  return (static_cast<uint32_t>(u[0]) << 24) | (static_cast<uint32_t>(u[1]) << 16) |
         (static_cast<uint32_t>(u[2]) << 8) | static_cast<uint32_t>(u[3]);
}

} // namespace

uint64_t Manifest::totalBytes() const
{
  uint64_t total = 0;
  for (const auto &entry : entries) {
    total += entry.size;
  }
  return total;
}

std::string tokenToHex(const Token &token)
{
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(token.size() * 2);
  for (const auto byte : token) {
    hex += kDigits[byte >> 4];
    hex += kDigits[byte & 0x0f];
  }
  return hex;
}

std::optional<Token> tokenFromHex(std::string_view hex)
{
  Token token{};
  if (hex.size() != token.size() * 2) {
    return std::nullopt;
  }

  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return -1;
  };

  for (size_t i = 0; i < token.size(); ++i) {
    const int hi = nibble(hex[i * 2]);
    const int lo = nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    token[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return token;
}

Source buildSource(const std::vector<fs::path> &paths)
{
  Source source;
  std::set<std::string> usedTopNames;

  for (const auto &given : paths) {
    if (given.empty()) {
      continue;
    }

    auto path = given;
    if (!path.has_filename()) {
      path = path.parent_path();
    }

    std::error_code ec;
    const auto status = fs::status(path, ec);
    if (ec) {
      throw TransferError("unreadable path: " + path.string());
    }

    const bool directory = fs::is_directory(status);
    if (!directory && !fs::is_regular_file(status)) {
      continue;
    }

    auto name = uniqueTopName(toUtf8(path.filename()), usedTopNames);
    if (directory) {
      addEntry(source, name, path, true, 0);
      addTree(source, path, name);
    } else {
      const auto size = fs::file_size(path, ec);
      if (ec) {
        throw TransferError("cannot size file: " + path.string());
      }
      addEntry(source, std::move(name), path, false, size);
    }
  }

  uint64_t total = 0;
  for (const auto &entry : source.manifest.entries) {
    total += entry.size;
    if (total > kMaxTotalBytes) {
      throw TransferError("selection is too large to transfer");
    }
  }

  source.manifest.token = computeToken(source.manifest.entries);
  return source;
}

std::string encodeManifest(const Manifest &manifest)
{
  std::string body;
  encodeEntries(body, manifest.entries);

  std::string out;
  wire::putU32(out, kManifestMagic);
  out.append(reinterpret_cast<const char *>(manifest.token.data()), manifest.token.size());

  if (body.size() > kCompressThreshold) {
    const QByteArray packed = qCompress(reinterpret_cast<const uchar *>(body.data()), static_cast<int>(body.size()), 6);
    if (static_cast<size_t>(packed.size()) < body.size()) {
      wire::putU8(out, 1);
      wire::putU32(out, static_cast<uint32_t>(body.size()));
      out.append(packed.constData(), static_cast<size_t>(packed.size()));
      return out;
    }
  }

  wire::putU8(out, 0);
  wire::putU32(out, static_cast<uint32_t>(body.size()));
  out += body;
  return out;
}

bool isManifestPayload(std::string_view payload)
{
  return payload.size() >= 4 && readBigEndian32(payload.data()) == kManifestMagic;
}

Manifest decodeManifest(std::string_view payload)
{
  wire::Reader reader(payload);
  if (reader.u32() != kManifestMagic) {
    throw TransferError("not a transfer manifest");
  }

  Manifest manifest;
  const auto tokenBytes = reader.bytes(manifest.token.size());
  std::memcpy(manifest.token.data(), tokenBytes.data(), manifest.token.size());

  const bool compressed = reader.u8() != 0;
  const uint32_t rawLength = reader.u32();
  if (rawLength > kMaxBodyBytes) {
    throw TransferError("transfer manifest is too large");
  }
  const auto rest = reader.bytes(reader.remaining());

  std::string body;
  if (compressed) {
    if (rest.size() < 4 || readBigEndian32(rest.data()) != rawLength) {
      throw TransferError("corrupt transfer manifest");
    }
    const QByteArray unpacked =
        qUncompress(reinterpret_cast<const uchar *>(rest.data()), static_cast<int>(rest.size()));
    if (static_cast<size_t>(unpacked.size()) != rawLength) {
      throw TransferError("corrupt transfer manifest");
    }
    body.assign(unpacked.constData(), static_cast<size_t>(unpacked.size()));
  } else {
    if (rest.size() != rawLength) {
      throw TransferError("corrupt transfer manifest");
    }
    body.assign(rest);
  }

  wire::Reader entries(body);
  const uint32_t count = entries.u32();
  if (count > kMaxEntries) {
    throw TransferError("too many files in transfer manifest");
  }

  std::unordered_set<std::string> seen;
  seen.reserve(count);
  uint64_t total = 0;
  manifest.entries.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Entry entry;
    const uint8_t flags = entries.u8();
    if ((flags & ~kFlagDirectory) != 0) {
      throw TransferError("unknown flags in transfer manifest");
    }
    entry.directory = (flags & kFlagDirectory) != 0;
    entry.size = entries.u64();
    entry.modified = static_cast<int64_t>(entries.u64());

    const uint16_t nameLength = entries.u16();
    if (nameLength > kMaxNameBytes) {
      throw TransferError("file name too long in transfer manifest");
    }
    entry.name.assign(entries.bytes(nameLength));

    if (!aether::clipboard::isSafeEntryName(entry.name)) {
      throw TransferError("unsafe file name in transfer manifest");
    }
    if (entry.directory && entry.size != 0) {
      throw TransferError("directory with content in transfer manifest");
    }
    if (!seen.insert(entry.name).second) {
      throw TransferError("duplicate file name in transfer manifest");
    }

    total += entry.size;
    if (entry.size > kMaxTotalBytes || total > kMaxTotalBytes) {
      throw TransferError("transfer is too large");
    }
    manifest.entries.push_back(std::move(entry));
  }

  if (entries.remaining() != 0) {
    throw TransferError("trailing data in transfer manifest");
  }
  return manifest;
}

std::vector<Unit> layoutUnits(const Manifest &manifest)
{
  std::vector<Unit> units;
  for (size_t i = 0; i < manifest.entries.size(); ++i) {
    const auto &entry = manifest.entries[i];
    if (entry.directory) {
      continue;
    }
    for (uint64_t offset = 0; offset < entry.size; offset += kUnitBytes) {
      units.push_back({static_cast<uint32_t>(i), offset, std::min(kUnitBytes, entry.size - offset)});
    }
  }
  return units;
}

std::vector<Unit> streamUnits(const std::vector<Unit> &units, unsigned stream, unsigned streamCount)
{
  std::vector<Unit> mine;
  for (size_t i = stream; i < units.size(); i += streamCount) {
    mine.push_back(units[i]);
  }
  return mine;
}

uint64_t unitBytes(const std::vector<Unit> &units)
{
  uint64_t total = 0;
  for (const auto &unit : units) {
    total += unit.length;
  }
  return total;
}

unsigned streamCountFor(uint64_t totalBytes)
{
  if (totalBytes < 16ull * 1024 * 1024) {
    return 1;
  }
  if (totalBytes < 128ull * 1024 * 1024) {
    return 2;
  }
  return kDefaultStreams;
}

unsigned plannedStreams(const Manifest &manifest)
{
  const auto units = layoutUnits(manifest).size();
  return static_cast<unsigned>(
      std::clamp<size_t>(streamCountFor(manifest.totalBytes()), 1, std::max<size_t>(1, units))
  );
}

fs::path pathForEntry(const fs::path &root, const std::string &name)
{
  return root / fs::path(std::u8string_view(reinterpret_cast<const char8_t *>(name.data()), name.size()));
}

std::vector<std::string> topLevelNames(const Manifest &manifest)
{
  std::vector<std::string> names;
  std::set<std::string> seen;
  for (const auto &entry : manifest.entries) {
    auto top = entry.name.substr(0, entry.name.find('/'));
    if (seen.insert(top).second) {
      names.push_back(std::move(top));
    }
  }
  return names;
}

int64_t toUnixSeconds(fs::file_time_type time)
{
  const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
  return std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
}

fs::file_time_type fromUnixSeconds(int64_t seconds)
{
  const std::chrono::system_clock::time_point system{std::chrono::seconds(seconds)};
  return std::chrono::clock_cast<fs::file_time_type::clock>(system);
}

} // namespace aether::transfer
