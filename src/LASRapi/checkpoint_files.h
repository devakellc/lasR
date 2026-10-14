#ifndef LASR_CHECKPOINT_FILES_H
#define LASR_CHECKPOINT_FILES_H

#include <filesystem>

// Keep platform headers out of the engine translation unit: Windows typedefs
// such as INT32 and FLOAT conflict with the existing attribute enumerators.
namespace checkpoint_files
{
class Lock
{
public:
  explicit Lock(const std::filesystem::path& path);
  ~Lock();
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
private:
#ifdef _WIN32
  void* handle;
#else
  int handle;
#endif
};

std::filesystem::path engine_path();
void publish(const std::filesystem::path& source,
             const std::filesystem::path& destination, bool replace = true);
}

#endif
