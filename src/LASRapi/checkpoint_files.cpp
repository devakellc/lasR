#include "checkpoint_files.h"

#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace checkpoint_files
{
namespace fs = std::filesystem;

namespace
{
[[noreturn]] void fail(const std::string& message)
{
  throw std::runtime_error("Checkpoint: " + message);
}
}

fs::path engine_path()
{
  static const char marker = 0;
  fs::path module_path;
#ifdef _WIN32
  HMODULE module;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&marker), &module))
    fail("cannot identify the engine build");
  std::vector<wchar_t> path(32768);
  DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length == path.size()) fail("cannot identify the engine build");
  module_path = std::wstring(path.data(), length);
#else
  Dl_info info;
  if (!dladdr(&marker, &info) || !info.dli_fname) fail("cannot identify the engine build");
  module_path = info.dli_fname;
#endif
  return module_path;
}

// Process-owned locks release after a crash. Never unlink the lock file: a new
// inode would let a second process lock the same checkpoint independently.
Lock::Lock(const fs::path& path)
{
#ifdef _WIN32
  handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) fail("directory is locked by another process");
#else
  handle = open(path.c_str(), O_CREAT | O_RDWR, 0600);
  if (handle < 0) fail("cannot open lock file");
  if (flock(handle, LOCK_EX | LOCK_NB) != 0)
  {
    close(handle);
    fail("directory is locked by another process");
  }
#endif
}
Lock::~Lock()
{
#ifdef _WIN32
  CloseHandle(handle);
#else
  close(handle);
#endif
}

void sync_file(const fs::path& path)
{
#ifdef _WIN32
  HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) fail("cannot flush " + path.string());
  bool ok = FlushFileBuffers(h);
  CloseHandle(h);
#else
  int h = open(path.c_str(), O_RDONLY);
  if (h < 0) fail("cannot flush " + path.string());
  bool ok = fsync(h) == 0;
  close(h);
#endif
  if (!ok) fail("cannot flush " + path.string());
}

void publish(const fs::path& source, const fs::path& destination, bool replace)
{
  sync_file(source);
#ifdef _WIN32
  if (!MoveFileExW(source.wstring().c_str(), destination.wstring().c_str(),
                   (replace ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH))
    fail("cannot publish " + destination.string());
#else
  if (replace) fs::rename(source, destination);
  else
  {
    // link() atomically refuses an output created after preflight by another job.
    if (link(source.c_str(), destination.c_str()) != 0)
      fail("cannot publish without overwriting an existing output: " + destination.string());
    fs::remove(source);
  }
  int fd = open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) fail("cannot flush output directory");
  bool ok = fsync(fd) == 0;
  close(fd);
  if (!ok) fail("cannot flush output directory");
#endif
}

}
