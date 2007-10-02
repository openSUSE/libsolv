#!/bin/sh 

UNAME=`uname`

if [ "$UNAME" = "Darwin" ]; then
libtoolize --copy --force --automake
aclocal-1.9
autoheader-2.60
automake-1.9 --add-missing --copy --foreign
autoconf-2.60

else
libtoolize --copy --force --automake
aclocal
autoheader
automake --add-missing --copy --foreign
autoconf
fi
