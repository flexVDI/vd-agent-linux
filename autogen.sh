#!/bin/sh

autoreconf -fi
if test -z "$NOCONFIGURE"; then
  exec ./configure "$@"
fi
