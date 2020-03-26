#!/bin/sh
set -e

nix-shell -p gcc --command 'gcc -shared -s -Wall -Werror hugepages_preload.c -ldl -o hugepages_preload.so'
