#! /bin/sh
# repo2solv
#
# give it a directory of a local mirror of a repo and this
# tries to detect the repo type and generate one SOLV file on stdout
# set -x

LANG=C

dir="$1"

cd "$dir" || exit 1


if test -d suse/setup/descr; then
  olddir=`pwd`
  cd suse/setup/descr || exit 2
  filepack=`mktemp` || exit 3

  (
    # First packages
    if test -s packages.gz; then
      gzip -dc packages.gz
    elif test -s packages.bz2; then
      bzip2 -dc packages.bz2
    elif test -s packages; then
      cat packages
    fi

    # patterns: but only those mentioned in the file 'patterns'
    if test -f patterns; then
      for i in `cat patterns`; do
        test -s "$i" || continue
        case $i in
          *.gz) gzip -dc "$i" ;;
	  *.bz2) bzip2 -dc "$i" ;;
	  *) cat "$i" ;;
	esac
      done
    fi
  ) | susetags2solv > $filepack

  cd "$olddir"
  mergesolv $filecont $filepack
  rm -f $filepack
fi
