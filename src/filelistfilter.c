/*
 * Copyright (c) 2018, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * filelistfilter.c
 *
 * Support repodata with a filelist filtered by a custom filter
 */

#define _GNU_SOURCE
#include <string.h>
#include <fnmatch.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>

#include "repo.h"
#include "pool.h"
#include "util.h"

static Id default_filelist_filter;

#define FF_EXACT	0
#define FF_END		1
#define FF_START	2
#define FF_SUB		3		/* FF_END | FF_START */
#define FF_GLOB		4
#define FF_START5	5

void
repodata_free_filelistfilter(Repodata *data)
{
  if (data->filelistfilter)
    {
      if (data->filelistfilter != &default_filelist_filter)
        solv_free(data->filelistfilter);
      data->filelistfilter = 0;
    }
  data->filelistfilterdata = solv_free(data->filelistfilterdata);
}

static void
repodata_set_filelistfilter(Repodata *data)
{
  Id type;
  Queue q;
  int i, j;
  char *filterdata;
  int nfilterdata;

  if (data->filelistfilter && data->filelistfilter != &default_filelist_filter)
    data->filelistfilter = solv_free(data->filelistfilter);
  data->filelistfilterdata = solv_free(data->filelistfilterdata);
  type = repodata_lookup_type(data, SOLVID_META, REPOSITORY_FILTEREDFILELIST);
  if (type != REPOKEY_TYPE_IDARRAY)
    {
      data->filelistfilter = &default_filelist_filter;
      return;
    }
  queue_init(&q);
  repodata_lookup_idarray(data, SOLVID_META, REPOSITORY_FILTEREDFILELIST, &q);
  if (q.count == 3)
    {
      /* check if this is the default filter */
      int t = 0;
      for (i = 0; i < 3; i++)
	{
	  Id id = q.elements[i];
	  const char *g = data->localpool ? stringpool_id2str(&data->spool, id) : pool_id2str(data->repo->pool, id); 
	  if (!strcmp(g, "*bin/*"))
	    t |= 1;
	  else if (!strcmp(g, "/etc/*"))
	    t |= 2;
	  else if (!strcmp(g, "/usr/lib/sendmail"))
	    t |= 4;
	}
      if (t == 7)
	{
	  queue_free(&q);
	  data->filelistfilter = &default_filelist_filter;
	  return;
	}
    }
  data->filelistfilter = solv_calloc(q.count * 2 + 1, sizeof(Id));
  filterdata = solv_calloc_block(1, 1, 255);
  nfilterdata = 1;
  
  for (i = j = 0; i < q.count; i++)
    {
      Id id = q.elements[i];
      const char *g = data->localpool ? stringpool_id2str(&data->spool, id) : pool_id2str(data->repo->pool, id); 
      const char *p;
      int t = FF_EXACT;
      int gl;
      if (!id || !g || !*g)
	continue;
      for (p = g; *p && t != FF_GLOB; p++)
	{
	  if (*p == '*')
	    {
	      if (p == g)
		t |= FF_END;
	      else if (!p[1])
		t |= FF_START;
	      else
		t = FF_GLOB;
	    }
	  else if (*p == '[' || *p == '?')
	    t = FF_GLOB;
	}
      gl = strlen(g);
      if (t == FF_END)	/* not supported */
	t = FF_GLOB;
      if (t == FF_START && gl == 5)
	t = FF_START5;
      filterdata = solv_extend(filterdata, nfilterdata, gl + 1, 1, 255);
      data->filelistfilter[j++] = nfilterdata;
      data->filelistfilter[j++] = t;
      switch (t)
	{
	case FF_START:
	case FF_START5:
	  strcpy(filterdata + nfilterdata, g);
	  filterdata[nfilterdata + gl - 1] = 0;
	  nfilterdata += gl;
	  break;
	case FF_SUB:
	  strcpy(filterdata + nfilterdata, g + 1);
	  filterdata[nfilterdata + gl - 2] = 0;
	  nfilterdata += gl - 1;
	  break;
	default:
	  strcpy(filterdata + nfilterdata, g);
	  nfilterdata += gl + 1;
	  break;
	}
    }
  filterdata = solv_realloc(filterdata, nfilterdata);
  data->filelistfilter[j++] = 0;
  data->filelistfilterdata = filterdata;
  queue_free(&q);
}

int
repodata_filelistfilter_matches(Repodata *data, const char *str)
{
  Id *ff;
  if (data && !data->filelistfilter)
    repodata_set_filelistfilter(data);
  if (!data || data->filelistfilter == &default_filelist_filter)
    {
      /* '.*bin\/.*', '^\/etc\/.*', '^\/usr\/lib\/sendmail$' */
      if (strstr(str, "bin/"))
	return 1;
      if (!strncmp(str, "/etc/", 5))
	return 1;
      if (!strcmp(str, "/usr/lib/sendmail"))
	return 1;
      return 0;
    }
  for (ff = data->filelistfilter; *ff; ff += 2)
    {
      const char *g = data->filelistfilterdata + *ff;
      switch (ff[1])
	{
	case FF_EXACT:
	  if (!strcmp(str, g))
	    return 1;
	  break;
	case FF_START:
	  if (!strncmp(str, g, strlen(g)))
	    return 1;
	  break;
	case FF_SUB:
	  if (!strstr(str, g))
	    return 1;
	  break;
	case FF_START5:
	  if (!strncmp(str, g, 5))
	    return 1;
	  break;
	default:
	  if (!fnmatch(g, str, 0))
	    return 1;
	  break;
	}
    }
  return 0;
}

