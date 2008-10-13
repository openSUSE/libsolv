/*
 * Copyright (c) 2007-2008, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#ifndef SATSOLVER_REPOPAGE_H
#define SATSOLVER_REPOPAGE_H

#define BLOB_PAGEBITS 15
#define BLOB_PAGESIZE (1 << BLOB_PAGEBITS)

typedef struct _Attrblobpage
{
  /* mapped_at == -1  --> not loaded, otherwise offset into
     store->blob_store.  The size of the mapping is BLOB_PAGESIZE
     except for the last page.  */
  unsigned int mapped_at;
  long file_offset;
  /* file_size == 0 means the page is not backed by some file storage.
     Otherwise it is L*2+(compressed ? 1 : 0), with L being the data
     length.  */
  long file_size;
} Attrblobpage;

typedef struct _Repopagestore {
  int pagefd;	/* file descriptor we're paging from */

  unsigned char *blob_store;
  Attrblobpage *pages;
  unsigned int num_pages;

  /* mapped[i] is zero if nothing is mapped at logical page I,
   otherwise it contains the pagenumber plus one (of the mapped page).  */
  unsigned int *mapped;
  unsigned int nmapped, ncanmap;
  unsigned int rr_counter;
} Repopagestore;

void repopagestore_init(Repopagestore *store);
void repopagestore_free(Repopagestore *store);

/* load pages pstart..pend into consecutive memory, return address */
unsigned char *repopagestore_load_page_range(Repopagestore *store, unsigned int pstart, unsigned int pend);

/* compress a page, return compressed len */
unsigned int repopagestore_compress_page(unsigned char *page, unsigned int len, unsigned char *cpage, unsigned int max);

/* setup page data for repodata_load_page_range */
int repopagestore_read_or_setup_pages(Repopagestore *store, FILE *fp, unsigned int pagesz, unsigned int blobsz);

void repopagestore_disable_paging(Repopagestore *store);

#endif	/* SATSOLVER_REPOPAGE_H */
