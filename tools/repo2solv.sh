#! /bin/sh
# repo2solv
#
# give it a directory of a local mirror of a repo and this
# tries to detect the repo type and generate one SOLV file on stdout

# this should signal an error if there is a problem
set -e 

LANG=C
parser_options=${PARSER_OPTIONS:-}


dir="$1"
cd "$dir" || exit 1
if test -d repodata; then
  cd repodata || exit 2

  # This contains a primary.xml* and maybe patches
  for i in primary.xml*; do
    case $i in
      *.gz) cmd="gzip -dc" ;;
      *.bz2) cmd="bzip2 -dc" ;;
      *) cmd="cat" ;;
    esac
    # only check the first primary.xml*, in case there are more
    break
  done
  primfile="/nonexist"
  if test -n "$cmd"; then
    # we have some primary.xml*
    primfile=`mktemp` || exit 3
    $cmd $i | rpmmd2solv $parser_options > $primfile || exit 4
  fi

  # This contains a updateinfo.xml* and maybe patches
  if test -f updateinfo.xml || test -f updateinfo.xml.gz || test -f updateinfo.xml.bz2 ; then
      for i in updateinfo.xml*; do
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
          $cmd $i | updateinfoxml2solv $parser_options > $updateinfofile || exit 4
      fi
  fi

  patchfile="/nonexist"
  if test -f patches.xml; then
    patchfile=`mktemp` || exit 3
    (
     echo '<patches>'
     for i in patch-*.xml*; do
       case $i in
         *.gz) gzip -dc "$i" ;;
	 *.bz2) bzip2 -dc "$i" ;;
	 *) cat "$i" ;;
       esac
     done
     echo '</patches>'
    ) | grep -v '\?xml' | patchxml2solv $parser_options > $patchfile || exit 4
  fi

  # Now merge primary and patches and updateinfo
  if test -s $primfile && test -s $patchfile && test -s $updateinfofile; then
      mergesolv $primfile $patchfile $updateinfofile
  elif test -s $primfile && test -s $updateinfofile; then
      mergesolv $primfile $updateinfofile
  elif test -s $primfile && test -s $patchfile; then
    mergesolv $primfile $patchfile
  elif test -s $primfile; then
    cat $primfile
  elif test -s $patchfile; then
    cat $patchfile
  fi
  rm -f $primfile $patchfile $updateinfofile
elif test -d suse/setup/descr && test -s content; then
  olddir=`pwd`
  cd suse/setup/descr || exit 2
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
	  eval "$prog '$i'" ;;
      esac
    done

  ) | susetags2solv -c "${olddir}/content" $parser_options || exit 4
  cd "$olddir"
else
  rpms=''
  for r in *.rpm ; do
    rpms="$rpms$r
"
  done
  if test -n "$rpms" ; then
      echo "$rpms" | rpms2solv -m -
  else
      exit 1
  fi
fi
