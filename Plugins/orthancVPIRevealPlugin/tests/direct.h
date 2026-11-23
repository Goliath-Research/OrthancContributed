/* Stand-in for the Windows <direct.h> calls in Plugin.cpp.
   _mkdir and _chdir accept '/' and '\\' (CRT directory routines) and
   resolve '.' and '..' components against the working directory. */
#ifndef VPI_TEST_DIRECT_H
#define VPI_TEST_DIRECT_H

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int errno_t;

#ifdef __cplusplus
extern "C" {
#endif

void vpi_test_note_mkdir(const char* absolute_path, int created_new);

#ifdef __cplusplus
}
#endif

static int vpi_is_sep(char c)
{
  return c == '/' || c == '\\';
}

/* Resolve path into an absolute host path. Returns 0 on success. */
static int vpi_resolve_path(const char* input, char* out, size_t outlen)
{
  char base[4096];
  char comps[256][256];
  int ncomps = 0;
  const char* p;
  int absolute;

  if (input == NULL || input[0] == '\0' || outlen < 2)
  {
    errno = ENOENT;
    return -1;
  }

  absolute = vpi_is_sep(input[0]);
  if (!absolute)
  {
    if (getcwd(base, sizeof(base)) == NULL)
      return -1;
    p = base;
    while (*p)
    {
      while (vpi_is_sep(*p))
        p++;
      if (*p == '\0')
        break;
      {
        char comp[256];
        size_t k = 0;
        while (*p && !vpi_is_sep(*p))
        {
          if (k + 1 >= sizeof(comp))
          {
            errno = ENAMETOOLONG;
            return -1;
          }
          comp[k++] = *p++;
        }
        comp[k] = '\0';
        if (strcmp(comp, ".") == 0)
          continue;
        if (strcmp(comp, "..") == 0)
        {
          if (ncomps > 0)
            ncomps--;
          continue;
        }
        if (ncomps >= 256)
        {
          errno = ENAMETOOLONG;
          return -1;
        }
        memcpy(comps[ncomps], comp, k + 1);
        ncomps++;
      }
    }
  }

  p = input;
  while (*p)
  {
    while (vpi_is_sep(*p))
      p++;
    if (*p == '\0')
      break;
    {
      char comp[256];
      size_t k = 0;
      while (*p && !vpi_is_sep(*p))
      {
        if (k + 1 >= sizeof(comp))
        {
          errno = ENAMETOOLONG;
          return -1;
        }
        comp[k++] = *p++;
      }
      comp[k] = '\0';
      if (strcmp(comp, ".") == 0 || comp[0] == '\0')
        continue;
      if (strcmp(comp, "..") == 0)
      {
        if (ncomps > 0)
          ncomps--;
        continue;
      }
      if (ncomps >= 256)
      {
        errno = ENAMETOOLONG;
        return -1;
      }
      memcpy(comps[ncomps], comp, k + 1);
      ncomps++;
    }
  }

  if (ncomps == 0)
  {
    if (outlen < 2)
    {
      errno = ENAMETOOLONG;
      return -1;
    }
    out[0] = '/';
    out[1] = '\0';
    return 0;
  }

  {
    size_t used = 0;
    int i;
    out[0] = '\0';
    for (i = 0; i < ncomps; i++)
    {
      size_t len = strlen(comps[i]);
      if (used + 1 + len + 1 > outlen)
      {
        errno = ENAMETOOLONG;
        return -1;
      }
      out[used++] = '/';
      memcpy(out + used, comps[i], len);
      used += len;
      out[used] = '\0';
    }
  }
  return 0;
}

static int _mkdir(const char* path)
{
  char resolved[4096];
  if (vpi_resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;
  if (mkdir(resolved, 0755) == 0)
  {
    vpi_test_note_mkdir(resolved, 1);
    return 0;
  }
  vpi_test_note_mkdir(resolved, 0);
  return -1;
}

static int _chdir(const char* path)
{
  char resolved[4096];
  if (vpi_resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;
  return chdir(resolved);
}

static char* _getcwd(char* buffer, size_t size)
{
  return getcwd(buffer, size);
}

static errno_t _get_errno(int* result)
{
  if (result == NULL)
    return EINVAL;
  *result = errno;
  return 0;
}

#endif
