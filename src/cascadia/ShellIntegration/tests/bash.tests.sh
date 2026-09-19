#!/usr/bin/env bash
# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

set -euo pipefail

helper=$(cygpath -u "$1")
scratch=$2
rm -rf "$scratch"
mkdir -p "$scratch/nonlogin" "$scratch/login" "$scratch/true-login" "$scratch/replaced-hook"
trap 'rm -rf "$scratch"' EXIT

cat > "$scratch/nonlogin/.bashrc" <<'EOF'
printf 'bashrc\n' >> "$WT_TEST_LOG"
PS1='CUSTOM>'
PROMPT_COMMAND='printf "status=%s\n" "$?" >> "$WT_TEST_LOG"'
EOF
WT_TEST_LOG="$scratch/nonlogin.log" \
WT_TEST_OUTPUT="$scratch/nonlogin.output" \
HOME="$scratch/nonlogin" \
WT_SHELL_INTEGRATION_BASH_LOGIN=0 \
WT_SHELL_INTEGRATION_BASH_SKIP_PROFILE=0 \
WT_SHELL_INTEGRATION_BASH_SKIP_RC=0 \
bash --noprofile --rcfile "$helper" -i -c '
    trap - DEBUG
    [[ "$(__wt_escape_value "a;b\\c")" == "a\x3bb\\\\c" ]]
    false
    __wt_prompt_command > "$WT_TEST_OUTPUT" || true
    output=$(cat "$WT_TEST_OUTPUT")
    [[ "$output" == *$'"'"'\e]633;P;Cwd='"'"'* ]]
    [[ "$output" == *$'"'"'\e]633;A\a'"'"'* ]]
    [[ "$PS1" == *"633;B"* ]]
    [[ "$PS1" == CUSTOM\>* ]]
'
[[ "$(cat "$scratch/nonlogin.log")" == $'bashrc\nstatus=1' ]]

printf 'printf "profile\\n" >> "$WT_TEST_LOG"\n' > "$scratch/login/.bash_profile"
printf 'printf "logout\\n" >> "$WT_TEST_LOG"\n' > "$scratch/login/.bash_logout"
WT_TEST_LOG="$scratch/login.log" \
HOME="$scratch/login" \
WT_SHELL_INTEGRATION_BASH_LOGIN=1 \
WT_SHELL_INTEGRATION_BASH_SKIP_PROFILE=0 \
WT_SHELL_INTEGRATION_BASH_SKIP_RC=0 \
bash --noprofile --rcfile "$helper" -i -c 'trap - DEBUG; true'
[[ "$(cat "$scratch/login.log")" == $'profile\nlogout' ]]

root=$(dirname "$(dirname "$helper")")
windows_root=$(cygpath -w "$root")
bootstrap='__wt_status=$?;if [ "$PROMPT_COMMAND" = "$WT_SHELL_INTEGRATION_EXPECTED_PROMPT_COMMAND" ];then __wt_root=$(cygpath -u "$WT_SHELL_INTEGRATION_ROOT" 2>/dev/null);__wt_bootstrap="$__wt_root/bash/windows-terminal-bootstrap.bash";[ ! -r "$__wt_bootstrap" ]||. "$__wt_bootstrap";fi'
original_prompt_command='printf "prompt-status=%s\n" "$?" >> "$WT_TEST_LOG"'
expected_prompt_command="$bootstrap;$original_prompt_command"

cat > "$scratch/true-login/.bash_profile" <<'EOF'
printf 'profile\n' >> "$WT_TEST_LOG"
if shopt -q login_shell; then
    printf 'login=yes\n' >> "$WT_TEST_LOG"
else
    printf 'login=no\n' >> "$WT_TEST_LOG"
fi
printf 'argv0=%s\n' "$0" >> "$WT_TEST_LOG"
PS1='LOGIN>'
EOF
printf 'printf "logout\\n" >> "$WT_TEST_LOG"\n' > "$scratch/true-login/.bash_logout"

printf 'exit\n' |
WT_TEST_LOG="$scratch/true-login-baseline.log" \
HOME="$scratch/true-login" \
PROMPT_COMMAND="$original_prompt_command" \
bash --login -i > "$scratch/true-login-baseline.output" 2>&1

printf 'false\npwd\nexit\n' |
WT_TEST_LOG="$scratch/true-login.log" \
HOME="$scratch/true-login" \
WT_SHELL_INTEGRATION_ROOT="$windows_root" \
WT_SHELL_INTEGRATION_BASH_LOGIN=1 \
WT_SHELL_INTEGRATION_BASH_SKIP_PROFILE=0 \
WT_SHELL_INTEGRATION_BASH_SKIP_RC=0 \
WT_SHELL_INTEGRATION_ORIGINAL_PROMPT_COMMAND="$original_prompt_command" \
WT_SHELL_INTEGRATION_EXPECTED_PROMPT_COMMAND="$expected_prompt_command" \
PROMPT_COMMAND="$expected_prompt_command" \
bash --login -i > "$scratch/true-login.output" 2>&1

[[ "$(grep -c '^profile$' "$scratch/true-login.log")" == 1 ]]
grep -q '^login=yes$' "$scratch/true-login.log"
grep -q '^argv0=' "$scratch/true-login.log"
[[ "$(grep '^argv0=' "$scratch/true-login.log")" == "$(grep '^argv0=' "$scratch/true-login-baseline.log")" ]]
grep -q '^prompt-status=1$' "$scratch/true-login.log"
[[ "$(grep -c '^logout$' "$scratch/true-login.log")" == 1 ]]
[[ "$(grep -c '^profile$' "$scratch/true-login-baseline.log")" == 1 ]]
grep -q '^login=yes$' "$scratch/true-login-baseline.log"
[[ "$(grep -c '^logout$' "$scratch/true-login-baseline.log")" == 1 ]]
grep -q $'\e]633;P;Cwd=' "$scratch/true-login.output"
grep -q $'\e]633;A\a' "$scratch/true-login.output"
grep -q $'\e]633;B\a' "$scratch/true-login.output"
if grep -q 'windows-terminal-bootstrap' "$scratch/true-login.output"; then
    printf 'Login bootstrap leaked a textual payload.\n' >&2
    exit 1
fi

cat > "$scratch/replaced-hook/.bash_profile" <<'EOF'
printf 'profile\n' >> "$WT_TEST_LOG"
PROMPT_COMMAND='printf "custom-hook\n" >> "$WT_TEST_LOG"'
PS1='CUSTOM>'
EOF
printf 'exit\n' |
WT_TEST_LOG="$scratch/replaced-hook.log" \
HOME="$scratch/replaced-hook" \
WT_SHELL_INTEGRATION_ROOT="$windows_root" \
WT_SHELL_INTEGRATION_BASH_LOGIN=1 \
WT_SHELL_INTEGRATION_BASH_SKIP_PROFILE=0 \
WT_SHELL_INTEGRATION_BASH_SKIP_RC=0 \
WT_SHELL_INTEGRATION_ORIGINAL_PROMPT_COMMAND="$original_prompt_command" \
WT_SHELL_INTEGRATION_EXPECTED_PROMPT_COMMAND="$expected_prompt_command" \
PROMPT_COMMAND="$expected_prompt_command" \
bash --login -i > "$scratch/replaced-hook.output" 2>&1
grep -q '^custom-hook$' "$scratch/replaced-hook.log"
if grep -q $'\e]633;' "$scratch/replaced-hook.output"; then
    printf 'Replaced PROMPT_COMMAND unexpectedly enabled integration.\n' >&2
    exit 1
fi

mkdir -p "$scratch/fake"
cat > "$scratch/fake/bash" <<'EOF'
#!/usr/bin/env bash
printf 'args=%s\n' "$*" >> "$WT_TEST_LOG"
printf 'root=%s\n' "${WT_SHELL_INTEGRATION_ROOT:-}" >> "$WT_TEST_LOG"
printf 'prompt=%s\n' "${PROMPT_COMMAND:-}" >> "$WT_TEST_LOG"
EOF
cat > "$scratch/fake/zsh" <<'EOF'
#!/usr/bin/env bash
printf 'args=%s\n' "$*" >> "$WT_TEST_LOG"
printf 'zdotdir=%s\n' "${ZDOTDIR:-}" >> "$WT_TEST_LOG"
printf 'user-zdotdir=%s\n' "${WT_SHELL_INTEGRATION_USER_ZDOTDIR:-}" >> "$WT_TEST_LOG"
EOF
chmod +x "$scratch/fake/bash" "$scratch/fake/zsh"

WT_TEST_LOG="$scratch/wsl-login.log" \
WT_SHELL_INTEGRATION_BASH_LOGIN=1 \
PROMPT_COMMAND='printf inherited' \
sh "$root/bash/windows-terminal-wsl.sh" "$root" "$scratch/fake/bash" -l
grep -q '^args=-l$' "$scratch/wsl-login.log"
grep -q "^root=$root$" "$scratch/wsl-login.log"
grep -q 'windows-terminal-bootstrap.bash' "$scratch/wsl-login.log"

WT_TEST_LOG="$scratch/wsl-nonlogin.log" \
WT_SHELL_INTEGRATION_BASH_LOGIN=0 \
sh "$root/bash/windows-terminal-wsl.sh" "$root" "$scratch/fake/bash"
grep -q -- "--noprofile --rcfile $root/bash/windows-terminal.bash -i" "$scratch/wsl-nonlogin.log"

WT_TEST_LOG="$scratch/wsl-zsh.log" \
ZDOTDIR="$scratch/user-zdotdir" \
sh "$root/bash/windows-terminal-wsl.sh" "$root" "$scratch/fake/zsh" -l
grep -q '^args=-l$' "$scratch/wsl-zsh.log"
grep -q "^zdotdir=$root/zsh$" "$scratch/wsl-zsh.log"
grep -q "^user-zdotdir=$scratch/user-zdotdir$" "$scratch/wsl-zsh.log"

printf 'Bash shell integration checks passed.\n'
