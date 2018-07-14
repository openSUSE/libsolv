/*
 * solv_xmlparser.c
 *
 * XML parser abstraction
 *
 * Copyright (c) 2017, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WITH_LIBXML2
#include <libxml/parser.h>
#else
#include <expat.h>
#endif

#include "util.h"
#include "queue.h"
#include "solv_xmlparser.h"

static inline void
add_contentspace(struct solv_xmlparser *xmlp, int l)
{
  l += xmlp->lcontent + 1;	/* plus room for trailing zero */
  if (l > xmlp->acontent)
    {    
      xmlp->acontent = l + 256; 
      xmlp->content = solv_realloc(xmlp->content, xmlp->acontent);
    }    
}


#ifdef WITH_LIBXML2
static void
character_data(void *userData, const xmlChar *s, int len)
#else
static void XMLCALL
character_data(void *userData, const XML_Char *s, int len) 
#endif
{
  struct solv_xmlparser *xmlp = userData;

  if (!xmlp->docontent || !len)
    return;
  add_contentspace(xmlp, len);
  memcpy(xmlp->content + xmlp->lcontent, s, len);
  xmlp->lcontent += len; 
}

#ifdef WITH_LIBXML2
static void
start_element(void *userData, const xmlChar *name, const xmlChar **atts)
#else
static void XMLCALL
start_element(void *userData, const char *name, const char **atts)
#endif
{
  struct solv_xmlparser *xmlp = userData;
  struct solv_xmlparser_element *elements;
  Id *elementhelper;
  struct solv_xmlparser_element *el;
  int i, oldstate;

  if (xmlp->unknowncnt)
    {
      xmlp->unknowncnt++;
      return;
    }
  elementhelper = xmlp->elementhelper;
  elements = xmlp->elements;
  oldstate = xmlp->state;
  for (i = elementhelper[xmlp->nelements + oldstate]; i; i = elementhelper[i - 1])
    if (!strcmp(elements[i - 1].element, (char *)name))
      break;
  if (!i)
    {
#if 0
      fprintf(stderr, "into unknown: %s\n", name);
#endif
      xmlp->unknowncnt++;
      return;
    }
  el = xmlp->elements + i - 1;
  queue_push(&xmlp->elementq, xmlp->state);
  xmlp->state = el->tostate;
  xmlp->docontent = el->docontent;
  xmlp->lcontent = 0;
#ifdef WITH_LIBXML2
  if (!atts)
    {
      static const char *nullattr;
      atts = (const xmlChar **)&nullattr;
    }
#endif
  if (xmlp->state != oldstate)
    xmlp->startelement(xmlp, xmlp->state, el->element, (const char **)atts);
}

#ifdef WITH_LIBXML2
static void
end_element(void *userData, const xmlChar *name)
#else
static void XMLCALL
end_element(void *userData, const char *name)
#endif
{
  struct solv_xmlparser *xmlp = userData;

  if (xmlp->unknowncnt)
    {
      xmlp->unknowncnt--;
      xmlp->lcontent = 0;
      xmlp->docontent = 0;
      return;
    }
  xmlp->content[xmlp->lcontent] = 0;
  if (xmlp->elementq.count && xmlp->state != xmlp->elementq.elements[xmlp->elementq.count - 1])
    xmlp->endelement(xmlp, xmlp->state, xmlp->content);
  xmlp->state = queue_pop(&xmlp->elementq);
  xmlp->docontent = 0;
  xmlp->lcontent = 0;
}

void
solv_xmlparser_init(struct solv_xmlparser *xmlp,
    struct solv_xmlparser_element *elements,
    void *userdata,
    void (*startelement)(struct solv_xmlparser *, int state, const char *name, const char **atts),
    void (*endelement)(struct solv_xmlparser *, int state, char *content),
    void (*errorhandler)(struct solv_xmlparser *, const char *errstr, unsigned int line, unsigned int column))
{
  int i, nstates, nelements;
  struct solv_xmlparser_element *el;
  Id *elementhelper;

  memset(xmlp, 0, sizeof(*xmlp));
  nstates = 0;
  nelements = 0;
  for (el = elements; el->element; el++)
    {
      nelements++;
      if (el->fromstate > nstates)
	nstates = el->fromstate;
      if (el->tostate > nstates)
	nstates = el->tostate;
    }
  nstates++;

  xmlp->elements = elements;
  xmlp->nelements = nelements;
  elementhelper = solv_calloc(nelements + nstates, sizeof(Id));
  for (i = nelements - 1; i >= 0; i--)
    {
      int fromstate = elements[i].fromstate;
      elementhelper[i] = elementhelper[nelements + fromstate];
      elementhelper[nelements + fromstate] = i + 1;
    }
  xmlp->elementhelper = elementhelper;
  queue_init(&xmlp->elementq);
  xmlp->acontent = 256;
  xmlp->content = solv_malloc(xmlp->acontent);

  xmlp->userdata = userdata;
  xmlp->startelement = startelement;
  xmlp->endelement = endelement;
  xmlp->errorhandler = errorhandler;
}

void
solv_xmlparser_free(struct solv_xmlparser *xmlp)
{
  xmlp->elementhelper = solv_free(xmlp->elementhelper);
  queue_free(&xmlp->elementq);
  xmlp->content = solv_free(xmlp->content);
}

#ifdef WITH_LIBXML2

static inline int
create_parser(struct solv_xmlparser *xmlp)
{
  /* delayed to parse_block so that we have the first bytes */
  return 1;
}

static inline void
free_parser(struct solv_xmlparser *xmlp)
{
  if (xmlp->parser)
    xmlFreeParserCtxt(xmlp->parser);
  xmlp->parser = 0;
}

static xmlParserCtxtPtr create_parser_ctx(struct solv_xmlparser *xmlp, char *buf, int l)
{
  xmlSAXHandler sax;
  memset(&sax, 0, sizeof(sax));
  sax.startElement = start_element;
  sax.endElement = end_element;
  sax.characters = character_data;
  return xmlCreatePushParserCtxt(&sax, xmlp, buf, l, NULL);
}

static inline int
parse_block(struct solv_xmlparser *xmlp, char *buf, int l)
{
  if (!xmlp->parser)
    {
      int l2 = l > 4 ? 4 : 0;
      xmlp->parser = create_parser_ctx(xmlp, buf, l2);
      if (!xmlp->parser)
	{
	  xmlp->errorhandler(xmlp, "could not create parser", 0, 0);
	  return 0;
	}
      buf += l2;
      l -= l2;
      if (l2 && !l)
	return 1;
    }
  if (xmlParseChunk(xmlp->parser, buf, l, l == 0 ? 1 : 0))
    {
      xmlErrorPtr err = xmlCtxtGetLastError(xmlp->parser);
      xmlp->errorhandler(xmlp, err->message, err->line, err->int2);
      return 0;
    }
  return 1;
}

unsigned int
solv_xmlparser_lineno(struct solv_xmlparser *xmlp)
{
  return (unsigned int)xmlSAX2GetLineNumber(xmlp->parser);
}

#else

static inline int
create_parser(struct solv_xmlparser *xmlp)
{
  xmlp->parser = XML_ParserCreate(NULL);
  if (!xmlp->parser)
    return 0;
  XML_SetUserData(xmlp->parser, xmlp);
  XML_SetElementHandler(xmlp->parser, start_element, end_element);
  XML_SetCharacterDataHandler(xmlp->parser, character_data);
  return 1;
}

static inline void
free_parser(struct solv_xmlparser *xmlp)
{
  XML_ParserFree(xmlp->parser);
  xmlp->parser = 0;
}

static inline int
parse_block(struct solv_xmlparser *xmlp, char *buf, int l)
{
  if (XML_Parse(xmlp->parser, buf, l, l == 0) == XML_STATUS_ERROR)
    {
      unsigned int line = XML_GetCurrentLineNumber(xmlp->parser);
      unsigned int column = XML_GetCurrentColumnNumber(xmlp->parser);
      xmlp->errorhandler(xmlp, XML_ErrorString(XML_GetErrorCode(xmlp->parser)), line, column);
      return 0;
    }
  return 1;
}

unsigned int
solv_xmlparser_lineno(struct solv_xmlparser *xmlp)
{
  return (unsigned int)XML_GetCurrentLineNumber(xmlp->parser);
}

#endif

void
solv_xmlparser_parse(struct solv_xmlparser *xmlp, FILE *fp)
{
  char buf[8192];
  int l;

  xmlp->state = 0;
  xmlp->unknowncnt = 0;
  xmlp->docontent = 0;
  xmlp->lcontent = 0;
  queue_empty(&xmlp->elementq);

  if (!create_parser(xmlp))
    {
      xmlp->errorhandler(xmlp, "could not create xml parser", 0, 0);
      return;
    }
  for (;;)
    {
      l = fread(buf, 1, sizeof(buf), fp);
      if (!parse_block(xmlp, buf, l) || !l)
	break;
    }
  free_parser(xmlp);
}

char *
solv_xmlparser_contentspace(struct solv_xmlparser *xmlp, int l)
{
  xmlp->lcontent = 0;
  if (l > xmlp->acontent)
    {    
      xmlp->acontent = l + 256; 
      xmlp->content = solv_realloc(xmlp->content, xmlp->acontent);
    }    
  return xmlp->content;
}

