/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#define BLOCK_SIZE 65536

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
        - back ref of length l+2, at offset -(o+1) (o < 1 << 16)
   f 1111llll <8o> <8o> <8l>
        - back ref, length l+18 (l < 1<<12), offset -(o+1) (o < 1<<16)

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
   L >= 2, L <= 17, O < 65536                          : encode as e
   L >= 18, L <= 4095+18, O < 65536                    : encode as f
   else we have either L >= 4096+18 or O >= 65536.
   O >= 65536: encode as 1-literal, too bad
     (with the current block size this can't happen)
   L >= 4096+18, so reduce to 4095+18                  : encode as f
*/


static unsigned int
compress_buf (const unsigned char *in, unsigned int in_len,
	      unsigned char *out, unsigned int out_len)
{
  unsigned int oo = 0;		//out-offset
  unsigned int io = 0;		//in-offset
#define HS (65536)
  unsigned short htab[HS];
  unsigned short hnext[BLOCK_SIZE];
  memset (htab, 0, sizeof (htab));
  memset (hnext, 0, sizeof (hnext));
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
      for (tries = 0; tries < 4; tries++)
	{
	  unsigned int this_len, this_ofs;
	  this_len = 0;
	  this_ofs = 0;
	  if (try < io)
	    {
	      for (;
		   io + this_len < in_len
		   && in[try + this_len] == in[io + this_len]; this_len++)
		;
	      this_ofs = (io - try) - 1;
	      if (this_len < 2)
		this_len = 0;
	      else if (this_len == 2 && (litofs || ((io - try) - 1) >= 1024))
		this_len = 0;
	      else if (this_len >= 4096 + 18)
		this_len = 4095 + 18;
	      else if (this_ofs >= 65536)
		this_len = 0;
	      if (this_len > mlen || (this_len == mlen && this_ofs < mofs))
		mlen = this_len, mofs = this_ofs;
	    }
	  try = hnext[try];
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
		  if (litlen == 1 && in[litofs] < 0x80)
		    {
		      if (oo + 1 >= out_len)
			return 0;
		      out[oo++] = in[litofs++];
		      break;
		    }
		  else if (litlen == 2 && in[litofs] < 0x80
			   && in[litofs + 1] < 0x80)
		    {
		      if (oo + 2 >= out_len)
			return 0;
		      out[oo++] = in[litofs++];
		      out[oo++] = in[litofs++];
		      break;
		    }
		  else if (litlen <= 32)
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
	  else if (mlen >= 2 && mlen <= 17)
	    {
	      assert (mofs < 65536);
	      if (oo + 3 >= out_len)
		return 0;
	      out[oo++] = 0xe0 | (mlen - 2);
	      out[oo++] = mofs >> 8;
	      out[oo++] = mofs & 0xff;
	    }
	  else
	    {
	      assert (mlen >= 18 && mlen <= 4095 + 18 && mofs < 65536);
	      if (oo + 4 >= out_len)
		return 0;
	      out[oo++] = 0xf0 | ((mlen - 18) >> 8);
	      out[oo++] = mofs >> 8;
	      out[oo++] = mofs & 0xff;
	      out[oo++] = (mlen - 18) & 0xff;
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
	  if (litlen == 1 && in[litofs] < 0x80)
	    {
	      if (oo + 1 >= out_len)
		return 0;
	      out[oo++] = in[litofs++];
	      break;
	    }
	  else if (litlen == 2 && in[litofs] < 0x80 && in[litofs + 1] < 0x80)
	    {
	      if (oo + 2 >= out_len)
		return 0;
	      out[oo++] = in[litofs++];
	      out[oo++] = in[litofs++];
	      break;
	    }
	  else if (litlen <= 32)
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

static unsigned int
unchecked_decompress_buf (const unsigned char *in, unsigned int in_len,
			  unsigned char *out,
			  unsigned int out_len __attribute__((unused)))
{
  unsigned char *orig_out = out;
  const unsigned char *in_end = in + in_len;
  while (in < in_end)
    {
      unsigned int first = *in++;
      unsigned int o = o;
      switch (first >> 5)
	{
	case 0:
	case 1:
	case 2:
	case 3:
	  //a 0LLLLLLL
	  //fprintf (stderr, "lit: 1\n");
	  *out++ = first;
	  continue;
	case 4:
	  //b 100lllll <l+1 bytes>
	  {
	    unsigned int l = (first & 31) + 1;
	    //fprintf (stderr, "lit: %d\n", l);
	    while (l--)
	      *out++ = *in++;
	    continue;
	  }
	case 5:
	  //c 101oolll <8o>
	  {
	    o = first & (3 << 3);
	    o = (o << 5) | *in++;
	    first = (first & 7) + 2;
	    break;
	  }
	case 6:
	  //d 110lllll <8o>
	  {
	    o = *in++;
	    first = (first & 31) + 10;
	    break;
	  }
	case 7:
	  //e 1110llll <8o> <8o>
	  //f 1111llll <8o> <8o> <8l>
	  {
	    o = *in++ << 8;
	    o |= *in++;
	    first = first & 31;
	    if (first >= 16)
	      first = (((first - 16) << 8) | *in++) + 18;
	    else
	      first += 2;
	    break;
	  }
	}
      //fprintf (stderr, "ref: %d @ %d\n", first, o);
      o++;
      /* We know that first will not be zero, and this loop structure is
         better optimizable.  */
      do
	{
	  *out = out[-o];
	  out++;
	}
      while (--first);
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
  float seconds = (end - start) / (float) CLOCKS_PER_SEC;
  fprintf (stderr, "%.2f seconds == %.2f MB/s\n", seconds,
	   ((long long) in_len * per_loop * 10) / (1024 * 1024 * seconds));

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
