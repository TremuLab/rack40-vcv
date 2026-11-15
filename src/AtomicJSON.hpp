// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <jansson.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace zoia {
// Same-directory replacement: failures before rename leave the user's old file
// intact. This does not promise durability across filesystem/power failures.
inline int saveJSON(json_t *value, const std::string &path) {
  // Serialize before opening any file, including rejecting null roots.
  char *encoded = json_dumps(value, JSON_INDENT(2));
  if (!encoded) return -1;
  std::string payload(encoded);
  free(encoded);
  std::string temporary;
  FILE *file = nullptr;
#ifdef _WIN32
  auto wide = [](const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> result(n > 0 ? n : 1, 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, result.data(), n);
    return result;
  };
  // Exclusive creation never truncates a pre-existing temporary file.
  for (unsigned i = 0; i < 100 && !file; ++i) {
    temporary = path + ".rack40-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(i) + ".tmp";
    auto name = wide(temporary);
    file = _wfopen(name.data(), L"wbx");
  }
#else
  temporary = path + ".rack40-XXXXXX";
  std::vector<char> name(temporary.begin(), temporary.end());
  name.push_back('\0');
  int fd = mkstemp(name.data());
  if (fd >= 0) {
    temporary = name.data();
    file = fdopen(fd, "wb");
    if (!file) { close(fd); std::remove(temporary.c_str()); }
  }
#endif
  if (!file) return -1;
  bool ok = std::fwrite(payload.data(), 1, payload.size(), file) == payload.size();
  if (std::fflush(file)) ok = false;
#ifdef _WIN32
  if (ok && _commit(_fileno(file))) ok = false;
#else
  if (ok && fsync(fileno(file))) ok = false;
#endif
  if (std::fclose(file)) ok = false;
#ifdef _WIN32
  auto from = wide(temporary), to = wide(path);
  if (ok) ok = MoveFileExW(from.data(), to.data(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  if (!ok) DeleteFileW(from.data());
#else
  if (ok) ok = std::rename(temporary.c_str(), path.c_str()) == 0;
  if (!ok) std::remove(temporary.c_str());
#endif
  return ok ? 0 : -1;
}
} // namespace zoia
