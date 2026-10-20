#!/bin/sh
# Run the same gates as CI before committing: the guard and package-check tests, the recomp's runtime compile check
# (macOS, no game files needed) and unit tests, the proprietary-content guard on changed files.
#   scripts/check.sh            all of it
#   scripts/check.sh --no-recomp  skip the recomp compile check
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
recomp=true
for arg in "$@"; do
  case "$arg" in
    --no-recomp) recomp=false ;;
    *) echo "usage: scripts/check.sh [--no-recomp]" >&2; exit 2 ;;
  esac
done
cd "$root"
python3 scripts/test_check_no_proprietary.py -q
python3 scripts/test_check_package.py -q
if $recomp; then make -s -C "$root/recomp" test-buildsync; fi
if $recomp && [ "$(uname -s)" = Darwin ]; then
  make -s -C "$root/recomp" GAME=sm35 -j8 runtime-objs
  make -s -C "$root/recomp" test-framegen test-ui
fi
files=$(git ls-files -m -o --exclude-standard; git diff --cached --name-only --diff-filter=AM)
if [ -n "$files" ]; then python3 scripts/check_no_proprietary.py $files; fi
echo "check: all gates passed"
