SUBDIRS = src tools testsuite

AUTOMAKE_OPTIONS = no-dist-gzip dist-bzip2

package: dist

etags: TAGS
	find . -name "*.[chCH]" -print | etags -
