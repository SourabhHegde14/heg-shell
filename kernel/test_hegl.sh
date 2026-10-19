#!/bin/bash
# Differential test: heg-lite vs bash on the features heg-lite supports.
H=${1:-./kernel/hegl}; d=$(mktemp -d); cd "$d"; fail=0; n=0
printf 'b\na\nc\n' > f.txt
check() { n=$((n+1)); exp=$(bash -c "$1" 2>&1); got=$(printf '%s\n' "$1" | "$H" 2>&1)
  if [ "$exp" == "$got" ]; then echo "  ok    $1"; else echo "  FAIL  $1"; echo "    bash: $exp"; echo "    hegl: $got"; fail=1; fi; }
check 'echo hello'
check 'echo a b   c'
check 'echo "a  b" '"'x  y'"
check 'cat f.txt | sort'
check 'cat < f.txt | sort | head -n 2'
check 'sort < f.txt > o.txt; cat o.txt'
check 'echo one > o.txt; echo two >> o.txt; cat o.txt'
check 'echo a; echo b; echo c'
check 'cat f.txt | sort | uniq | wc -l'
check 'true | cat'
check 'echo x | cat | cat | cat | tr x y'
check 'cd /tmp; pwd'
rm -rf "$d"; echo; [ $fail = 0 ] && echo "heg-lite: all $n checks passed" || { echo "FAILED"; exit 1; }
