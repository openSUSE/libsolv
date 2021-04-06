/*
 * Copyright (c) 2011, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#ifdef _WIN32
  #include "fmemopen.c"
#endif

#include "solv_xfopen.h"
#include "util.h"

#ifndef WITHOUT_COOKIEOPEN

static FILE *cookieopen(void *cookie, const char *mode,
	ssize_t (*cread)(void *, char *, size_t),
	ssize_t (*cwrite)(void *, const char *, size_t),
	int (*cclose)(void *))
{
#ifdef HAVE_FUNOPEN
  if (!cookie)
    return 0;
  return funopen(cookie,
      (int (*)(void *, char *, int))(*mode == 'r' ? cread : NULL),		/* readfn */
      (int (*)(void *, const char *, int))(*mode == 'w' ? cwrite : NULL),	/* writefn */
      (fpos_t (*)(void *, fpos_t, int))NULL,					/* seekfn */
      cclose
      );
#elif defined(HAVE_FOPENCOOKIE)
  cookie_io_functions_t cio;

  if (!cookie)
    return 0;
  memset(&cio, 0, sizeof(cio));
  if (*mode == 'r')
    cio.read = cread;
  else if (*mode == 'w')
    cio.write = cwrite;
  cio.close = cclose;
  return  fopencookie(cookie, *mode == 'w' ? "w" : "r", cio);
#else
# error Need to implement custom I/O
#endif
}


#ifdef ENABLE_ZLIB_COMPRESSION

/* gzip compression */

#include <zlib.h>

static ssize_t cookie_gzread(void *cookie, char *buf, size_t nbytes)
{
  ssize_t r = gzread((gzFile)cookie, buf, nbytes);
  if (r == 0)
    {
      int err = 0;
      gzerror((gzFile)cookie, &err);
      if (err == Z_BUF_ERROR)
	r = -1;
    }
  return r;
}

static ssize_t cookie_gzwrite(void *cookie, const char *buf, size_t nbytes)
{
  return gzwrite((gzFile)cookie, buf, nbytes);
}

static int cookie_gzclose(void *cookie)
{
  return gzclose((gzFile)cookie);
}

static inline FILE *mygzfopen(const char *fn, const char *mode)
{
  gzFile gzf = gzopen(fn, mode);
  return cookieopen(gzf, mode, cookie_gzread, cookie_gzwrite, cookie_gzclose);
}

static inline FILE *mygzfdopen(int fd, const char *mode)
{
  gzFile gzf = gzdopen(fd, mode);
  return cookieopen(gzf, mode, cookie_gzread, cookie_gzwrite, cookie_gzclose);
}

#endif


#ifdef ENABLE_BZIP2_COMPRESSION

/* bzip2 compression */

#include <bzlib.h>

static ssize_t cookie_bzread(void *cookie, char *buf, size_t nbytes)
{
  return BZ2_bzread((BZFILE *)cookie, buf, nbytes);
}

static ssize_t cookie_bzwrite(void *cookie, const char *buf, size_t nbytes)
{
  return BZ2_bzwrite((BZFILE *)cookie, (char *)buf, nbytes);
}

static int cookie_bzclose(void *cookie)
{
  BZ2_bzclose((BZFILE *)cookie);
  return 0;
}

static inline FILE *mybzfopen(const char *fn, const char *mode)
{
  BZFILE *bzf = BZ2_bzopen(fn, mode);
  return cookieopen(bzf, mode, cookie_bzread, cookie_bzwrite, cookie_bzclose);
}

static inline FILE *mybzfdopen(int fd, const char *mode)
{
  BZFILE *bzf = BZ2_bzdopen(fd, mode);
  return cookieopen(bzf, mode, cookie_bzread, cookie_bzwrite, cookie_bzclose);
}

#endif


#ifdef ENABLE_LZMA_COMPRESSION

/* lzma code written by me in 2008 for rpm's rpmio.c */

#include <lzma.h>

typedef struct lzfile {
  unsigned char buf[1 << 15];
  lzma_stream strm;
  FILE *file;
  int encoding;
  int eof;
} LZFILE;

static inline lzma_ret setup_alone_encoder(lzma_stream *strm, int level)
{
  lzma_options_lzma options;
  lzma_lzma_preset(&options, level);
  return lzma_alone_encoder(strm, &options);
}

static lzma_stream stream_init = LZMA_STREAM_INIT;

static LZFILE *lzopen(const char *path, const char *mode, int fd, int isxz)
{
  int level = 7;
  int encoding = 0;
  FILE *fp;
  LZFILE *lzfile;
  lzma_ret ret;

  if ((!path && fd < 0) || (path && fd >= 0))
    return 0;
  for (; *mode; mode++)
    {
      if (*mode == 'w')
	encoding = 1;
      else if (*mode == 'r')
	encoding = 0;
      else if (*mode >= '1' && *mode <= '9')
	level = *mode - '0';
    }
  lzfile = solv_calloc(1, sizeof(*lzfile));
  lzfile->encoding = encoding;
  lzfile->eof = 0;
  lzfile->strm = stream_init;
  if (encoding)
    {
      if (isxz)
	ret = lzma_easy_encoder(&lzfile->strm, level, LZMA_CHECK_SHA256);
      else
	ret = setup_alone_encoder(&lzfile->strm, level);
    }
  else
    ret = lzma_auto_decoder(&lzfile->strm, 100 << 20, 0);
  if (ret != LZMA_OK)
    {
      solv_free(lzfile);
      return 0;
    }
  if (!path)
    fp = fdopen(fd, encoding ? "w" : "r");
  else
    fp = fopen(path, encoding ? "w" : "r");
  if (!fp)
    {
      lzma_end(&lzfile->strm);
      solv_free(lzfile);
      return 0;
    }
  lzfile->file = fp;
  return lzfile;
}

static int lzclose(void *cookie)
{
  LZFILE *lzfile = cookie;
  lzma_ret ret;
  size_t n;
  int rc;

  if (!lzfile)
    return -1;
  if (lzfile->encoding)
    {
      for (;;)
	{
	  lzfile->strm.avail_out = sizeof(lzfile->buf);
	  lzfile->strm.next_out = lzfile->buf;
	  ret = lzma_code(&lzfile->strm, LZMA_FINISH);
	  if (ret != LZMA_OK && ret != LZMA_STREAM_END)
	    return -1;
	  n = sizeof(lzfile->buf) - lzfile->strm.avail_out;
	  if (n && fwrite(lzfile->buf, 1, n, lzfile->file) != n)
	    return -1;
	  if (ret == LZMA_STREAM_END)
	    break;
	}
    }
  lzma_end(&lzfile->strm);
  rc = fclose(lzfile->file);
  solv_free(lzfile);
  return rc;
}

static ssize_t lzread(void *cookie, char *buf, size_t len)
{
  LZFILE *lzfile = cookie;
  lzma_ret ret;
  int eof = 0;

  if (!lzfile || lzfile->encoding)
    return -1;
  if (lzfile->eof)
    return 0;
  lzfile->strm.next_out = (unsigned char *)buf;
  lzfile->strm.avail_out = len;
  for (;;)
    {
      if (!lzfile->strm.avail_in)
	{
	  lzfile->strm.next_in = lzfile->buf;
	  lzfile->strm.avail_in = fread(lzfile->buf, 1, sizeof(lzfile->buf), lzfile->file);
	  if (!lzfile->strm.avail_in)
	    eof = 1;
	}
      ret = lzma_code(&lzfile->strm, LZMA_RUN);
      if (ret == LZMA_STREAM_END)
	{
	  lzfile->eof = 1;
	  return len - lzfile->strm.avail_out;
	}
      if (ret != LZMA_OK)
	return -1;
      if (!lzfile->strm.avail_out)
	return len;
      if (eof)
	return -1;
    }
}

static ssize_t lzwrite(void *cookie, const char *buf, size_t len)
{
  LZFILE *lzfile = cookie;
  lzma_ret ret;
  size_t n;
  if (!lzfile || !lzfile->encoding)
    return -1;
  if (!len)
    return 0;
  lzfile->strm.next_in = (unsigned char *)buf;
  lzfile->strm.avail_in = len;
  for (;;)
    {
      lzfile->strm.next_out = lzfile->buf;
      lzfile->strm.avail_out = sizeof(lzfile->buf);
      ret = lzma_code(&lzfile->strm, LZMA_RUN);
      if (ret != LZMA_OK)
	return -1;
      n = sizeof(lzfile->buf) - lzfile->strm.avail_out;
      if (n && fwrite(lzfile->buf, 1, n, lzfile->file) != n)
	return -1;
      if (!lzfile->strm.avail_in)
	return len;
    }
}

static inline FILE *myxzfopen(const char *fn, const char *mode)
{
  LZFILE *lzf = lzopen(fn, mode, -1, 1);
  return cookieopen(lzf, mode, lzread, lzwrite, lzclose);
}

static inline FILE *myxzfdopen(int fd, const char *mode)
{
  LZFILE *lzf = lzopen(0, mode, fd, 1);
  return cookieopen(lzf, mode, lzread, lzwrite, lzclose);
}

static inline FILE *mylzfopen(const char *fn, const char *mode)
{
  LZFILE *lzf = lzopen(fn, mode, -1, 0);
  return cookieopen(lzf, mode, lzread, lzwrite, lzclose);
}

static inline FILE *mylzfdopen(int fd, const char *mode)
{
  LZFILE *lzf = lzopen(0, mode, fd, 0);
  return cookieopen(lzf, mode, lzread, lzwrite, lzclose);
}

#endif /* ENABLE_LZMA_COMPRESSION */

#ifdef ENABLE_ZSTD_COMPRESSION

#include <zstd.h>

typedef struct zstdfile {
  ZSTD_CStream *cstream;
  ZSTD_DStream *dstream;
  FILE *file;
  int encoding;
  int eof;
  ZSTD_inBuffer in;
  ZSTD_outBuffer out;
  unsigned char buf[1 << 15];
} ZSTDFILE;

static ZSTDFILE *zstdopen(const char *path, const char *mode, int fd)
{
  int level = 7;
  int encoding = 0;
  FILE *fp;
  ZSTDFILE *zstdfile;

  if ((!path && fd < 0) || (path && fd >= 0))
    return 0;
  for (; *mode; mode++)
    {
      if (*mode == 'w')
	encoding = 1;
      else if (*mode == 'r')
	encoding = 0;
      else if (*mode >= '1' && *mode <= '9')
	level = *mode - '0';
    }
  zstdfile = solv_calloc(1, sizeof(*zstdfile));
  zstdfile->encoding = encoding;
  if (encoding)
    {
      zstdfile->cstream = ZSTD_createCStream();
      zstdfile->encoding = 1;
      if (!zstdfile->cstream)
	{
	  solv_free(zstdfile);
	  return 0;
	}
      if (ZSTD_isError(ZSTD_initCStream(zstdfile->cstream, level)))
 	{
	  ZSTD_freeCStream(zstdfile->cstream);
	  solv_free(zstdfile);
	  return 0;
	}
      zstdfile->out.dst = zstdfile->buf;
      zstdfile->out.pos = 0;
      zstdfile->out.size = sizeof(zstdfile->buf);
    }
  else
    {
      zstdfile->dstream = ZSTD_createDStream();
      if (ZSTD_isError(ZSTD_initDStream(zstdfile->dstream)))
 	{
	  ZSTD_freeDStream(zstdfile->dstream);
	  solv_free(zstdfile);
	  return 0;
	}
      zstdfile->in.src = zstdfile->buf;
      zstdfile->in.pos = 0;
      zstdfile->in.size = 0;
    }
  if (!path)
    fp = fdopen(fd, encoding ? "w" : "r");
  else
    fp = fopen(path, encoding ? "w" : "r");
  if (!fp)
    {
      if (encoding)
	ZSTD_freeCStream(zstdfile->cstream);
      else
	ZSTD_freeDStream(zstdfile->dstream);
      solv_free(zstdfile);
      return 0;
    }
  zstdfile->file = fp;
  return zstdfile;
}

static int zstdclose(void *cookie)
{
  ZSTDFILE *zstdfile = cookie;
  int rc;

  if (!zstdfile)
    return -1;
  if (zstdfile->encoding)
    {
      for (;;)
	{
	  size_t ret;
	  zstdfile->out.pos = 0;
	  ret = ZSTD_endStream(zstdfile->cstream, &zstdfile->out);
	  if (ZSTD_isError(ret))
	    return -1;
	  if (zstdfile->out.pos && fwrite(zstdfile->buf, 1, zstdfile->out.pos, zstdfile->file) != zstdfile->out.pos)
	    return -1;
	  if (ret == 0)
	    break;
	}
      ZSTD_freeCStream(zstdfile->cstream);
    }
  else
    {
      ZSTD_freeDStream(zstdfile->dstream);
    }
  rc = fclose(zstdfile->file);
  solv_free(zstdfile);
  return rc;
}

static ssize_t zstdread(void *cookie, char *buf, size_t len)
{
  ZSTDFILE *zstdfile = cookie;
  int eof = 0;
  size_t ret = 0;
  if (!zstdfile || zstdfile->encoding)
    return -1;
  if (zstdfile->eof)
    return 0;
  zstdfile->out.dst = buf;
  zstdfile->out.pos = 0;
  zstdfile->out.size = len;
  for (;;)
    {
      if (!eof && zstdfile->in.pos == zstdfile->in.size)
	{
	  zstdfile->in.pos = 0;
	  zstdfile->in.size = fread(zstdfile->buf, 1, sizeof(zstdfile->buf), zstdfile->file);
	  if (!zstdfile->in.size)
	    eof = 1;
	}
      if (ret || !eof)
        ret = ZSTD_decompressStream(zstdfile->dstream, &zstdfile->out, &zstdfile->in);
      if (ret == 0 && eof)
	{
	  zstdfile->eof = 1;
	  return zstdfile->out.pos;
	}
      if (ZSTD_isError(ret))
	return -1;
      if (zstdfile->out.pos == len)
	return len;
    }
}

static ssize_t zstdwrite(void *cookie, const char *buf, size_t len)
{
  ZSTDFILE *zstdfile = cookie;
  if (!zstdfile || !zstdfile->encoding)
    return -1;
  if (!len)
    return 0;
  zstdfile->in.src = buf;
  zstdfile->in.pos = 0;
  zstdfile->in.size = len;

  for (;;)
    {
      size_t ret;
      zstdfile->out.pos = 0;
      ret = ZSTD_compressStream(zstdfile->cstream, &zstdfile->out, &zstdfile->in);
      if (ZSTD_isError(ret))
        return -1;
      if (zstdfile->out.pos && fwrite(zstdfile->buf, 1, zstdfile->out.pos, zstdfile->file) != zstdfile->out.pos)
        return -1;
      if (zstdfile->in.pos == len)
        return len;
    }
}

static inline FILE *myzstdfopen(const char *fn, const char *mode)
{
  ZSTDFILE *zstdfile = zstdopen(fn, mode, -1);
  return cookieopen(zstdfile, mode, zstdread, zstdwrite, zstdclose);
}

static inline FILE *myzstdfdopen(int fd, const char *mode)
{
  ZSTDFILE *zstdfile = zstdopen(0, mode, fd);
  return cookieopen(zstdfile, mode, zstdread, zstdwrite, zstdclose);
}

#endif

#ifdef ENABLE_ZCHUNK_COMPRESSION

#ifdef WITH_SYSTEM_ZCHUNK
/* use the system's zchunk library that supports reading and writing of zchunk files */

#include <zck.h>

static ssize_t cookie_zckread(void *cookie, char *buf, size_t nbytes)
{
  return zck_read((zckCtx *)cookie, buf, nbytes);
}

static ssize_t cookie_zckwrite(void *cookie, const char *buf, size_t nbytes)
{
  return zck_write((zckCtx *)cookie, buf, nbytes);
}

static int cookie_zckclose(void *cookie)
{
  zckCtx *zck = (zckCtx *)cookie;
  int fd = zck_get_fd(zck);
  if (fd != -1)
    close(fd);
  zck_free(&zck);
  return 0;
}

static void *zchunkopen(const char *path, const char *mode, int fd)
{
  zckCtx *f;

  if ((!path && fd < 0) || (path && fd >= 0))
    return 0;
  if (path)
    {
      if (*mode != 'w')
        fd = open(path, O_RDONLY);
      else
        fd = open(path, O_WRONLY | O_CREAT, 0666);
      if (fd == -1)
	return 0;
    }
  f = zck_create();
  if (!f)
    {
      if (path)
	close(fd);
      return 0;
    }
  if (*mode != 'w')
    {
      if(!zck_init_read(f, fd))
	{
	  zck_free(&f);
	  if (path)
	    close(fd);
	  return 0;
	}
    }
   else
    {
      if(!zck_init_write(f, fd))
	{
	  zck_free(&f);
	  if (path)
	    close(fd);
	  return 0;
	}
    }
  return cookieopen(f, mode, cookie_zckread, cookie_zckwrite, cookie_zckclose);
}

#else

#include "solv_zchunk.h"
/* use the libsolv's limited zchunk implementation that only supports reading of zchunk files */

static void *zchunkopen(const char *path, const char *mode, int fd)
{
  FILE *fp;
  void *f;
  if ((!path && fd < 0) || (path && fd >= 0))
    return 0;
  if (strcmp(mode, "r") != 0)
    return 0;
  if (!path)
    fp = fdopen(fd, mode);
  else
    fp = fopen(path, mode);
  if (!fp)
    return 0;
  f = solv_zchunk_open(fp, 1);
  if (!f)
    {
      if (!path)
	{
	  /* The fd passed by user must not be closed! */
	  /* Dup (save) the original fd to a temporary variable and then back. */
	  /* It is ugly and thread unsafe hack (non atomical sequence fclose dup2). */
	  int tmpfd = dup(fd);
	  fclose(fp);
	  dup2(tmpfd, fd);
	  close(tmpfd);
	}
      else
	{
	  fclose(fp);
	}
    }
  return cookieopen(f, mode, (ssize_t (*)(void *, char *, size_t))solv_zchunk_read, 0, (int (*)(void *))solv_zchunk_close);
}

#endif

static inline FILE *myzchunkfopen(const char *fn, const char *mode)
{
  return zchunkopen(fn, mode, -1);
}

static inline FILE *myzchunkfdopen(int fd, const char *mode)
{
  return zchunkopen(0, mode, fd);
}

#endif /* ENABLE_ZCHUNK_COMPRESSION */

#else
/* no cookies no compression */
#undef ENABLE_ZLIB_COMPRESSION
#undef ENABLE_LZMA_COMPRESSION
#undef ENABLE_BZIP2_COMPRESSION
#undef ENABLE_ZSTD_COMPRESSION
#undef ENABLE_ZCHUNK_COMPRESSION
#endif



FILE *
solv_xfopen(const char *fn, const char *mode)
{
  char *suf;

  if (!fn)
    return 0;
  if (!mode)
    mode = "r";
  suf = strrchr(fn, '.');
#ifdef ENABLE_ZLIB_COMPRESSION
  if (suf && !strcmp(suf, ".gz"))
    return mygzfopen(fn, mode);
#else
  if (suf && !strcmp(suf, ".gz"))
    return 0;
#endif
#ifdef ENABLE_LZMA_COMPRESSION
  if (suf && !strcmp(suf, ".xz"))
    return myxzfopen(fn, mode);
  if (suf && !strcmp(suf, ".lzma"))
    return mylzfopen(fn, mode);
#else
  if (suf && !strcmp(suf, ".xz"))
    return 0;
  if (suf && !strcmp(suf, ".lzma"))
    return 0;
#endif
#ifdef ENABLE_BZIP2_COMPRESSION
  if (suf && !strcmp(suf, ".bz2"))
    return mybzfopen(fn, mode);
#else
  if (suf && !strcmp(suf, ".bz2"))
    return 0;
#endif
#ifdef ENABLE_ZSTD_COMPRESSION
  if (suf && !strcmp(suf, ".zst"))
    return myzstdfopen(fn, mode);
#else
  if (suf && !strcmp(suf, ".zst"))
    return 0;
#endif
#ifdef ENABLE_ZCHUNK_COMPRESSION
  if (suf && !strcmp(suf, ".zck"))
    return myzchunkfopen(fn, mode);
#else
  if (suf && !strcmp(suf, ".zck"))
    return 0;
#endif
  return fopen(fn, mode);
}

FILE *
solv_xfopen_fd(const char *fn, int fd, const char *mode)
{
  const char *simplemode = mode;
  char *suf;

  suf = fn ? strrchr(fn, '.') : 0;
  if (!mode)
    {
      #ifndef _WIN32
      int fl = fcntl(fd, F_GETFL, 0);
      #else
      HANDLE handle = (HANDLE) _get_osfhandle(fd);
      BY_HANDLE_FILE_INFORMATION file_info;
      if (!GetFileInformationByHandle(handle, &file_info))
        return 0;
      int fl = file_info.dwFileAttributes;
      #endif
      if (fl == -1)
	return 0;
      fl &= O_RDONLY|O_WRONLY|O_RDWR;
      if (fl == O_WRONLY)
	mode = simplemode = "w";
      else if (fl == O_RDWR)
	{
	  mode = "r+";
	  simplemode = "r";
	}
      else
	mode = simplemode = "r";
    }
#ifdef ENABLE_ZLIB_COMPRESSION
  if (suf && !strcmp(suf, ".gz"))
    return mygzfdopen(fd, simplemode);
#else
  if (suf && !strcmp(suf, ".gz"))
    return 0;
#endif
#ifdef ENABLE_LZMA_COMPRESSION
  if (suf && !strcmp(suf, ".xz"))
    return myxzfdopen(fd, simplemode);
  if (suf && !strcmp(suf, ".lzma"))
    return mylzfdopen(fd, simplemode);
#else
  if (suf && !strcmp(suf, ".xz"))
    return 0;
  if (suf && !strcmp(suf, ".lzma"))
    return 0;
#endif
#ifdef ENABLE_BZIP2_COMPRESSION
  if (suf && !strcmp(suf, ".bz2"))
    return mybzfdopen(fd, simplemode);
#else
  if (suf && !strcmp(suf, ".bz2"))
    return 0;
#endif
#ifdef ENABLE_ZSTD_COMPRESSION
  if (suf && !strcmp(suf, ".zst"))
    return myzstdfdopen(fd, simplemode);
#else
  if (suf && !strcmp(suf, ".zst"))
    return 0;
#endif
#ifdef ENABLE_ZCHUNK_COMPRESSION
  if (suf && !strcmp(suf, ".zck"))
    return myzchunkfdopen(fd, simplemode);
#else
  if (suf && !strcmp(suf, ".zck"))
    return 0;
#endif
  return fdopen(fd, mode);
}

int
solv_xfopen_iscompressed(const char *fn)
{
  const char *suf = fn ? strrchr(fn, '.') : 0;
  if (!suf)
    return 0;
#ifdef ENABLE_ZLIB_COMPRESSION
  if (!strcmp(suf, ".gz"))
    return 1;
#else
    return -1;
#endif
  if (!strcmp(suf, ".xz") || !strcmp(suf, ".lzma"))
#ifdef ENABLE_LZMA_COMPRESSION
    return 1;
#else
    return -1;
#endif
  if (!strcmp(suf, ".bz2"))
#ifdef ENABLE_BZIP2_COMPRESSION
    return 1;
#else
    return -1;
#endif
  if (!strcmp(suf, ".zst"))
#ifdef ENABLE_ZSTD_COMPRESSION
    return 1;
#else
    return -1;
#endif
  if (!strcmp(suf, ".zck"))
#ifdef ENABLE_ZCHUNK_COMPRESSION
    return 1;
#else
    return -1;
#endif
  return 0;
}


#ifndef WITHOUT_COOKIEOPEN

struct bufcookie {
  char **bufp;
  size_t *buflp;
  char *freemem;
  size_t bufl_int;
  char *buf_int;
};

static ssize_t cookie_bufread(void *cookie, char *buf, size_t nbytes)
{
  struct bufcookie *bc = cookie;
  size_t n = *bc->buflp > nbytes ? nbytes : *bc->buflp;
  if (n)
    {
      memcpy(buf, *bc->bufp, n);
      *bc->bufp += n;
      *bc->buflp -= n;
    }
  return n;
}

static ssize_t cookie_bufwrite(void *cookie, const char *buf, size_t nbytes)
{
  struct bufcookie *bc = cookie;
  int n = nbytes > 0x40000000 ? 0x40000000 : nbytes;
  if (n)
    {
      *bc->bufp = solv_extend(*bc->bufp, *bc->buflp, n + 1, 1, 4095);
      memcpy(*bc->bufp, buf, n);
      (*bc->bufp)[n] = 0;	/* zero-terminate */
      *bc->buflp += n;
    }
  return n;
}

static int cookie_bufclose(void *cookie)
{
  struct bufcookie *bc = cookie;
  if (bc->freemem)
    solv_free(bc->freemem);
  solv_free(bc);
  return 0;
}

FILE *
solv_xfopen_buf(const char *fn, char **bufp, size_t *buflp, const char *mode)
{
  struct bufcookie *bc;
  FILE *fp;
  if (*mode != 'r' && *mode != 'w')
    return 0;
  bc = solv_calloc(1, sizeof(*bc));
  bc->freemem = 0;
  bc->bufp = bufp;
  if (!buflp)
    {
      bc->bufl_int = *mode == 'w' ? 0 : strlen(*bufp);
      buflp = &bc->bufl_int;
    }
  bc->buflp = buflp;
  if (*mode == 'w')
    {
      *bc->bufp = solv_extend(0, 0, 1, 1, 4095);	/* always zero-terminate */
      (*bc->bufp)[0] = 0;
      *bc->buflp = 0;
    }
  fp = cookieopen(bc, mode, cookie_bufread, cookie_bufwrite, cookie_bufclose);
  if (!strcmp(mode, "rf"))	/* auto-free */
    bc->freemem = *bufp;
  if (!fp)
    {
      if (*mode == 'w')
	*bc->bufp = solv_free(*bc->bufp);
      cookie_bufclose(bc);
    }
  return fp;
}

FILE *
solv_fmemopen(const char *buf, size_t bufl, const char *mode)
{
  struct bufcookie *bc;
  FILE *fp;
  if (*mode != 'r')
    return 0;
  bc = solv_calloc(1, sizeof(*bc));
  bc->buf_int = (char *)buf;
  bc->bufl_int = bufl;
  bc->bufp = &bc->buf_int;
  bc->buflp = &bc->bufl_int;
  fp = cookieopen(bc, mode, cookie_bufread, cookie_bufwrite, cookie_bufclose);
  if (!strcmp(mode, "rf"))	/* auto-free */
    bc->freemem = bc->buf_int;
  if (!fp)
    cookie_bufclose(bc);
  return fp;
}

#else

FILE *
solv_fmemopen(const char *buf, size_t bufl, const char *mode)
{
  FILE *fp;
  if (*mode != 'r')
    return 0;
  if (!strcmp(mode, "rf"))
    {
      if (!(fp = fmemopen(0, bufl, "r+")))
	return 0;
      if (bufl && fwrite(buf, bufl, 1, fp) != 1)
	{
	  fclose(fp);
	  return 0;
	}
      solv_free((char *)buf);
      rewind(fp);
    }
  else
    fp = fmemopen((char *)buf, bufl, "r");
  return fp;
}

#endif

