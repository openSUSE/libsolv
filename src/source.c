/*
 * source.c
 *
 * Manage metadata coming from one repository
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "source.h"
#include "pool.h"
#include "poolid_private.h"
#include "util.h"

#define IDARRAY_BLOCK     4095


/*
 * create empty source
 * and add to pool
 */

Source *
pool_addsource_empty(Pool *pool)
{
  Source *source;

  pool_freewhatprovides(pool);
  source = (Source *)xcalloc(1, sizeof(*source));
  pool->sources = (Source **)xrealloc(pool->sources, (pool->nsources + 1) * sizeof(Source *));
  pool->sources[pool->nsources++] = source;
  source->name = "empty";
  source->pool = pool;
  source->start = pool->nsolvables;
  source->nsolvables = 0;
  return source;
}

/*
 * add Id to source
 * olddeps = offset into idarraydata
 * 
 */

unsigned int
source_addid(Source *source, Offset olddeps, Id id)
{
  Id *idarray;
  int idarraysize;
  int i;
  
  idarray = source->idarraydata;
  idarraysize = source->idarraysize;

  if (!idarray)			       /* check idarray size */
    {
      idarray = (Id *)xmalloc((1 + IDARRAY_BLOCK) * sizeof(Id));
      idarraysize = 1;
      source->lastoff = 0;
    }

  if (!olddeps)			       /* no deps yet */
    {   
      olddeps = idarraysize;
      if ((idarraysize & IDARRAY_BLOCK) == 0)
        idarray = (Id *)xrealloc(idarray, (idarraysize + 1 + IDARRAY_BLOCK) * sizeof(Id));
    }   
  else if (olddeps == source->lastoff) /* append at end */
    idarraysize--;
  else				       /* check space */
    {
      i = olddeps;
      olddeps = idarraysize;
      for (; idarray[i]; i++)
        {
          if ((idarraysize & IDARRAY_BLOCK) == 0)
            idarray = (Id *)xrealloc(idarray, (idarraysize + 1 + IDARRAY_BLOCK) * sizeof(Id));
          idarray[idarraysize++] = idarray[i];
        }
      if ((idarraysize & IDARRAY_BLOCK) == 0)
        idarray = (Id *)xrealloc(idarray, (idarraysize + 1 + IDARRAY_BLOCK) * sizeof(Id));
    }
  
  idarray[idarraysize++] = id;	       /* insert Id into array */

  if ((idarraysize & IDARRAY_BLOCK) == 0)   /* realloc if at block boundary */
    idarray = (Id *)xrealloc(idarray, (idarraysize + 1 + IDARRAY_BLOCK) * sizeof(Id));

  idarray[idarraysize++] = ID_NULL;    /* ensure NULL termination */

  source->idarraydata = idarray;
  source->idarraysize = idarraysize;
  source->lastoff = olddeps;

  return olddeps;
}


/*
 * add dependency (as Id) to source
 * olddeps = offset into idarraydata
 * isreq = 0 for normal dep
 * isreq = 1 for requires
 * isreq = 2 for pre-requires
 * 
 */

unsigned int
source_addid_dep(Source *source, Offset olddeps, Id id, int isreq)
{
  Id oid, *oidp, *marker = 0;

  if (!olddeps)
    return source_addid(source, olddeps, id);

  if (!isreq)
    {
      for (oidp = source->idarraydata + olddeps; (oid = *oidp) != ID_NULL; oidp++)
	{
	  if (oid == id)
	    return olddeps;
	}
      return source_addid(source, olddeps, id);
    }

  for (oidp = source->idarraydata + olddeps; (oid = *oidp) != ID_NULL; oidp++)
    {
      if (oid == SOLVABLE_PREREQMARKER)
	marker = oidp;
      else if (oid == id)
	break;
    }

  if (oid)
    {
      if (marker || isreq == 1)
        return olddeps;
      marker = oidp++;
      for (; (oid = *oidp) != ID_NULL; oidp++)
        if (oid == SOLVABLE_PREREQMARKER)
          break;
      if (!oid)
        {
          oidp--;
          if (marker < oidp)
            memmove(marker, marker + 1, (oidp - marker) * sizeof(Id));
          *oidp = SOLVABLE_PREREQMARKER;
          return source_addid(source, olddeps, id);
        }
      while (oidp[1])
        oidp++;
      memmove(marker, marker + 1, (oidp - marker) * sizeof(Id));
      *oidp = id;
      return olddeps;
    }
  if (isreq == 2 && !marker)
    olddeps = source_addid(source, olddeps, SOLVABLE_PREREQMARKER);
  else if (isreq == 1 && marker)
    {
      *marker++ = id;
      id = *--oidp;
      if (marker < oidp)
        memmove(marker + 1, marker, (oidp - marker) * sizeof(Id));
      *marker = SOLVABLE_PREREQMARKER;
    }
  return source_addid(source, olddeps, id);
}


/*
 * reserve Ids
 * make space for 'num' more dependencies
 */

unsigned int
source_reserve_ids(Source *source, unsigned int olddeps, int num)
{
  num++;	/* room for trailing ID_NULL */

  if (!source->idarraysize)	       /* ensure buffer space */
    {
      source->idarraysize = 1;
      source->idarraydata = (Id *)xmalloc(((1 + num + IDARRAY_BLOCK) & ~IDARRAY_BLOCK) * sizeof(Id));
      source->lastoff = 1;
      return 1;
    }

  if (olddeps && olddeps != source->lastoff)   /* if not appending */
    {
      /* can't insert into idarray, this would invalidate all 'larger' offsets
       * so create new space at end and move existing deps there.
       * Leaving 'hole' at old position.
       */
      
      Id *idstart, *idend;
      int count;

      for (idstart = idend = source->idarraydata + olddeps; *idend++; )   /* find end */
	;
      count = idend - idstart - 1 + num;	       /* new size */

      /* realloc if crossing block boundary */
      if (((source->idarraysize - 1) | IDARRAY_BLOCK) != ((source->idarraysize + count - 1) | IDARRAY_BLOCK))
	source->idarraydata = (Id *)xrealloc(source->idarraydata, ((source->idarraysize + count + IDARRAY_BLOCK) & ~IDARRAY_BLOCK) * sizeof(Id));

      /* move old deps to end */
      olddeps = source->lastoff = source->idarraysize;
      memcpy(source->idarraydata + olddeps, idstart, count - num);
      source->idarraysize = olddeps + count - num;

      return olddeps;
    }

  if (olddeps)			       /* appending */
    source->idarraysize--;

  /* realloc if crossing block boundary */
  if (((source->idarraysize - 1) | IDARRAY_BLOCK) != ((source->idarraysize + num - 1) | IDARRAY_BLOCK))
    source->idarraydata = (Id *)xrealloc(source->idarraydata, ((source->idarraysize + num + IDARRAY_BLOCK) & ~IDARRAY_BLOCK) * sizeof(Id));

  /* appending or new */
  source->lastoff = olddeps ? olddeps : source->idarraysize;

  return source->lastoff;
}


/*
 * remove source from pool
 * 
 */

void
pool_freesource(Pool *pool, Source *source)
{
  int i, nsolvables;

  pool_freewhatprovides(pool);

  for (i = 0; i < pool->nsources; i++)	/* find source in pool */
    {
      if (pool->sources[i] == source)
	break;
    }
  if (i == pool->nsources)	       /* source not in pool, return */
    return;

  /* close gap
   * all sources point into pool->solvables _relatively_ to source->start
   * so closing the gap only needs adaption of source->start for all
   * other sources.
   */
  
  nsolvables = source->nsolvables;
  if (pool->nsolvables > source->start + nsolvables)
    memmove(pool->solvables + source->start, pool->solvables + source->start + nsolvables, (pool->nsolvables - source->start - nsolvables) * sizeof(Solvable));
  pool->nsolvables -= nsolvables;

  for (; i < pool->nsources - 1; i++)
    {
      pool->sources[i] = pool->sources[i + 1];   /* remove source */
      pool->sources[i]->start -= nsolvables;     /* adapt start offset of remaining sources */
    }
  pool->nsources = i;

  xfree(source->idarraydata);
  xfree(source->rpmdbid);
  xfree(source);
}

unsigned int
source_fix_legacy(Source *source, unsigned int provides, unsigned int supplements)
{
  Pool *pool = source->pool;
  Id id, idp, idl, idns;
  char buf[1024], *p, *dep;
  int i;

  if (provides)
    {
      for (i = provides; source->idarraydata[i]; i++)
	{
	  id = source->idarraydata[i];
	  if (ISRELDEP(id))
	    continue;
	  dep = (char *)id2str(pool, id);
	  if (!strncmp(dep, "locale(", 7) && strlen(dep) < sizeof(buf) - 2)
	    {
	      idp = 0;
	      strcpy(buf + 2, dep);
	      dep = buf + 2 + 7;
	      if ((p = strchr(dep, ':')) != 0 && p != dep)
		{
		  *p++ = 0;
		  idp = str2id(pool, dep, 1);
		  dep = p;
		}
	      id = 0;
	      while ((p = strchr(dep, ';')) != 0)
		{
		  if (p == dep)
		    {
		      dep = p + 1;
		      continue;
		    }
		  strncpy(dep - 9, "language:", 9);
		  *p++ = 0;
		  idl = str2id(pool, dep - 9, 1);
		  if (id)
		    id = rel2id(pool, id, idl, REL_OR, 1);
		  else
		    id = idl;
		  dep = p;
		}
	      if (dep[0] && dep[1])
		{
	 	  for (p = dep; *p && *p != ')'; p++)
		    ;
		  *p = 0;
		  strncpy(dep - 9, "language:", 9);
		  idl = str2id(pool, dep - 9, 1);
		  if (id)
		    id = rel2id(pool, id, idl, REL_OR, 1);
		  else
		    id = idl;
		}
	      if (idp)
		id = rel2id(pool, idp, id, REL_AND, 1);
	      if (id)
		supplements = source_addid_dep(source, supplements, id, 0);
	    }
	  else if ((p = strchr(dep, ':')) != 0 && p != dep && p[1] == '/' && strlen(dep) < sizeof(buf))
	    {
	      strcpy(buf, dep);
	      p = buf + (p - dep);
	      *p++ = 0;
	      idp = str2id(pool, buf, 1);
	      idns = str2id(pool, "namespace:installed", 1);
	      id = str2id(pool, p, 1);
	      id = rel2id(pool, idns, id, REL_NAMESPACE, 1);
	      id = rel2id(pool, idp, id, REL_AND, 1);
	      supplements = source_addid_dep(source, supplements, id, 0);
	    }
	}
    }
  if (!supplements)
    return 0;
  for (i = supplements; source->idarraydata[i]; i++)
    {
      id = source->idarraydata[i];
      if (ISRELDEP(id))
	continue;
      dep = (char *)id2str(pool, id);
      if (!strncmp(dep, "system:modalias(", 16))
	dep += 7;
      if (!strncmp(dep, "modalias(", 9) && dep[9] && dep[10] && strlen(dep) < sizeof(buf))
	{
	  strcpy(buf, dep);
	  p = strchr(buf + 9, ':');
	  idns = str2id(pool, "namespace:modalias", 1);
	  if (p && p != buf + 9 && strchr(p + 1, ':'))
	    {
	      *p++ = 0;
	      idp = str2id(pool, buf + 9, 1);
	      p[strlen(p) - 1] = 0;
	      id = str2id(pool, p, 1);
	      id = rel2id(pool, idns, id, REL_NAMESPACE, 1);
	      id = rel2id(pool, idp, id, REL_AND, 1);
	    }
	  else
	    {
	      p = buf + 9;
	      p[strlen(p) - 1] = 0;
	      id = str2id(pool, p, 1);
	      id = rel2id(pool, idns, id, REL_NAMESPACE, 1);
	    }
	  if (id)
	    source->idarraydata[i] = id;
	}
      else if (!strncmp(dep, "packageand(", 11) && strlen(dep) < sizeof(buf))
	{
	  strcpy(buf, dep);
	  id = 0;
	  dep = buf + 11;
	  while ((p = strchr(dep, ':')) != 0)
	    {
	      if (p == dep)
		{
		  dep = p + 1;
		  continue;
		}
	      *p++ = 0;
	      idp = str2id(pool, dep, 1);
	      if (id)
		id = rel2id(pool, id, idp, REL_AND, 1);
	      else
		id = idp;
	      dep = p;
	    }
	  if (dep[0] && dep[1])
	    {
	      dep[strlen(dep) - 1] = 0;
	      idp = str2id(pool, dep, 1);
	      if (id)
		id = rel2id(pool, id, idp, REL_AND, 1);
	      else
		id = idp;
	    }
	  if (id)
	    source->idarraydata[i] = id;
	}
    }
  return supplements;
}

// EOF
