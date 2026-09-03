#!/usr/bin/env sh
# src/tetris.c is plain C with no ESP-IDF dependency beyond esp_random(), so
# the rules can be tested on the host in a second instead of on the board.
#
#   tools/host-test/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

cc -std=c11 -Wall -Wextra -Wno-unused-parameter -O1 -g \
   -I"$root/include" -I"$here" \
   "$here/test_tetris.c" "$root/src/tetris.c" -o "$out/test_tetris"
"$out/test_tetris"
