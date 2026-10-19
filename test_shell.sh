#!/usr/bin/env bash
# Differential tests for heg: every snippet runs under bash and under heg in a
# fresh empty directory, and stdout + exit status must match exactly.
# (stderr is discarded: error wording legitimately differs between shells.)
#
# usage: ./test_shell.sh [path-to-heg]

HEG="$(realpath "${1:-./heg}")"
pass=0
fail=0

run_in_fresh_dir() {
    local shell="$1" cmd="$2" d
    d=$(mktemp -d)
    (cd "$d" && timeout 10 "$shell" -c "$cmd" 2>/dev/null; echo "rc=$?")
    rm -rf "$d"
}

check() {
    local cmd="$1" exp got
    exp=$(run_in_fresh_dir bash "$cmd")
    got=$(run_in_fresh_dir "$HEG" "$cmd")
    if [[ "$exp" == "$got" ]]; then
        pass=$((pass + 1))
        printf '  ok    %s\n' "$cmd"
    else
        fail=$((fail + 1))
        printf '  FAIL  %s\n        bash: %s\n        heg : %s\n' "$cmd" "${exp//$'\n'/\\n}" "${got//$'\n'/\\n}"
    fi
}

expect_eq() { # name expected actual
    if [[ "$2" == "$3" ]]; then
        pass=$((pass + 1))
        printf '  ok    %s\n' "$1"
    else
        fail=$((fail + 1))
        printf '  FAIL  %s\n        expected: %q\n        actual  : %q\n' "$1" "$2" "$3"
    fi
}

echo "== basics =="
check 'echo hello world'
check 'echo "a   b" '"'"'c   d'"'"' e\ f'
check 'nosuchcmd_xyz; echo $?'
check 'exit 3'
check 'true; false; echo $?'

echo "== pipes =="
check 'echo hi | tr a-z A-Z'
check 'printf "b\na\nc\n" | sort | head -2'
check 'seq 1 1000 | grep 1 | sort -r | head -n 3 | tail -n 1'
check 'yes | head -3'
check 'cat /dev/null | cat; echo ok'
check 'echo a | echo b'
check 'touch x y z; ls | wc -l'

echo "== redirection =="
check 'echo hi > f; cat f'
check 'echo a > f; echo b >> f; cat f'
check 'echo hello > f; cat < f | tr a-z A-Z'
check 'ls /nonexistent_dir_xyz 2> err; wc -l < err'
check 'ls /nonexistent_dir_xyz > out 2>&1; wc -l < out'
check 'ls /nonexistent_dir_xyz 2>&1 >/dev/null | wc -l'
check 'echo to-err >&2 2> f; wc -c < f'
check 'cat < /nonexistent_file_xyz; echo $?'
check 'echo x > f1; echo y > f2; cat f1 f2'

echo "== lists =="
check 'true && echo yes'
check 'false || echo no'
check 'false && echo x || echo y'
check 'true || echo x && echo y'
check 'echo a; echo b; echo c'
check 'cd /nonexistent_dir_xyz; echo $?'

echo "== quoting and expansion =="
check 'echo "a;b && c | d"'
check 'echo a\;b'
check 'echo '"'"'$HOME'"'"
check 'echo $UNSET_VAR_XYZ x'
check 'echo "$UNSET_VAR_XYZ"x'
check 'export FOO=bar; echo $FOO ${FOO}baz "$FOO"'
check 'X=hello; echo $X world'
check 'echo ~ | grep -c "^$HOME$"'
check 'touch a.c b.c c.h; echo *.c'
check 'touch a.c b.c; echo "*.c" '"'"'*.c'"'"
check 'touch a1 a2 b1; echo a?'
check 'echo -n hi; echo there'
check 'echo -e "a\tb"'
check 'echo # this is a comment'

echo "== builtins =="
check 'cd /tmp && pwd'
check 'cd /tmp; cd /; cd -; pwd'
check 'cd /tmp && echo $PWD'
check 'cd /usr; echo $OLDPWD | wc -c'

echo "== substitution & arithmetic =="
check 'echo $(echo hi) `echo there`'
check 'x=$(echo a b c); echo "$x"; echo $x'
check 'echo $((2+3*4)) $(( (7-1)/2 )) $((10%3))'
check 'n=5; echo $((n*n))'
check 'echo "$(echo a; echo b)"'

echo "== parameter expansion =="
check 'unset U; echo ${U:-dflt} ${U-dflt}'
check 'V=abc; echo ${V:-x} ${#V}'
check 'unset U; echo ${U:=set}; echo $U'
check 'V=hello; echo ${V:+yes}'

echo "== control flow =="
check 'if true; then echo y; else echo n; fi'
check 'if false; then echo y; elif true; then echo mid; else echo n; fi'
check 'for i in 1 2 3; do echo $i; done'
check 'i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done'
check 'until [ -f f ]; do touch f; echo made; done'
check 'for i in a b c; do [ $i = b ] && continue; echo $i; done'
check 'for i in 1 2 3; do for j in x y; do [ $i = 2 ] && break 2; echo $i$j; done; done'

echo "== functions =="
check 'f() { echo "got $1 $2"; }; f a b'
check 'f() { return 3; }; f; echo $?'
check 'fact() { if [ $1 -le 1 ]; then echo 1; else echo $(( $1 * $(fact $(($1-1))) )); fi; }; fact 6'
check 'x=outer; f() { local x=inner; echo $x; }; f; echo $x'
check 'f() { echo $#; shift; echo $1; }; f a b c'

echo "== groups, subshells, heredocs =="
check '(cd /; pwd); pwd | grep -c .'
check '(exit 4); echo $?'
check '{ echo a; echo b; } | tr a-z A-Z'
check 'cat <<EOF
line one
value=$((1+1))
EOF'
check "cat <<'EOF'
no \$expansion here
EOF"
check 'echo out; echo err >&2' 
check 'echo both &>f; cat f' 

echo "== misc builtins =="
check 'echo a b | { read x y; echo "$y-$x"; }'
check 'test -z "" && echo empty'
check '[ 3 -gt 2 ] && [ -d / ] && echo ok'
check 'VAR=temp env | grep -c ^VAR=temp'
check 'eval "echo evald"'

echo "== background jobs =="
check 'sleep 0.1 & echo started; wait; echo done'
check 'echo bg & wait; echo after'

echo "== script / stdin / flags =="
d=$(mktemp -d)
printf 'echo from script\nexport V=42\necho $V\nexit 7\n' >"$d/s.heg"
out=$("$HEG" "$d/s.heg"); rc=$?
expect_eq "script file output" $'from script\n42' "$out"
expect_eq "script file exit code" "7" "$rc"
out=$(printf 'echo piped stdin\nfalse\necho $?\n' | "$HEG")
expect_eq "commands from stdin" $'piped stdin\n1' "$out"
printf 'echo sourced\n' >"$d/inc.heg"
out=$("$HEG" -c "source $d/inc.heg; echo back")
expect_eq "source builtin" $'sourced\nback' "$out"
rm -rf "$d"

echo "== errors =="
"$HEG" -c 'echo |' 2>/dev/null; expect_eq "syntax error: trailing pipe -> 2" "2" "$?"
"$HEG" -c 'echo "unterminated' 2>/dev/null; expect_eq "unterminated quote -> 2" "2" "$?"
"$HEG" -c '&& echo' 2>/dev/null; expect_eq "leading && -> 2" "2" "$?"
msg=$("$HEG" -c 'nosuchcmd_xyz' 2>&1 >/dev/null)
expect_eq "command not found message" "heg: nosuchcmd_xyz: command not found" "$msg"
"$HEG" -c 'nosuchcmd_xyz' 2>/dev/null; expect_eq "command not found -> 127" "127" "$?"
"$HEG" -c '/etc/passwd' 2>/dev/null; expect_eq "not executable -> 126" "126" "$?"

echo
echo "passed: $pass   failed: $fail"
[[ $fail -eq 0 ]]
