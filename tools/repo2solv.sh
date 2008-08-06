#! /bin/sh
# repo2solv
#
# give it a directory of a local mirror of a repo and this
# tries to detect the repo type and generate one SOLV file on stdout

test_susetags() {
  if test -s content; then
    DESCR=`grep DESCRDIR content | cut -d ' ' -f 2`
    if test -z $DESCR; then
      DESCR=suse/setup/descr
    fi
    test -d $DESCR
    return $?
  else
    return 1
  fi
}

# this should signal an error if there is a problem
set -e 

LANG=C
unset CDPATH
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


  # This contains repomd.xml
  # for now we only read some keys like expiration
  if test -f repomd.xml || test -f repomd.xml.gz || test -f repomd.xml.bz2 ; then
      for i in repomd.xml*; do
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
          $cmd $i | repomdxml2solv $parser_options > $repomdfile || exit 4
      fi
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

  # This contains a deltainfo.xml*
  if test -f deltainfo.xml || test -f deltainfo.xml.gz || test -f deltainfo.xml.bz2 ; then
      for i in deltainfo.xml*; do
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
          $cmd $i | deltainfoxml2solv $parser_options > $deltainfofile || exit 4
      fi
  fi

  # Now merge primary, patches, updateinfo, and deltainfo
  if test -s $repomdfile; then
    m_repomdfile=$repomdfile
  fi
  if test -s $primfile; then
    m_primfile=$primfile
  fi
  if test -s $prodfile; then
    m_prodfile=$prodfile
  fi
  if test -s $patchfile; then
    m_patchfile=$patchfile
  fi
  if test -s $updateinfofile; then
    m_updateinfofile=$updateinfofile
  fi
  if test -s $deltainfofile; then
    m_deltainfofile=$deltainfofile
  fi
  mergesolv $m_repomdfile $m_primfile $m_prodfile $m_patchfile $m_updateinfofile $m_deltainfofile
  rm -f $repomdfile $primfile $prodfile $patchfile $updateinfofile $deltainfofile

elif test_susetags; then
  olddir=`pwd`
  DESCR=`grep DESCRDIR content | cut -d ' ' -f 2`
  if test -z $DESCR; then
    DESCR=suse/setup/descr
  fi
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
