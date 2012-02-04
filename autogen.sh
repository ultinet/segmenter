#!/bin/sh

${LIBTOOLIZE:-libtoolize} -c -f
${ACLOCAL:-aclocal} -I m4
${AUTOMAKE:-automake} -c -a
${AUTOHEADER:-autoheader}
${AUTOCONF:-autoconf}
