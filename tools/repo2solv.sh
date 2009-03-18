#! /bin/sh
# repo2solv
#
# give it a directory of a local mirror of a repo and this
# tries to detect the repo type and generate one SOLV file on stdout

get_DESCRDIR () {
  local d=$(grep '^DESCRDIR' content | sed 's/^DESCRDIR[[:space:]]\+\(.*[^[:space:]]\)[[:space:]]*$/\1/')
  if  test -z "$d"; then
    echo suse/setup/desc
  else
    echo ${d}
  fi
}

test_susetags() {
  if test -s content; then
    DESCR=$(get_DESCRDIR)
    test -d $DESCR
    return $?
  else
    return 1
  fi
}

# signal an error if there is a problem
set -e

LANG=C
unset CDPATH
parser_options=${PARSER_OPTIONS:-}

findopt="-prune"
repotype=

while true ; do
  if test "$1" = "-o" ; then
    exec > "$2"
    shift
    shift
  elif test "$1" = "-R" ; then
    # recursive
    findopt=
    repotype=plaindir
    shift
  else
    break
  fi
done

dir="$1"
cd "$dir" || exit 1

if test -z "$repotype" ; then
  # autodetect repository type
  if test -d repodata ; then
    repotype=rpmmd
  elif test_susetags ; then
    repotype=susetags
  else
    repotype=plaindir
  fi
fi

if test "$repotype" = rpmmd ; then
  cd repodata || exit 2

  primfile="/nonexist"
  if test -f primary.xml || test -f primary.xml.gz || test -f primary.xml.bz2 ; then
    primfile=`mktemp` || exit 3
    (
     # fake tag to combine primary.xml and extensions
     # like susedata.xml, other.xml, filelists.xml
     echo '<rpmmd>'
     for i in primary.xml* susedata.xml*; do
       test -s "$i" || continue
       case $i in
         *.gz) gzip -dc "$i";;
	 *.bz2) bzip2 -dc "$i";;
	 *) cat "$i";;
       esac
       # add a newline
       echo
       # only the first
       break
     done
     for i in susedata.xml*; do
       test -s "$i" || continue
       case $i in
         *.gz) gzip -dc "$i";;
 	 *.bz2) bzip2 -dc "$i";;
         *) cat "$i";;
       esac
       # only the first
       break
     done
     echo '</rpmmd>'
    ) | grep -v '\?xml' |  sed '1i\<?xml version="1.0" encoding="UTF-8"?>' | rpmmd2solv $parser_options > $primfile || exit 4
  fi

  prodfile="/nonexist"
  if test -f product.xml; then
    prodfile=`mktemp` || exit 3
    (
     echo '<products>'
     for i in product*.xml*; do
       case $i in
         *.gz) gzip -dc "$i" ;;
	 *.bz2) bzip2 -dc "$i" ;;
	 *) cat "$i" ;;
       esac
     done
     echo '</products>'
    ) | grep -v '\?xml' | rpmmd2solv $parser_options > $prodfile || exit 4
  fi

  cmd=
  patternfile="/nonexist"
  for i in patterns.xml*; do
    test -s "$i" || continue
    case $i in
      *.gz) cmd='gzip -dc' ;;
      *.bz2) cmd='bzip2 -dc' ;;
      *) cmd='cat' ;;
    esac
    break
  done
  if test -n "$cmd" ; then
    patternfile=`mktemp` || exit 3
    $cmd "$i" | rpmmd2solv $parser_options > $patternfile || exit 4
  fi

  # This contains repomd.xml
  # for now we only read some keys like timestamp
  cmd=
  for i in repomd.xml*; do
      test -s "$i" || continue
      case $i in
	  *.gz) cmd="gzip -dc" ;;
	  *.bz2) cmd="bzip2 -dc" ;;
	  *) cmd="cat" ;;
      esac
      # only check the first repomd.xml*, in case there are more
      break
  done
  repomdfile="/nonexist"
  if test -n "$cmd"; then
      # we have some repomd.xml*
      repomdfile=`mktemp` || exit 3
      $cmd "$i" | repomdxml2solv $parser_options > $repomdfile || exit 4
  fi

  # This contains suseinfo.xml, which is extensions to repomd.xml
  # for now we only read some keys like expiration and products
  cmd=
  for i in suseinfo.xml*; do
      test -s "$i" || continue
      case $i in
	  *.gz) cmd="gzip -dc" ;;
	  *.bz2) cmd="bzip2 -dc" ;;
	  *) cmd="cat" ;;
      esac
      # only check the first suseinfo.xml*, in case there are more
      break
  done
  suseinfofile="/nonexist"
  if test -n "$cmd"; then
      # we have some suseinfo.xml*
      suseinfofile=`mktemp` || exit 3
      $cmd "$i" | repomdxml2solv $parser_options > $suseinfofile || exit 4
  fi

  # This contains a updateinfo.xml* and maybe patches
  cmd=
  for i in updateinfo.xml*; do
      test -s "$i" || continue
      case $i in
	  *.gz) cmd="gzip -dc" ;;
	  *.bz2) cmd="bzip2 -dc" ;;
	  *) cmd="cat" ;;
      esac
      # only check the first updateinfo.xml*, in case there are more
      break
  done
  updateinfofile="/nonexist"
  if test -n "$cmd"; then
      # we have some updateinfo.xml*
      updateinfofile=`mktemp` || exit 3
      $cmd "$i" | updateinfoxml2solv $parser_options > $updateinfofile || exit 4
  fi

  # This contains a deltainfo.xml*
  cmd=
  for i in deltainfo.xml*; do
      test -s "$i" || continue
      case $i in
	  *.gz) cmd="gzip -dc" ;;
	  *.bz2) cmd="bzip2 -dc" ;;
	  *) cmd="cat" ;;
      esac
      # only check the first deltainfo.xml*, in case there are more
      break
  done
  deltainfofile="/nonexist"
  if test -n "$cmd"; then
      # we have some deltainfo.xml*
      deltainfofile=`mktemp` || exit 3
      $cmd "$i" | deltainfoxml2solv $parser_options > $deltainfofile || exit 4
  fi

  # Now merge primary, patches, updateinfo, and deltainfo
  if test -s $repomdfile; then
    m_repomdfile=$repomdfile
  fi
  if test -s $suseinfofile; then
    m_suseinfofile=$suseinfofile
  fi
  if test -s $primfile; then
    m_primfile=$primfile
  fi
  if test -s $patternfile; then
    m_patternfile=$patternfile
  fi
  if test -s $prodfile; then
    m_prodfile=$prodfile
  fi
  if test -s $updateinfofile; then
    m_updateinfofile=$updateinfofile
  fi
  if test -s $deltainfofile; then
    m_deltainfofile=$deltainfofile
  fi
  mergesolv $m_repomdfile $m_suseinfofile $m_primfile $m_prodfile $m_patternfile $m_updateinfofile $m_deltainfofile
  rm -f $repomdfile $suseinfofile $primfile $patternfile $prodfile $updateinfofile $deltainfofile

elif test "$repotype" = susetags ; then
  olddir=`pwd`
  DESCR=$(get_DESCRDIR)
  cd ${DESCR} || exit 2
  (
    # First packages
    if test -s packages.gz; then
      gzip -dc packages.gz
    elif test -s packages.bz2; then
      bzip2 -dc packages.bz2
    elif test -s packages; then
      cat packages
    fi

    # DU
    if test -s packages.DU.gz; then
      gzip -dc packages.DU.gz
    elif test -s packages.DU.bz2; then
      bzip2 -dc packages.DU.bz2
    elif test -s packages.DU; then
      cat packages.DU
    fi

    # Now default language
    if test -s packages.en.gz; then
      gzip -dc packages.en.gz
    elif test -s packages.en.bz2; then
      bzip2 -dc packages.en.bz2
    elif test -s packages.en; then
      cat packages.en
    fi

    # Now patterns.  Not simply those files matching *.pat{,.gz,bz2},
    # but only those mentioned in the file 'patterns'
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

    # Now all other packages.{lang}.  Needs to come last as it switches
    # languages for all following susetags files
    for i in packages.*; do
      case $i in
	*.gz) name="${i%.gz}" ; prog="gzip -dc" ;;
	*.bz2) name="${i%.bz2}" ; prog="bzip2 -dc" ;;
	*) name="$i"; prog=cat ;;
      esac
      case $name in
	# ignore files we handled already
	*.DU | *.en | *.FL | packages ) continue ;;
	*)
	  suff=${name#packages.}
	  echo "=Lan: $suff"
	  $prog "$i" ;;
      esac
    done

  ) | susetags2solv -c "${olddir}/content" $parser_options || exit 4
  cd "$olddir"
elif test "$repotype" = plaindir ; then
  find * -name .\* -prune -o $findopt -name \*.delta.rpm -o -name \*.patch.rpm -o -name \*.rpm -a -type f -print0 | rpms2solv -0 -m -
else
  echo "unknown repository type '$repotype'" >&2
  exit 1
fi
