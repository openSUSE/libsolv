/*
 * Copyright (c) 2018, SUSE LLC.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <zstd.h>

#include "chksum.h"
#include "util.h"
#include "solv_zchunk.h"

#define MAX_HDR_SIZE  0xffffff00
#define MAX_CHUNK_CNT 0x0fffffff

#undef VERIFY_DATA_CHKSUM

struct solv_zchunk {
  FILE *fp;
  unsigned char *hdr;
  unsigned char *hdr_end;

  unsigned int flags;	/* header flags */
  unsigned int comp;	/* compression type */

  unsigned int hdr_chk_type;	/* header + data checksum */
  unsigned int hdr_chk_len;
  Id hdr_chk_id;

  unsigned int chunk_chk_type;	/* chunk checksum */
  unsigned int chunk_chk_len;
  Id chunk_chk_id;

  Chksum *data_chk;	/* for data checksum verification */
  unsigned char *data_chk_ptr;

  unsigned int streamid;	/* stream we are reading */
  unsigned int nchunks;		/* chunks left */
  unsigned char *chunks;

  ZSTD_DCtx *dctx;
  ZSTD_DDict *ddict;

  int eof;
  unsigned char *buf;
  unsigned int buf_used;
  unsigned int buf_avail;
};

/* return 32bit compressed integer. returns NULL on overflow. */
static unsigned char *
getuint(unsigned char *p, unsigned char *endp, unsigned int *dp)
{
  if (!p || p >= endp)
    return 0;
  if (p < endp && (*p & 0x80) != 0)
    {
      *dp = p[0] ^ 0x80;
      return p + 1;
    }
  if (++p < endp && (*p & 0x80) != 0)
    {
      *dp = p[-1] ^ ((p[0] ^ 0x80) << 7);
      return p + 1;
    }
  if (++p < endp && (*p & 0x80) != 0)
    {
      *dp = p[-2] ^ (p[-1] << 7) ^ ((p[0] ^ 0x80) << 14);
      return p + 1;
    }
  if (++p < endp && (*p & 0x80) != 0)
    {
      *dp = p[-3] ^ (p[-2] << 7) ^ (p[-1] << 14) ^ ((p[0] ^ 0x80) << 21);
      return p + 1;
    }
  if (++p < endp && (*p & 0xf0) == 0x80)
    {
      *dp = p[-4] ^ (p[-3] << 7) ^ (p[-2] << 14) ^ (p[-1] << 21) ^ ((p[0] ^ 0x80) << 28);
      return p + 1;
    }
  return 0;
}

static unsigned char *
getchksum(unsigned char *p, unsigned char *endp, unsigned int *typep, unsigned int *lenp, Id *idp)
{
  if ((p = getuint(p, endp, typep)) == 0)
    return 0;
  switch (*typep)
    {
    case 0:
      *lenp = 20;
      *idp = REPOKEY_TYPE_SHA1;
      return p;
    case 1:
      *lenp = 32;
      *idp = REPOKEY_TYPE_SHA256;
      return p;
    case 2:
      *lenp = 64;
      *idp = REPOKEY_TYPE_SHA512;
      return p;
    case 3:
      *lenp = 16;
      *idp = REPOKEY_TYPE_SHA512;
      return p;
    default:
      break;
    }
  return 0;
}

static int
skip_bytes(FILE *fp, size_t skip, Chksum *chk)
{
  unsigned char buf[4096];
  while (skip)
    {
      size_t bite = skip > sizeof(buf) ? sizeof(buf) : skip;
      if (fread(buf, bite, 1, fp) != 1)
	return 0;
      if (chk)
	solv_chksum_add(chk, buf, bite);
      skip -= bite;
    }
  return 1;
}

static int
nextchunk(struct solv_zchunk *zck, unsigned int streamid)
{
  unsigned char *p = zck->chunks;
  unsigned char *chunk_chk_ptr;
  unsigned int sid, chunk_len, uncompressed_len;
  unsigned char *cbuf;

  /* free old buffer */
  zck->buf = solv_free(zck->buf);
  zck->buf_avail = 0;
  zck->buf_used = 0;

  for (;;)
    {
      if (zck->nchunks == 0)
	{
	  zck->chunks = p;
	  return 1;		/* EOF reached */
	}
      if (p >= zck->hdr_end)
	return 0;
      sid = streamid ? 1 : 0;
      /* check if this is the correct stream */
      if ((zck->flags & 1) != 0 && (p = getuint(p, zck->hdr_end, &sid)) == 0)
	return 0;
      chunk_chk_ptr = p;	/* remember for verification */
      p += zck->chunk_chk_len;
      if (p >= zck->hdr_end)
	return 0;
      if ((p = getuint(p, zck->hdr_end, &chunk_len)) == 0)
	return 0;
      if ((p = getuint(p, zck->hdr_end, &uncompressed_len)) == 0)
	return 0;
      zck->nchunks--;
      if (sid == streamid)
	break;
      /* skip the chunk, but the dict chunk must come first */
      if (streamid == 0 || skip_bytes(zck->fp, chunk_len, zck->data_chk) == 0)
	return 0;
    }
  zck->chunks = p;

  /* ok, read the compressed chunk */
  if (!chunk_len)
    return uncompressed_len ? 0 : 1;
  cbuf = solv_malloc(chunk_len);
  if (fread(cbuf, chunk_len, 1, zck->fp) != 1)
    {
      solv_free(cbuf);
      return 0;
    }
  if (zck->data_chk)
    solv_chksum_add(zck->data_chk, cbuf, chunk_len);

  /* verify the chunk checksum */
  if (zck->chunk_chk_id)
    {
      Chksum *chk = solv_chksum_create(zck->chunk_chk_id);
      if (!chk)
	{
	  solv_free(cbuf);
	  return 0;
	}
      solv_chksum_add(chk, cbuf, chunk_len);
      if (memcmp(solv_chksum_get(chk, 0), chunk_chk_ptr, zck->chunk_chk_len) != 0)
	{
	  solv_chksum_free(chk, 0);
	  solv_free(cbuf);
	  return 0;
	}
      solv_chksum_free(chk, 0);
    }

  /* uncompress */
  if (zck->comp == 0)
    {
      /* not compressed */
      if (chunk_len != uncompressed_len)
	{
	  solv_free(cbuf);
	  return 0;
	}
      zck->buf = cbuf;
      zck->buf_avail = uncompressed_len;
      return 1;
    }
  if (zck->comp == 2)
    {
      /* zstd compressed */
      size_t r;
      zck->buf = solv_malloc(uncompressed_len + 1);	/* +1 so we can detect too large frames */
      if (zck->ddict)
	r = ZSTD_decompress_usingDDict(zck->dctx, zck->buf, uncompressed_len + 1, cbuf, chunk_len, zck->ddict);
      else
	r = ZSTD_decompressDCtx(zck->dctx, zck->buf, uncompressed_len + 1, cbuf, chunk_len);
      solv_free(cbuf);
      if (r != uncompressed_len)
	return 0;
      zck->buf_avail = uncompressed_len;
      return 1;
    }
  solv_free(cbuf);
  return 0;
}

static inline struct solv_zchunk *
open_error(struct solv_zchunk *zck)
{
  solv_zchunk_close(zck);
  return 0;
}

struct solv_zchunk *
solv_zchunk_open(FILE *fp, unsigned int streamid)
{
  struct solv_zchunk *zck;
  unsigned char *p;
  unsigned int hdr_size;	/* preface + index + signatures */
  unsigned int lead_size;
  unsigned int preface_size;
  unsigned int index_size;

  zck = solv_calloc(1, sizeof(*zck));

  /* read and parse the lead, read the complete header */
  zck->hdr = solv_calloc(15, 1);
  zck->hdr_end = zck->hdr + 15;
  if (fread(zck->hdr, 15, 1, fp) != 1 || memcmp(zck->hdr, "\000ZCK1", 5) != 0)
    return open_error(zck);
  p = zck->hdr + 5;
  if ((p = getchksum(p, zck->hdr_end, &zck->hdr_chk_type, &zck->hdr_chk_len, &zck->hdr_chk_id)) == 0)
    return open_error(zck);
  if ((p = getuint(p, zck->hdr_end, &hdr_size)) == 0 || hdr_size > MAX_HDR_SIZE)
    return open_error(zck);
  lead_size = p - zck->hdr + zck->hdr_chk_len;
  zck->hdr = solv_realloc(zck->hdr, lead_size + hdr_size);
  zck->hdr_end = zck->hdr + lead_size + hdr_size;
  if (fread(zck->hdr + 15, lead_size + hdr_size - 15, 1, fp) != 1)
    return open_error(zck);

  /* verify header checksum to guard against corrupt files */
  if (zck->hdr_chk_id)
    {
      Chksum *chk = solv_chksum_create(zck->hdr_chk_id);
      if (!chk)
	return open_error(zck);
      solv_chksum_add(chk, zck->hdr, lead_size - zck->hdr_chk_len);
      solv_chksum_add(chk, zck->hdr + lead_size, hdr_size);
      if (memcmp(solv_chksum_get(chk, 0), zck->hdr + (lead_size - zck->hdr_chk_len), zck->hdr_chk_len) != 0)
	{
	  solv_chksum_free(chk, 0);
	  return open_error(zck);
	}
      solv_chksum_free(chk, 0);
    }

  /* parse preface: data chksum, flags, compression */
  p = zck->hdr + lead_size;
  if (p + zck->hdr_chk_len > zck->hdr_end)
    return open_error(zck);
  zck->data_chk_ptr = p;
  p += zck->hdr_chk_len;
#ifdef VERIFY_DATA_CHKSUM
  if (zck->hdr_chk_id && (zck->data_chk = solv_chksum_create(zck->hdr_chk_id)) == 0)
    return open_error(zck);
#endif
  if ((p = getuint(p, zck->hdr_end, &zck->flags)) == 0)
    return open_error(zck);
  if ((zck->flags & ~(3)) != 0)
    return open_error(zck);
  if ((p = getuint(p, zck->hdr_end, &zck->comp)) == 0 || (zck->comp != 0 && zck->comp != 2))
    return open_error(zck);	/* only uncompressed + zstd supported */
  /* skip all optional elements if present */
  if ((zck->flags & 2) != 0)
    {
      unsigned int nopt, lopt;
      if ((p = getuint(p, zck->hdr_end, &nopt)) == 0)
        return open_error(zck);
      for (; nopt != 0; nopt--)
	{
	  if ((p = getuint(p, zck->hdr_end, &lopt)) == 0)
            return open_error(zck);
	  if ((p = getuint(p, zck->hdr_end, &lopt)) == 0)
            return open_error(zck);
	  if (p + lopt > zck->hdr_end)
	    return open_error(zck);
	  p += lopt;
	}
    }

  preface_size = p - (zck->hdr + lead_size);

  /* parse index: index size, index chksum type, num chunks, chunk data  */
  if ((p = getuint(p, zck->hdr_end, &index_size)) == 0)
    return open_error(zck);
  if (hdr_size < preface_size + index_size)
    return open_error(zck);
  if ((p = getchksum(p, zck->hdr_end, &zck->chunk_chk_type, &zck->chunk_chk_len, &zck->chunk_chk_id)) == 0)
    return open_error(zck);
  if ((p = getuint(p, zck->hdr_end, &zck->nchunks)) == 0 || zck->nchunks > MAX_CHUNK_CNT)
    return open_error(zck);

  /* setup decompressor */
  if (zck->comp == 2)
    {
      if ((zck->dctx = ZSTD_createDCtx()) == 0)
	return open_error(zck);
    }

  zck->fp = fp;
  zck->chunks = p;
  zck->streamid = streamid;
  if (streamid == 0)
    {
      zck->nchunks = zck->nchunks ? 1 : 0;	/* limit to dict chunk */
      return zck;	
    }

  /* setup dictionary */
  if (!nextchunk(zck, 0))
    {
      zck->fp = 0;
      return open_error(zck);
    }
  if (zck->comp == 2 && zck->buf_avail)
    {
      if ((zck->ddict = ZSTD_createDDict(zck->buf, zck->buf_avail)) == 0)
	{
	  zck->fp = 0;
	  return open_error(zck);
	}
    }
  zck->buf = solv_free(zck->buf);
  zck->buf_used = 0;
  zck->buf_avail = 0;

  /* ready to read the rest of the chunks */
  return zck;
}

ssize_t
solv_zchunk_read(struct solv_zchunk *zck, char *buf, size_t len)
{
  size_t n = 0;
  if (!zck || zck->eof == 2)
    return -1;
  while (n < len && !zck->eof)
    {
      unsigned int bite;
      while (!zck->buf_avail)
	{
	  if (!zck->nchunks)
	    {
	      /* verify data checksum if requested */
	      if (zck->streamid != 0 && zck->data_chk && memcmp(solv_chksum_get(zck->data_chk, 0), zck->data_chk_ptr, zck->hdr_chk_len) != 0) {
	        zck->eof = 2;
	        return -1;
	      }
	      zck->eof = 1;
	      return n;
	    }
	  if (!nextchunk(zck, zck->streamid))
	    {
	      zck->eof = 2;
	      return -1;
	    }
	}
      bite = len - n > zck->buf_avail ? zck->buf_avail : len - n;
      memcpy(buf + n, zck->buf + zck->buf_used, bite);
      n += bite;
      zck->buf_used += bite;
      zck->buf_avail -= bite;
    }
  return n;
}

int
solv_zchunk_close(struct solv_zchunk *zck)
{
  if (zck->data_chk)
    solv_chksum_free(zck->data_chk, 0);
  if (zck->ddict)
    ZSTD_freeDDict(zck->ddict);
  if (zck->dctx)
    ZSTD_freeDCtx(zck->dctx);
  solv_free(zck->hdr);
  solv_free(zck->buf);
  if (zck->fp)
    fclose(zck->fp);
  solv_free(zck);
  return 0;
}
