/* POSIX stand-in for the Windows CRT headers this plugin includes.
 * Only the calls Plugin.cpp actually makes are implemented. A path that
 * contains a backslash is the configuration path Orthanc returns on
 * Windows; the plugin strips the file name and changes to that directory
 * before exporting. Those paths are not created here. Relative export
 * paths are real directories in the test's working directory.
 */
#ifndef VPI_TEST_DIRECT_H
#define VPI_TEST_DIRECT_H

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef int errno_t;

static inline int _mkdir(const char* path)
{
  return mkdir(path, 0777);
}

static inline int _chdir(const char* path)
{
  if (path != NULL && strchr(path, '\\') != NULL)
    return 0;
  return chdir(path);
}

static inline char* _getcwd(char* buf, size_t size)
{
  return getcwd(buf, size);
}

static inline errno_t _get_errno(int* err)
{
  if (err != NULL)
    *err = errno;
  return 0;
}

#endif
