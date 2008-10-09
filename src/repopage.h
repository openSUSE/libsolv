/*
 * Copyright (c) 2007-2008, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define BLOB_PAGEBITS 15
#define BLOB_PAGESIZE (1 << BLOB_PAGEBITS)

/* load pages pstart..pend into consecutive memory, return address */
unsigned char *repodata_load_page_range(Repodata *data, unsigned int pstart, unsigned int pend);

/* compress a page, return compressed len */
unsigned int repodata_compress_page(unsigned char *page, unsigned int len, unsigned char *cpage, unsigned int max);

/* setup page data for repodata_load_page_range */
void repodata_read_or_setup_pages(Repodata *data, unsigned int pagesz, unsigned int blobsz);
