/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * knownid.h
 *
 */

#undef KNOWNID
#ifdef KNOWNID_INITIALIZE
# define KNOWNID(a, b) b
static const char *initpool_data[] = {
#else
# define KNOWNID(a, b) a
enum sat_knownid {
#endif

KNOWNID(ID_NULL,			"<NULL>"),
KNOWNID(ID_EMPTY,			""),
KNOWNID(SOLVABLE_NAME,			"solvable:name"),
KNOWNID(SOLVABLE_ARCH,			"solvable:arch"),
KNOWNID(SOLVABLE_EVR,			"solvable:evr"),
KNOWNID(SOLVABLE_VENDOR,		"solvable:vendor"),
KNOWNID(SOLVABLE_PROVIDES,		"solvable:provides"),
KNOWNID(SOLVABLE_OBSOLETES,		"solvable:obsoletes"),
KNOWNID(SOLVABLE_CONFLICTS,		"solvable:conflicts"),
KNOWNID(SOLVABLE_REQUIRES,		"solvable:requires"),
KNOWNID(SOLVABLE_RECOMMENDS,		"solvable:recommends"),
KNOWNID(SOLVABLE_SUGGESTS,		"solvable:suggests"),
KNOWNID(SOLVABLE_SUPPLEMENTS,		"solvable:supplements"),
KNOWNID(SOLVABLE_ENHANCES,		"solvable:enhances"),
KNOWNID(SOLVABLE_FRESHENS,		"solvable:freshens"),
KNOWNID(RPM_RPMDBID,			"rpm:dbid"),
/* normal requires before this, prereqs after this */
KNOWNID(SOLVABLE_PREREQMARKER,		"solvable:prereqmarker"),
/* normal provides before this, generated file provides after this */
KNOWNID(SOLVABLE_FILEMARKER,		"solvable:filemarker"),
KNOWNID(NAMESPACE_INSTALLED,		"namespace:installed"),
KNOWNID(NAMESPACE_MODALIAS,		"namespace:modalias"),
KNOWNID(NAMESPACE_SPLITPROVIDES,	"namespace:splitprovides"),
KNOWNID(NAMESPACE_LANGUAGE,		"namespace:language"),
KNOWNID(NAMESPACE_FILESYSTEM,		"namespace:filesystem"),
KNOWNID(SYSTEM_SYSTEM,			"system:system"),
KNOWNID(ARCH_SRC,			"src"),
KNOWNID(ARCH_NOSRC,			"nosrc"),
KNOWNID(ARCH_NOARCH,			"noarch"),
KNOWNID(REPODATA_INFO,			"repodata:info"),
KNOWNID(REPODATA_EXTERNAL,		"repodata:external"),
KNOWNID(REPODATA_KEYS,			"repodata:keys"),
KNOWNID(REPODATA_LOCATION,		"repodata:location"),
KNOWNID(REPODATA_ADDEDFILEPROVIDES,	"repodata:addedfileprovides"),

/* The void type is usable to encode one-valued attributes, they have
   no associated data.  This is useful to encode values which many solvables
   have in common, and whose overall set is relatively limited.  A prime
   example would be the media number.  The actual value is encoded in the
   SIZE member of the key structure.  Be warned: careless use of this
   leads to combinatoric explosion of number of schemas.  */
KNOWNID(REPOKEY_TYPE_VOID,		"repokey:type:void"),
KNOWNID(REPOKEY_TYPE_CONSTANT,		"repokey:type:constant"),
KNOWNID(REPOKEY_TYPE_CONSTANTID,	"repokey:type:constantid"),
KNOWNID(REPOKEY_TYPE_ID,		"repokey:type:id"),
KNOWNID(REPOKEY_TYPE_NUM,		"repokey:type:num"),
KNOWNID(REPOKEY_TYPE_U32,		"repokey:type:num32"),
KNOWNID(REPOKEY_TYPE_DIR,		"repokey:type:dir"),
KNOWNID(REPOKEY_TYPE_STR,		"repokey:type:str"),
KNOWNID(REPOKEY_TYPE_IDARRAY,		"repokey:type:idarray"),
KNOWNID(REPOKEY_TYPE_REL_IDARRAY,	"repokey:type:relidarray"),
KNOWNID(REPOKEY_TYPE_DIRSTRARRAY,	"repokey:type:dirstrarray"),
KNOWNID(REPOKEY_TYPE_DIRNUMNUMARRAY,	"repokey:type:dirnumnumarray"),
KNOWNID(REPOKEY_TYPE_MD5,		"repokey:type:md5"),
KNOWNID(REPOKEY_TYPE_SHA1,		"repokey:type:sha1"),
KNOWNID(REPOKEY_TYPE_SHA256,		"repokey:type:sha256"),

KNOWNID(SOLVABLE_SUMMARY,		"solvable:summary"),
KNOWNID(SOLVABLE_DESCRIPTION,		"solvable:description"),
KNOWNID(SOLVABLE_AUTHORS,		"solvable:authors"),
KNOWNID(SOLVABLE_GROUP,			"solvable:group"),
KNOWNID(SOLVABLE_KEYWORDS,		"solvable:keywords"),
KNOWNID(SOLVABLE_LICENSE,		"solvable:license"),
KNOWNID(SOLVABLE_BUILDTIME,		"solvable:buildtime"),
KNOWNID(SOLVABLE_EULA,			"solvable:eula"),
KNOWNID(SOLVABLE_MESSAGEINS,		"solvable:messageins"),
KNOWNID(SOLVABLE_MESSAGEDEL,		"solvable:messagedel"),
KNOWNID(SOLVABLE_INSTALLSIZE,		"solvable:installsize"),
KNOWNID(SOLVABLE_DISKUSAGE,		"solvable:diskusage"),
KNOWNID(SOLVABLE_FILELIST,		"solvable:filelist"),
KNOWNID(SOLVABLE_INSTALLTIME,		"solvable:installtime"),
KNOWNID(SOLVABLE_MEDIADIR,		"solvable:mediadir"),
KNOWNID(SOLVABLE_MEDIAFILE,		"solvable:mediafile"),
KNOWNID(SOLVABLE_MEDIANR,		"solvable:medianr"),
KNOWNID(SOLVABLE_DOWNLOADSIZE,		"solvable:downloadsize"),
KNOWNID(SOLVABLE_SOURCEARCH,		"solvable:sourcearch"),
KNOWNID(SOLVABLE_SOURCENAME,		"solvable:sourcename"),
KNOWNID(SOLVABLE_SOURCEEVR,		"solvable:sourceevr"),
KNOWNID(SOLVABLE_ISVISIBLE,		"solvable:isvisible"),
KNOWNID(SOLVABLE_CHECKSUM,		"solvable:checksum"),
/* pkgid: md5sum over header + payload */
KNOWNID(SOLVABLE_PKGID,			"solvable:pkgid"),
/* hdrid: sha1sum over header only */
KNOWNID(SOLVABLE_HDRID,			"solvable:hdrid"),
/* leadsigid: md5sum over lead + sigheader */
KNOWNID(SOLVABLE_LEADSIGID,		"solvable:leadsigid"),

KNOWNID(SOLVABLE_PATCHCATEGORY,		"solvable:patchcategory"),
KNOWNID(SOLVABLE_HEADEREND,		"solvable:headerend"),

KNOWNID(SOLVABLE_CATEGORY,		"solvable:category"),
KNOWNID(SOLVABLE_INCLUDES,		"solvable:includes"),
KNOWNID(SOLVABLE_EXTENDS,		"solvable:extends"),
KNOWNID(SOLVABLE_ICON,			"solvable:icon"),
KNOWNID(SOLVABLE_ORDER,			"solvable:order"),

KNOWNID(UPDATE_SEVERITY,		"update:severity"), /* update severity (security, recommended, optional, ...) */
KNOWNID(UPDATE_REBOOT,		        "update:reboot"),   /* reboot suggested (kernel update) */
KNOWNID(UPDATE_RESTART,		        "update:restart"),  /* restart suggested (update stack update) */
KNOWNID(UPDATE_TIMESTAMP,	        "update:timestamp"),/* release date */

KNOWNID(ID_NUM_INTERNAL,		0)

#ifdef KNOWNID_INITIALIZE
};
#else
};
#endif

#undef KNOWNID

