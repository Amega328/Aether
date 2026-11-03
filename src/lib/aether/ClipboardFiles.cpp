/*
 * Aether -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Rimora Studio
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "aether/ClipboardFiles.h"

#include "base/Log.h"

#include <cctype>
#include <fstream>
#include <optional>
#include <set>

namespace {

constexpr uint8_t kFlagDirectory = 0x01;

void writeUInt32(std::string &out, uint32_t v)
{
  out += static_cast<char>((v >> 24) & 0xff);
  out += static_cast<char>((v >> 16) & 0xff);
  out += static_cast<char>((v >> 8) & 0xff);
  out += static_cast<char>(v & 0xff);
}

void writeUInt64(std::string &out, uint64_t v)
{
  writeUInt32(out, static_cast<uint32_t>(v >> 32));
  writeUInt32(out, static_cast<uint32_t>(v & 0xffffffff));
}

uint32_t readUInt32(std::string_view &in)
{
  if (in.size() < 4)
    throw aether::clipboard::ClipboardFilesError("truncated uint32");
  const auto *u = reinterpret_cast<const unsigned char *>(in.data());
  const uint32_t v = (static_cast<uint32_t>(u[0]) << 24) | (static_cast<uint32_t>(u[1]) << 16) |
                     (static_cast<uint32_t>(u[2]) << 8) | static_cast<uint32_t>(u[3]);
  in.remove_prefix(4);
  return v;
}

uint64_t readUInt64(std::string_view &in)
{
  return (static_cast<uint64_t>(readUInt32(in)) << 32) | readUInt32(in);
}

std::string toUtf8(const std::filesystem::path &path)
{
  const auto u8 = path.generic_u8string();
  return {reinterpret_cast<const char *>(u8.data()), u8.size()};
}

void appendEntry(std::string &out, const std::string &name, bool isDir, uint64_t size)
{
  out += static_cast<char>(isDir ? kFlagDirectory : 0);
  writeUInt32(out, static_cast<uint32_t>(name.size()));
  out += name;
  writeUInt64(out, size);
}

uint64_t addPath(
    std::string &out, uint32_t &count, const std::filesystem::path &fsPath, const std::string &name, uint64_t maxBytes,
    uint64_t used
)
{
  std::error_code ec;
  const auto status = std::filesystem::status(fsPath, ec);
  if (ec)
    throw aether::clipboard::ClipboardFilesError("unreadable path: " + fsPath.string());

  if (std::filesystem::is_directory(status)) {
    appendEntry(out, name, true, 0);
    ++count;
    std::filesystem::recursive_directory_iterator it(fsPath, ec), end;
    if (ec)
      throw aether::clipboard::ClipboardFilesError("cannot list directory: " + fsPath.string());
    for (; !ec && it != end; it.increment(ec)) {
      const auto rel = std::filesystem::relative(it->path(), fsPath, ec);
      if (ec)
        throw aether::clipboard::ClipboardFilesError("cannot resolve path: " + it->path().string());
      const auto childName = name + "/" + toUtf8(rel);
      if (std::filesystem::is_directory(it->status(ec))) {
        appendEntry(out, childName, true, 0);
        ++count;
      } else if (std::filesystem::is_regular_file(it->status(ec))) {
        const auto fileSize = it->file_size(ec);
        if (ec)
          throw aether::clipboard::ClipboardFilesError("cannot size file: " + it->path().string());
        used += fileSize;
        if (used > maxBytes)
          throw aether::clipboard::ClipboardFilesError("clipboard files exceed size limit");
        appendEntry(out, childName, false, fileSize);
        ++count;

        std::ifstream file(it->path(), std::ios::binary);
        if (!file)
          throw aether::clipboard::ClipboardFilesError("cannot read file: " + it->path().string());
        std::string content(fileSize, '\0');
        if (fileSize > 0)
          file.read(content.data(), static_cast<std::streamsize>(fileSize));
        out += content;
      }
    }
    if (ec)
      throw aether::clipboard::ClipboardFilesError("cannot read directory: " + fsPath.string());
    return used;
  }

  if (!std::filesystem::is_regular_file(status))
    return used;

  const auto fileSize = std::filesystem::file_size(fsPath, ec);
  if (ec)
    throw aether::clipboard::ClipboardFilesError("cannot size file: " + fsPath.string());
  used += fileSize;
  if (used > maxBytes)
    throw aether::clipboard::ClipboardFilesError("clipboard files exceed size limit");

  std::ifstream file(fsPath, std::ios::binary);
  if (!file)
    throw aether::clipboard::ClipboardFilesError("cannot read file: " + fsPath.string());

  appendEntry(out, name, false, fileSize);
  ++count;
  if (fileSize > 0) {
    std::string content(fileSize, '\0');
    file.read(content.data(), static_cast<std::streamsize>(fileSize));
    out += content;
  }
  return used;
}

// Reject names that could escape the staging directory or are
// invalid on common filesystems.
bool isSafeName(const std::string &name)
{
  if (name.empty() || name.size() > 512)
    return false;
  if (name.front() == '/' || name.front() == '\\')
    return false;
  if (name.size() >= 2 && name[1] == ':')
    return false;
  for (const char c : name) {
    if (c == '\\' || c == ':' || c == '<' || c == '>' || c == '|' || c == '?' || c == '*' ||
        static_cast<unsigned char>(c) < 0x20)
      return false;
  }
  for (const auto &part :
       std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t *>(name.data()), name.size()))) {
    if (part == "..")
      return false;
  }
  return true;
}

} // namespace

std::string aether::clipboard::packFiles(const std::vector<std::filesystem::path> &paths, uint64_t maxBytes)
{
  if (paths.empty())
    return {};

  std::string out;
  uint32_t count = 0;
  uint64_t used = 0;
  writeUInt32(out, 0); // patched with the entry count below

  for (const auto &path : paths) {
    if (path.empty())
      continue;
    used = addPath(out, count, path, toUtf8(path.filename()), maxBytes, used);
  }

  if (count == 0)
    return {};

  // patch the entry count
  out[0] = static_cast<char>((count >> 24) & 0xff);
  out[1] = static_cast<char>((count >> 16) & 0xff);
  out[2] = static_cast<char>((count >> 8) & 0xff);
  out[3] = static_cast<char>(count & 0xff);

  LOG_DEBUG("packed %u clipboard file entries (%d bytes)", count, out.size());
  return out;
}

std::vector<std::filesystem::path>
aether::clipboard::unpackFiles(std::string_view data, const std::filesystem::path &stagingDir)
{
  const uint32_t count = readUInt32(data);
  if (count > 100000)
    throw ClipboardFilesError("unreasonable entry count");

  std::vector<std::filesystem::path> topLevel;
  std::set<std::string> seenTop;

  for (uint32_t i = 0; i < count; ++i) {
    if (data.empty())
      throw ClipboardFilesError("truncated entry flags");
    const uint8_t flags = static_cast<uint8_t>(data.front());
    data.remove_prefix(1);

    const uint32_t nameLen = readUInt32(data);
    if (nameLen > data.size())
      throw ClipboardFilesError("truncated entry name");
    const std::string name(data.substr(0, nameLen));
    data.remove_prefix(nameLen);

    const uint64_t size = readUInt64(data);

    if (!isSafeName(name))
      throw ClipboardFilesError("unsafe entry name: " + name);

    const auto target =
        stagingDir /
        std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t *>(name.data()), name.size()));

    const auto topComponent = name.substr(0, name.find('/'));
    if (seenTop.insert(topComponent).second)
      topLevel.push_back(
          stagingDir /
          std::filesystem::path(
              std::u8string_view(reinterpret_cast<const char8_t *>(topComponent.data()), topComponent.size())
          )
      );

    if (flags & kFlagDirectory) {
      std::filesystem::create_directories(target);
      continue;
    }

    if (size > data.size())
      throw ClipboardFilesError("truncated file content");

    std::filesystem::create_directories(target.parent_path());
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file)
      throw ClipboardFilesError("cannot write file: " + target.string());
    if (size > 0)
      file.write(data.data(), static_cast<std::streamsize>(size));
    file.close();
    data.remove_prefix(size);
  }

  LOG_DEBUG("unpacked %u clipboard file entries to %s", count, stagingDir.string().c_str());
  return topLevel;
}

std::string aether::clipboard::toFileUri(const std::filesystem::path &absolutePath)
{
  constexpr std::string_view kHex = "0123456789ABCDEF";
  const auto bytes = toUtf8(absolutePath);

  std::string uri = "file://";
  if (bytes.empty() || bytes.front() != '/')
    uri += '/';
  for (const char ch : bytes) {
    const auto c = static_cast<unsigned char>(ch);
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                       c == '.' || c == '_' || c == '~' || c == '/';
    if (plain) {
      uri += ch;
    } else {
      uri += '%';
      uri += kHex[c >> 4];
      uri += kHex[c & 0x0f];
    }
  }
  return uri;
}

namespace {

int hexValue(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

bool startsWithNoCase(std::string_view text, std::string_view prefix)
{
  if (text.size() < prefix.size())
    return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i])
      return false;
  }
  return true;
}

std::optional<std::filesystem::path> pathFromFileUri(std::string_view uri)
{
  if (!startsWithNoCase(uri, "file:"))
    return std::nullopt;
  uri.remove_prefix(5);

  if (uri.starts_with("//")) {
    uri.remove_prefix(2);
    const auto hostEnd = uri.find('/');
    if (hostEnd == std::string_view::npos)
      return std::nullopt;
    const auto host = uri.substr(0, hostEnd);
    constexpr std::string_view kLocalhost = "localhost";
    if (!host.empty() && !(host.size() == kLocalhost.size() && startsWithNoCase(host, kLocalhost)))
      return std::nullopt;
    uri.remove_prefix(hostEnd);
  }

  std::string decoded;
  decoded.reserve(uri.size());
  for (size_t i = 0; i < uri.size(); ++i) {
    if (uri[i] != '%') {
      decoded += uri[i];
      continue;
    }
    if (i + 2 >= uri.size())
      return std::nullopt;
    const int hi = hexValue(uri[i + 1]);
    const int lo = hexValue(uri[i + 2]);
    if (hi < 0 || lo < 0)
      return std::nullopt;
    decoded += static_cast<char>((hi << 4) | lo);
    i += 2;
  }

  if (decoded.empty() || decoded.front() != '/' || decoded.find('\0') != std::string::npos)
    return std::nullopt;
  while (decoded.size() > 1 && decoded.back() == '/')
    decoded.pop_back();

  return std::filesystem::path(std::u8string(decoded.begin(), decoded.end()));
}

} // namespace

std::vector<std::filesystem::path> aether::clipboard::fromUriList(std::string_view uriList)
{
  std::vector<std::filesystem::path> paths;
  while (!uriList.empty()) {
    const auto lineEnd = uriList.find_first_of("\r\n");
    const auto line = uriList.substr(0, lineEnd);
    uriList.remove_prefix(lineEnd == std::string_view::npos ? uriList.size() : lineEnd + 1);

    if (line.empty() || line.front() == '#')
      continue;
    if (auto path = pathFromFileUri(line))
      paths.push_back(std::move(*path));
  }
  return paths;
}
