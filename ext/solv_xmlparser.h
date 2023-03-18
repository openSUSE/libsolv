
struct solv_xmlparser_element {
  int fromstate;
  char *element;
  int tostate;
  int docontent;
};

struct solv_xmlparser {
  void *userdata;
  char *errstr;
  unsigned int line;
  unsigned int column;

  int state;
  int docontent;

  Queue elementq;
  int unknowncnt;

  char *content;
  int lcontent;		/* current content length */
  int acontent;		/* allocated content length */

  struct solv_xmlparser_element *elements;
  int nelements;

  void (*startelement)(struct solv_xmlparser *xmlp, int state, const char *name, const char **atts);
  void (*endelement)(struct solv_xmlparser *xmlp, int state, char *content);

  Id *elementhelper;
  void *parser;
  void *attsdata;
};

#define SOLV_XMLPARSER_OK	0
#define SOLV_XMLPARSER_ERROR	-1

static inline const char *
solv_xmlparser_find_attr(const char *txt, const char **atts)
{
  for (; *atts; atts += 2)
    if (!strcmp(*atts, txt))
      return atts[1];
  return 0;
}

extern void solv_xmlparser_init(struct solv_xmlparser *xmlp, struct solv_xmlparser_element *elements, void *userdata,
    void (*startelement)(struct solv_xmlparser *xmlp, int state, const char *name, const char **atts),
    void (*endelement)(struct solv_xmlparser *xmlp, int state, char *content));

extern void solv_xmlparser_free(struct solv_xmlparser *xmlp);
extern int solv_xmlparser_parse(struct solv_xmlparser *xmlp, FILE *fp);
unsigned int solv_xmlparser_lineno(struct solv_xmlparser *xmlp);
char *solv_xmlparser_contentspace(struct solv_xmlparser *xmlp, int l);


