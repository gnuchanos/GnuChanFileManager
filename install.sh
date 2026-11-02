#!/bin/sh
set -eu

prefix=${PREFIX:-/usr/local}
make
make install PREFIX="$prefix" DESTDIR="${DESTDIR:-}"
