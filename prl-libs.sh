#!/bin/sh
# Print the link line for a static Qt from its .prl files.
#
#   prl-libs.sh PREFIX LIBDIR PLUGINDIR FILE.prl...
#
# Static Qt ships no pkg-config files. Each .prl lists, in
# QMAKE_PRL_LIBS, everything its library needs: other Qt archives,
# resource objects and system libraries, with $$[QT_INSTALL_*]
# placeholders. Archives and -l flags may repeat, the linker wants that
# for static order. A resource object may not: linked twice it is a
# duplicate symbol, so each .obj is kept once, at its first place.
set -e
prefix=$1 libdir=$2 plugindir=$3
shift 3
sed -n 's/^QMAKE_PRL_LIBS = //p' "$@" |
    sed -e "s#\$\$\[QT_INSTALL_PREFIX\]#$prefix#g" \
        -e "s#\$\$\[QT_INSTALL_LIBS\]#$libdir#g" \
        -e "s#\$\$\[QT_INSTALL_PLUGINS\]#$plugindir#g" |
    tr ' ' '\n' |
    awk 'NF && !(/\.obj$/ && seen[$0]++)' |
    tr '\n' ' '
echo
