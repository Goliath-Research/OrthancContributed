/* Test shim for the Windows directory calls in Plugin.cpp.
   The series-label bug is in the string copy, which runs on this path
   with these calls succeeding. */
#ifndef VPI_TEST_DIRECT_H
#define VPI_TEST_DIRECT_H

#include <errno.h>
#include <stdio.h>
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

static inline char* _getcwd(char* buf, size_t size)
{
  return getcwd(buf, size);
}

static inline errno_t _get_errno(int* err)
{
  if (err != NULL)
  {
    *err = errno;
  }
  return 0;
}

#endif
