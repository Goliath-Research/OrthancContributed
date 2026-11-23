/* POSIX stand-in for the MSVC header Plugin.cpp includes.
   The product calls _mkdir, _chdir, _getcwd, and _get_errno from this header. */
#ifndef VPI_TEST_DIRECT_H
#define VPI_TEST_DIRECT_H

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef int errno_t;

static inline int _mkdir(const char* path)
{
  return mkdir(path, 0755);
}

static inline int _chdir(const char* path)
{
  return chdir(path);
}

static inline char* _getcwd(char* buffer, size_t size)
{
  return getcwd(buffer, size);
}

static inline void _get_errno(int* error)
{
  if (error != NULL)
  {
    *error = errno;
  }
}

#endif
