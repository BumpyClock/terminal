#!/bin/sh
# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

root=$1
shell=$2
shift 2

case "$shell" in
*/bash|bash)
    helper="$root/bash/windows-terminal.bash"
    if [ ! -r "$helper" ]; then
        exec "$shell" "$@"
    fi

    if [ "${WT_SHELL_INTEGRATION_BASH_LOGIN:-0}" = 1 ]; then
        original_prompt_command=${PROMPT_COMMAND-}
        bootstrap_command='__wt_status=$?;if [ "$PROMPT_COMMAND" = "$WT_SHELL_INTEGRATION_EXPECTED_PROMPT_COMMAND" ];then __wt_root="$WT_SHELL_INTEGRATION_ROOT";__wt_bootstrap="$__wt_root/bash/windows-terminal-bootstrap.bash";[ ! -r "$__wt_bootstrap" ]||. "$__wt_bootstrap";fi'
        expected_prompt_command=$bootstrap_command
        if [ -n "$original_prompt_command" ]; then
            expected_prompt_command="$expected_prompt_command;$original_prompt_command"
        fi
        export WT_SHELL_INTEGRATION_ROOT="$root"
        export WT_SHELL_INTEGRATION_ORIGINAL_PROMPT_COMMAND="$original_prompt_command"
        export WT_SHELL_INTEGRATION_EXPECTED_PROMPT_COMMAND="$expected_prompt_command"
        export PROMPT_COMMAND="$expected_prompt_command"
        exec "$shell" "$@"
    fi

    exec "$shell" --noprofile --rcfile "$helper" -i
    ;;
*/zsh|zsh)
    if [ ! -r "$root/zsh/.zshenv" ]; then
        exec "$shell" "$@"
    fi
    export WT_SHELL_INTEGRATION_POSIX_ROOT="$root"
    export WT_SHELL_INTEGRATION_USER_ZDOTDIR="${ZDOTDIR:-$HOME}"
    export ZDOTDIR="$root/zsh"
    exec "$shell" "$@"
    ;;
*)
    exec "$shell" "$@"
    ;;
esac
