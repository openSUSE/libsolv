#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <windows.h>

FILE *
fmemopen(void *buf, size_t size, const char *mode)
{
  char temppath[MAX_PATH + 1];
  char tempnam[MAX_PATH + 1];
  DWORD l;
  HANDLE fh;
  FILE *fp;

  if (strcmp(mode, "r") != 0 && strcmp(mode, "r+") != 0)
    return 0;
  l = GetTempPath(MAX_PATH, temppath);
  if (!l || l >= MAX_PATH)
    return 0;
  if (!GetTempFileName(temppath, "solvtmp", 0, tempnam))
    return 0;
  fh = CreateFile(tempnam, DELETE | GENERIC_READ | GENERIC_WRITE, 0,
                  NULL, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE,
                  NULL);
  if (fh == INVALID_HANDLE_VALUE)
    return 0;
  fp = _fdopen(_open_osfhandle((intptr_t)fh, 0), "w+b");
  if (!fp)
    {
      CloseHandle(fh);
      return 0;
    }
  if (buf && size && fwrite(buf, size, 1, fp) != 1)
    {
      fclose(fp);
      return 0;
    }
  rewind(fp);
  return fp;
}
