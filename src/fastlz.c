/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#define BLOCK_SIZE (65536*1)
#if BLOCK_SIZE <= 65536
typedef __uint16_t Ref;
#else
typedef __uint32_t Ref;
#endif

/*
   The format is tailored for fast decompression (i.e. only byte based),
   and skewed to ASCII content (highest bit often not set):
   
   a 0LLLLLLL
        - self-describing ASCII character hex L
   b 100lllll <l+1 bytes>
        - literal run of length l+1
   c 101oolll <8o>
        - back ref of length l+2, at offset -(o+1) (o < 1 << 10)
   d 110lllll <8o>
        - back ref of length l+2+8, at offset -(o+1) (o < 1 << 8)
   e 1110llll <8o> <8o>
        - back ref of length l+3, at offset -(o+1) (o < 1 << 16)
  f1 1111llll <8l> <8o> <8o>
        - back ref, length l+19 (l < 1<<12), offset -(o+1) (o < 1<<16)
  f2 11110lll <8l> <8o> <8o>
        - back ref, length l+19 (l < 1<<11), offset -(o+1) (o < 1<<16)
   g 11111lll <8l> <8o> <8o> <8o>
        - back ref, length l+5 (l < 1<<11), offset -(o+1) (o < 1<<24)

   Generally for a literal of length L we need L+1 bytes, hence it is
   better to encode also very short backrefs (2 chars) as backrefs if
   their offset is small, as that only needs two bytes.  Except if we
   already have a literal run, in that case it's better to append there,
   instead of breaking it for a backref.  So given a potential backref
   at offset O, length L the strategy is as follows:

   L < 2 : encode as 1-literal
   L == 2, O > 1024 : encode as 1-literal
   L == 2, have already literals: encode as 1-literal
   O = O - 1
   L >= 2, L <= 9, O < 1024                            : encode as c
   L >= 10, L <= 41, O < 256                           : encode as d
   else we have either O >= 1024, or L >= 42:
   L < 3 : encode as 1-literal
   L >= 3, L <= 18, O < 65536                          : encode as e
   L >= 19, L <= 4095+18, O < 65536                    : encode as f
   else we have either L >= 4096+18 or O >= 65536.
   O >= 65536: encode as 1-literal, too bad
     (with the current block size this can't happen)
   L >= 4096+18, so reduce to 4095+18                  : encode as f
*/


unsigned int
compress_buf (const unsigned char *in, unsigned int in_len,
	      unsigned char *out, unsigned int out_len)
{
  unsigned int oo = 0;		//out-offset
  unsigned int io = 0;		//in-offset
#define HS (65536)
  Ref htab[HS];
  Ref hnext[BLOCK_SIZE];
  memset (htab, -1, sizeof (htab));
  memset (hnext, -1, sizeof (hnext));
  unsigned int litofs = 0;
  while (io + 2 < in_len)
    {
      /* Search for a match of the string starting at IN, we have at
         least three characters.  */
      unsigned int hval = in[io] | in[io + 1] << 8 | in[io + 2] << 16;
      unsigned int try, mlen, mofs, tries;
      hval = (hval ^ (hval << 5) ^ (hval >> 5)) - hval * 5;
      hval = hval & (HS - 1);
      try = htab[hval];
      hnext[io] = htab[hval];
      htab[hval] = io;
      mlen = 0;
      mofs = 0;

      for (tries = 0; try != -1 && tries < 12; tries++)
        {
	  if (try < io
	      && in[try] == in[io] && in[try + 1] == in[io + 1])
	    {
	      mlen = 2;
	      mofs = (io - try) - 1;
	      break;
	    }
	  try = hnext[try];
	}
      for (; try != -1 && tries < 12; tries++)
	{
	  //assert (mlen >= 2);
	  //assert (io + mlen < in_len);
	  /* Try a match starting from [io] with the strings at [try].
	     That's only sensible if TRY actually is before IO (can happen
	     with uninit hash table).  If we have a previous match already
	     we're only going to take the new one if it's longer, hence
	     check the potentially last character.  */
	  if (try < io && in[try + mlen] == in[io + mlen])
	    {
	      unsigned int this_len, this_ofs;
	      if (memcmp (in + try, in + io, mlen))
		goto no_match;
	      this_len = mlen + 1;
	      /* Now try extending the match by more characters.  */
	      for (;
		   io + this_len < in_len
		   && in[try + this_len] == in[io + this_len]; this_len++)
		;
#if 0
	      unsigned int testi;
	      for (testi = 0; testi < this_len; testi++)
		assert (in[try + testi] == in[io + testi]);
#endif
	      this_ofs = (io - try) - 1;
	      /*if (this_ofs > 65535)
		 goto no_match; */
#if 0
	      assert (this_len >= 2);
	      assert (this_len >= mlen);
	      assert (this_len > mlen || (this_len == mlen && this_ofs > mofs));
#endif
	      mlen = this_len, mofs = this_ofs;
	      /* If our match extends up to the end of input, no next
		 match can become better.  This is not just an
		 optimization, it establishes a loop invariant
		 (io + mlen < in_len).  */
	      if (io + mlen >= in_len)
		goto match_done;
	    }
	no_match:
	  try = hnext[try];
	  /*if (io - try - 1 >= 65536)
	    break;*/
	}

match_done:
      if (mlen)
	{
	  //fprintf (stderr, "%d %d\n", mlen, mofs);
	  if (mlen == 2 && (litofs || mofs >= 1024))
	    mlen = 0;
	  /*else if (mofs >= 65536)
	    mlen = 0;*/
	  else if (mofs >= 65536)
	    {
	      if (mlen >= 2048 + 5)
	        mlen = 2047 + 5;
	      else if (mlen < 5)
	        mlen = 0;
	    }
	  else if (mlen < 3)
	    mlen = 0;
	  /*else if (mlen >= 4096 + 19)
	    mlen = 4095 + 19;*/
	  else if (mlen >= 2048 + 19)
	    mlen = 2047 + 19;
	  /* Skip this match if the next character would deliver a better one,
	     but only do this if we have the chance to really extend the
	     length (i.e. our current length isn't yet the (conservative)
	     maximum).  */
	  if (mlen && mlen < (2048 + 5) && io + 3 < in_len)
	    {
	      unsigned int hval =
		in[io + 1] | in[io + 2] << 8 | in[io + 3] << 16;
	      unsigned int try;
	      hval = (hval ^ (hval << 5) ^ (hval >> 5)) - hval * 5;
	      hval = hval & (HS - 1);
	      try = htab[hval];
	      if (try < io + 1
		  && in[try] == in[io + 1] && in[try + 1] == in[io + 2])
		{
		  unsigned int this_len;
		  this_len = 2;
		  for (;
		       io + 1 + this_len < in_len
		       && in[try + this_len] == in[io + 1 + this_len];
		       this_len++)
		    ;
		  if (this_len >= mlen)
		    mlen = 0;
		}
	    }
	}
      if (!mlen)
	{
	  if (!litofs)
	    litofs = io + 1;
	  io++;
	}
      else
	{
	  if (litofs)
	    {
	      litofs--;
	      unsigned litlen = io - litofs;
	      //fprintf (stderr, "lit: %d\n", litlen);
	      while (litlen)
		{
		  unsigned int easy_sz;
		  /* Emit everything we can as self-describers.  As soon as
		     we hit a byte we can't emit as such we're going to emit
		     a length descriptor anyway, so we can as well include
		     bytes < 0x80 which might follow afterwards in that run.  */
		  for (easy_sz = 0;
		       easy_sz < litlen && in[litofs + easy_sz] < 0x80;
		       easy_sz++)
		    ;
		  if (easy_sz)
		    {
		      if (oo + easy_sz >= out_len)
			return 0;
		      memcpy (out + oo, in + litofs, easy_sz);
		      litofs += easy_sz;
		      oo += easy_sz;
		      litlen -= easy_sz;
		      if (!litlen)
			break;
		    }
		  if (litlen <= 32)
		    {
		      if (oo + 1 + litlen >= out_len)
			return 0;
		      out[oo++] = 0x80 | (litlen - 1);
		      while (litlen--)
			out[oo++] = in[litofs++];
		      break;
		    }
		  else
		    {
		      /* Literal length > 32, so chunk it.  */
		      if (oo + 1 + 32 >= out_len)
			return 0;
		      out[oo++] = 0x80 | 31;
		      memcpy (out + oo, in + litofs, 32);
		      oo += 32;
		      litofs += 32;
		      litlen -= 32;
		    }
		}
	      litofs = 0;
	    }

	  //fprintf (stderr, "ref: %d @ %d\n", mlen, mofs);

	  if (mlen >= 2 && mlen <= 9 && mofs < 1024)
	    {
	      if (oo + 2 >= out_len)
		return 0;
	      out[oo++] = 0xa0 | ((mofs & 0x300) >> 5) | (mlen - 2);
	      out[oo++] = mofs & 0xff;
	    }
	  else if (mlen >= 10 && mlen <= 41 && mofs < 256)
	    {
	      if (oo + 2 >= out_len)
		return 0;
	      out[oo++] = 0xc0 | (mlen - 10);
	      out[oo++] = mofs;
	    }
	  else if (mofs >= 65536)
	    {
	      assert (mlen >= 5 && mlen < 2048 + 5);
	      if (oo + 5 >= out_len)
	        return 0;
	      out[oo++] = 0xf8 | ((mlen - 5) >> 8);
	      out[oo++] = (mlen - 5) & 0xff;
	      out[oo++] = mofs & 0xff;
	      out[oo++] = (mofs >> 8) & 0xff;
	      out[oo++] = mofs >> 16;
	    }
	  else if (mlen >= 3 && mlen <= 18)
	    {
	      assert (mofs < 65536);
	      if (oo + 3 >= out_len)
		return 0;
	      out[oo++] = 0xe0 | (mlen - 3);
	      out[oo++] = mofs & 0xff;
	      out[oo++] = mofs >> 8;
	    }
	  else
	    {
	      assert (mlen >= 19 && mlen <= 4095 + 19 && mofs < 65536);
	      if (oo + 4 >= out_len)
		return 0;
	      out[oo++] = 0xf0 | ((mlen - 19) >> 8);
	      out[oo++] = (mlen - 19) & 0xff;
	      out[oo++] = mofs & 0xff;
	      out[oo++] = mofs >> 8;
	    }
	  /* Insert the hashes for the compressed run [io..io+mlen-1].
	     For [io] we have it already done at the start of the loop.
	     So it's from [io+1..io+mlen-1], and we need three chars per
	     hash, so the accessed characters will be [io+1..io+mlen-1+2],
	     ergo io+mlen+1 < in_len.  */
	  mlen--;
	  io++;
	  while (mlen--)
	    {
	      if (io + 2 < in_len)
		{
		  unsigned int hval =
		    in[io] | in[io + 1] << 8 | in[io + 2] << 16;
		  hval = (hval ^ (hval << 5) ^ (hval >> 5)) - hval * 5;
		  hval = hval & (HS - 1);
		  hnext[io] = htab[hval];
		  htab[hval] = io;
		}
	      io++;
	    };
	}
    }
  /* We might have some characters left.  */
  if (io < in_len && !litofs)
    litofs = io + 1;
  io = in_len;
  if (litofs)
    {
      litofs--;
      unsigned litlen = io - litofs;
      //fprintf (stderr, "lit: %d\n", litlen);
      while (litlen)
	{
	  unsigned int easy_sz;
	  /* Emit everything we can as self-describers.  As soon as we hit a
	     byte we can't emit as such we're going to emit a length
	     descriptor anyway, so we can as well include bytes < 0x80 which
	     might follow afterwards in that run.  */
	  for (easy_sz = 0; easy_sz < litlen && in[litofs + easy_sz] < 0x80;
	       easy_sz++)
	    ;
	  if (easy_sz)
	    {
	      if (oo + easy_sz >= out_len)
		return 0;
	      memcpy (out + oo, in + litofs, easy_sz);
	      litofs += easy_sz;
	      oo += easy_sz;
	      litlen -= easy_sz;
	      if (!litlen)
		break;
	    }
	  if (litlen <= 32)
	    {
	      if (oo + 1 + litlen >= out_len)
		return 0;
	      out[oo++] = 0x80 | (litlen - 1);
	      while (litlen--)
		out[oo++] = in[litofs++];
	      break;
	    }
	  else
	    {
	      /* Literal length > 32, so chunk it.  */
	      if (oo + 1 + 32 >= out_len)
		return 0;
	      out[oo++] = 0x80 | 31;
	      memcpy (out + oo, in + litofs, 32);
	      oo += 32;
	      litofs += 32;
	      litlen -= 32;
	    }
	}
      litofs = 0;
    }
  return oo;
}

unsigned int
unchecked_decompress_buf (const unsigned char *in, unsigned int in_len,
			  unsigned char *out,
			  unsigned int out_len __attribute__((unused)))
{
  unsigned char *orig_out = out;
  const unsigned char *in_end = in + in_len;
  while (in < in_end)
    {
      unsigned int first = *in++;
      int o;
      switch (first >> 4)
	{
	default:
	  /* This default case can't happen, but GCCs VRP is not strong
	     enough to see this, so make this explicitely not fall to
	     the end of the switch, so that we don't have to initialize
	     o above.  */
	  continue;
	case 0: case 1:
	case 2: case 3:
	case 4: case 5:
	case 6: case 7:
	  //a 0LLLLLLL
	  //fprintf (stderr, "lit: 1\n");
	  *out++ = first;
	  continue;
	case 8: case 9:
	  //b 100lllll <l+1 bytes>
	  {
	    unsigned int l = first & 31;
	    //fprintf (stderr, "lit: %d\n", l);
	    do
	      *out++ = *in++;
	    while (l--);
	    continue;
	  }
	case 10: case 11:
	  //c 101oolll <8o>
	  {
	    o = first & (3 << 3);
	    o = (o << 5) | *in++;
	    first = (first & 7) + 2;
	    break;
	  }
	case 12: case 13:
	  //d 110lllll <8o>
	  {
	    o = *in++;
	    first = (first & 31) + 10;
	    break;
	  }
	case 14:
	  // e 1110llll <8o> <8o>
	  {
	    o = in[0] | (in[1] << 8);
	    in += 2;
	    first = first & 31;
	    first += 3;
	    break;
	  }
	case 15:
	  //f1 1111llll <8o> <8o> <8l>
	  //f2 11110lll <8o> <8o> <8l>
	  // g 11111lll <8o> <8o> <8o> <8l>
	  {
	    first = first & 15;
	    if (first >= 8)
	      {
		first = (((first - 8) << 8) | in[0]) + 5;
		o = in[1] | (in[2] << 8) | (in[3] << 16);
		in += 4;
	      }
	    else
	      {
	        first = ((first << 8) | in[0]) + 19;
		o = in[1] | (in[2] << 8);
		in += 3;
	      }
	    break;
	  }
	}
      //fprintf (stderr, "ref: %d @ %d\n", first, o);
      o++;
      o = -o;
#if 0
      /* We know that first will not be zero, and this loop structure is
         better optimizable.  */
      do
	{
	  *out = *(out - o);
	  out++;
	}
      while (--first);
#else
      switch (first)
        {
	  case 18: *out = *(out + o); out++;
	  case 17: *out = *(out + o); out++;
	  case 16: *out = *(out + o); out++;
	  case 15: *out = *(out + o); out++;
	  case 14: *out = *(out + o); out++;
	  case 13: *out = *(out + o); out++;
	  case 12: *out = *(out + o); out++;
	  case 11: *out = *(out + o); out++;
	  case 10: *out = *(out + o); out++;
	  case  9: *out = *(out + o); out++;
	  case  8: *out = *(out + o); out++;
	  case  7: *out = *(out + o); out++;
	  case  6: *out = *(out + o); out++;
	  case  5: *out = *(out + o); out++;
	  case  4: *out = *(out + o); out++;
	  case  3: *out = *(out + o); out++;
	  case  2: *out = *(out + o); out++;
	  case  1: *out = *(out + o); out++;
	  case  0: break;
	  default:
	    /* Duff duff :-) */
	    switch (first & 15)
	      {
		do
		  {
		    case  0: *out = *(out + o); out++;
		    case 15: *out = *(out + o); out++;
		    case 14: *out = *(out + o); out++;
		    case 13: *out = *(out + o); out++;
		    case 12: *out = *(out + o); out++;
		    case 11: *out = *(out + o); out++;
		    case 10: *out = *(out + o); out++;
		    case  9: *out = *(out + o); out++;
		    case  8: *out = *(out + o); out++;
		    case  7: *out = *(out + o); out++;
		    case  6: *out = *(out + o); out++;
		    case  5: *out = *(out + o); out++;
		    case  4: *out = *(out + o); out++;
		    case  3: *out = *(out + o); out++;
		    case  2: *out = *(out + o); out++;
		    case  1: *out = *(out + o); out++;
		  }
		while ((int)(first -= 16) > 0);
	      }
	    break;
	}
#endif
    }
  return out - orig_out;
}

#ifdef STANDALONE

static void
transfer_file (FILE * from, FILE * to, int compress)
{
  unsigned char inb[BLOCK_SIZE];
  unsigned char outb[BLOCK_SIZE];
  while (!feof (from) && !ferror (from))
    {
      unsigned int in_len, out_len;
      if (compress)
	{
	  in_len = fread (inb, 1, BLOCK_SIZE, from);
	  if (in_len)
	    {
	      unsigned char *b = outb;
	      out_len = compress_buf (inb, in_len, outb, sizeof (outb));
	      if (!out_len)
		b = inb, out_len = in_len;
	      if (fwrite (&out_len, sizeof (out_len), 1, to) != 1)
		{
		  perror ("write size");
		  exit (1);
		}
	      if (fwrite (b, out_len, 1, to) != 1)
		{
		  perror ("write data");
		  exit (1);
		}
	    }
	}
      else
	{
	  if (fread (&in_len, sizeof (in_len), 1, from) != 1)
	    {
	      if (feof (from))
		return;
	      perror ("can't read size");
	      exit (1);
	    }
	  if (fread (inb, in_len, 1, from) != 1)
	    {
	      perror ("can't read data");
	      exit (1);
	    }
	  out_len =
	    unchecked_decompress_buf (inb, in_len, outb, sizeof (outb));
	  if (fwrite (outb, out_len, 1, to) != 1)
	    {
	      perror ("can't write output");
	      exit (1);
	    }
	}
    }
}

/* Just for benchmarking purposes.  */
static void
dumb_memcpy (void *dest, const void *src, unsigned int len)
{
  char *d = dest;
  const char *s = src;
  while (len--)
    *d++ = *s++;
}

static void
benchmark (FILE * from)
{
  unsigned char inb[BLOCK_SIZE];
  unsigned char outb[BLOCK_SIZE];
  unsigned int in_len = fread (inb, 1, BLOCK_SIZE, from);
  unsigned int out_len;
  if (!in_len)
    {
      perror ("can't read from input");
      exit (1);
    }

  unsigned int calib_loop;
  unsigned int per_loop;
  unsigned int i, j;
  clock_t start, end;
  float seconds;

#if 0
  calib_loop = 1;
  per_loop = 0;
  start = clock ();
  while ((clock () - start) < CLOCKS_PER_SEC / 4)
    {
      calib_loop *= 2;
      for (i = 0; i < calib_loop; i++)
	dumb_memcpy (outb, inb, in_len);
      per_loop += calib_loop;
    }

  fprintf (stderr, "memcpy:\nCalibrated to %d iterations per loop\n",
	   per_loop);

  start = clock ();
  for (i = 0; i < 10; i++)
    for (j = 0; j < per_loop; j++)
      dumb_memcpy (outb, inb, in_len);
  end = clock ();
  seconds = (end - start) / (float) CLOCKS_PER_SEC;
  fprintf (stderr, "%.2f seconds == %.2f MB/s\n", seconds,
	   ((long long) in_len * per_loop * 10) / (1024 * 1024 * seconds));
#endif

  calib_loop = 1;
  per_loop = 0;
  start = clock ();
  while ((clock () - start) < CLOCKS_PER_SEC / 4)
    {
      calib_loop *= 2;
      for (i = 0; i < calib_loop; i++)
	compress_buf (inb, in_len, outb, sizeof (outb));
      per_loop += calib_loop;
    }

  fprintf (stderr, "compression:\nCalibrated to %d iterations per loop\n",
	   per_loop);

  start = clock ();
  for (i = 0; i < 10; i++)
    for (j = 0; j < per_loop; j++)
      compress_buf (inb, in_len, outb, sizeof (outb));
  end = clock ();
  seconds = (end - start) / (float) CLOCKS_PER_SEC;
  fprintf (stderr, "%.2f seconds == %.2f MB/s\n", seconds,
	   ((long long) in_len * per_loop * 10) / (1024 * 1024 * seconds));

  out_len = compress_buf (inb, in_len, outb, sizeof (outb));

  calib_loop = 1;
  per_loop = 0;
  start = clock ();
  while ((clock () - start) < CLOCKS_PER_SEC / 4)
    {
      calib_loop *= 2;
      for (i = 0; i < calib_loop; i++)
	unchecked_decompress_buf (outb, out_len, inb, sizeof (inb));
      per_loop += calib_loop;
    }

  fprintf (stderr, "decompression:\nCalibrated to %d iterations per loop\n",
	   per_loop);

  start = clock ();
  for (i = 0; i < 10; i++)
    for (j = 0; j < per_loop; j++)
      unchecked_decompress_buf (outb, out_len, inb, sizeof (inb));
  end = clock ();
  seconds = (end - start) / (float) CLOCKS_PER_SEC;
  fprintf (stderr, "%.2f seconds == %.2f MB/s\n", seconds,
	   ((long long) in_len * per_loop * 10) / (1024 * 1024 * seconds));
}

int
main (int argc, char *argv[])
{
  int compress = 1;
  if (argc > 1 && !strcmp (argv[1], "-d"))
    compress = 0;
  if (argc > 1 && !strcmp (argv[1], "-b"))
    benchmark (stdin);
  else
    transfer_file (stdin, stdout, compress);
  return 0;
}

#endif
