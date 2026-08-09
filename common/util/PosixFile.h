#pragma once

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <stdio.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace posix_file {

class OwnedFd {
 public:
  OwnedFd() = default;
  explicit OwnedFd(int value) : m_value(value) {}
  ~OwnedFd() { reset(); }

  OwnedFd(const OwnedFd&) = delete;
  OwnedFd& operator=(const OwnedFd&) = delete;
  OwnedFd(OwnedFd&& other) noexcept : m_value(other.release()) {}
  OwnedFd& operator=(OwnedFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  explicit operator bool() const { return m_value >= 0; }
  int get() const { return m_value; }
  int release() {
    const int result = m_value;
    m_value = -1;
    return result;
  }
  void reset(int value = -1) {
    if (m_value >= 0) {
      ::close(m_value);
    }
    m_value = value;
  }

 private:
  int m_value = -1;
};

struct Identity {
  dev_t device = 0;
  ino_t inode = 0;
};

inline bool descriptor_identity(int descriptor, Identity* result, struct stat* status = nullptr) {
  struct stat local_status {};
  if (::fstat(descriptor, &local_status) != 0) {
    return false;
  }
  if (result) {
    *result = {local_status.st_dev, local_status.st_ino};
  }
  if (status) {
    *status = local_status;
  }
  return true;
}

inline bool same_identity(const struct stat& status, const Identity& expected) {
  return status.st_dev == expected.device && status.st_ino == expected.inode;
}

inline bool entry_identity(int directory,
                           std::string_view name,
                           const Identity& expected,
                           struct stat* status = nullptr) {
  const std::string owned_name(name);
  struct stat local_status {};
  if (::fstatat(directory, owned_name.c_str(), &local_status, AT_SYMLINK_NOFOLLOW) != 0 ||
      !same_identity(local_status, expected)) {
    return false;
  }
  if (status) {
    *status = local_status;
  }
  return true;
}

inline OwnedFd open_directory(const char* path) {
  return OwnedFd(::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
}

inline OwnedFd open_directory_at(int directory, std::string_view name) {
  const std::string owned_name(name);
  return OwnedFd(::openat(directory, owned_name.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
}

inline OwnedFd open_file_at(int directory, std::string_view name, int flags, mode_t mode = 0600) {
  const std::string owned_name(name);
  return OwnedFd(::openat(directory, owned_name.c_str(), flags | O_NOFOLLOW | O_CLOEXEC, mode));
}

inline int exclusive_rename_at(int source_directory,
                               std::string_view source_name,
                               int destination_directory,
                               std::string_view destination_name) {
  const std::string owned_source(source_name);
  const std::string owned_destination(destination_name);
#if defined(__APPLE__)
  return ::renameatx_np(source_directory, owned_source.c_str(), destination_directory,
                        owned_destination.c_str(), RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
  return static_cast<int>(::syscall(SYS_renameat2, source_directory, owned_source.c_str(),
                                    destination_directory, owned_destination.c_str(),
                                    RENAME_NOREPLACE));
#else
  (void)source_directory;
  (void)destination_directory;
  errno = ENOTSUP;
  return -1;
#endif
}

}  // namespace posix_file
