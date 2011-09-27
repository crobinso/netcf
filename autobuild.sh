#!/bin/sh

set -e
set -v

# Make things clean.

test -n "$1" && RESULTS=$1 || RESULTS=results.log
: ${AUTOBUILD_INSTALL_ROOT=$HOME/builder}

test -f Makefile && make -k distclean || :
rm -rf coverage

test -d build && chmod -R u+rw build || :
rm -rf build
mkdir build
cd build

../autogen.sh --prefix="$AUTOBUILD_INSTALL_ROOT" \
  --enable-compile-warnings=yes

# If the MAKEFLAGS envvar does not yet include a -j option,
# add -jN where N depends on the number of processors.
case $MAKEFLAGS in
  *-j*) ;;
  *) n=$(getconf _NPROCESSORS_ONLN 2> /dev/null)
    test "$n" -gt 0 || n=1
    n=$(expr $n + 1)
    MAKEFLAGS="$MAKEFLAGS -j$n"
    export MAKEFLAGS
    ;;
esac

make
make install

# set -o pipefail is a bashism; this use of exec is the POSIX alternative
exec 3>&1
st=$(
  exec 4>&1 >&3
  { make check 2>&1 3>&- 4>&-; echo $? >&4; } | tee "$RESULTS"
)
exec 3>&-
test "$st" = 0

rm -f *.tar.gz
make dist

if [ -n "$AUTOBUILD_COUNTER" ]; then
  EXTRA_RELEASE=".auto$AUTOBUILD_COUNTER"
else
  NOW=`date +"%s"`
  EXTRA_RELEASE=".$USER$NOW"
fi

if [ -f /usr/bin/rpmbuild ]; then
  rpmbuild --nodeps \
     --define "extra_release $EXTRA_RELEASE" \
     --define "_sourcedir `pwd`" \
     -ba --clean netcf.spec
fi

if [ -x /usr/bin/i686-pc-mingw32-gcc ]; then
  make distclean

  PKG_CONFIG_PATH="$AUTOBUILD_INSTALL_ROOT/i686-pc-mingw32/sys-root/mingw/lib/pkgconfig" \
  CC="i686-pc-mingw32-gcc" \
  ../configure \
    --build=$(uname -m)-pc-linux \
    --host=i686-pc-mingw32 \
    --prefix="$AUTOBUILD_INSTALL_ROOT/i686-pc-mingw32/sys-root/mingw" \
    --enable-compile-warnings=error

  make
  make install

  #set -o pipefail
  #make check 2>&1 | tee "$RESULTS"

  if [ -f /usr/bin/rpmbuild ]; then
    rpmbuild --nodeps \
       --define "extra_release $EXTRA_RELEASE" \
       --define "_sourcedir `pwd`" \
       -ba --clean mingw32-netcf.spec
  fi
fi
