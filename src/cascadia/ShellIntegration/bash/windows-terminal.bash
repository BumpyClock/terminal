# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

if [[ -n "${WT_SHELL_INTEGRATION_ACTIVE:-}" ]]; then
    return
fi
export WT_SHELL_INTEGRATION_ACTIVE=1

__wt_login="${WT_SHELL_INTEGRATION_BASH_LOGIN:-0}"
__wt_skip_profile="${WT_SHELL_INTEGRATION_BASH_SKIP_PROFILE:-0}"
__wt_skip_rc="${WT_SHELL_INTEGRATION_BASH_SKIP_RC:-0}"
__wt_startup_complete="${WT_SHELL_INTEGRATION_STARTUP_COMPLETE:-0}"

if [[ "$__wt_startup_complete" != 1 && "$__wt_login" == 1 ]]; then
    if [[ "$__wt_skip_profile" != 1 ]]; then
        [[ -r /etc/profile ]] && . /etc/profile
        if [[ -r ~/.bash_profile ]]; then
            . ~/.bash_profile
        elif [[ -r ~/.bash_login ]]; then
            . ~/.bash_login
        elif [[ -r ~/.profile ]]; then
            . ~/.profile
        fi
    fi
elif [[ "$__wt_startup_complete" != 1 && "$__wt_skip_rc" != 1 && -r ~/.bashrc ]]; then
    . ~/.bashrc
fi

if [[ -z "${WT_SHELL_INTEGRATION_ACTIVE:-}" ]]; then
    return
fi

__wt_get_trap() {
    local -a terms
    eval "terms=( $(trap -p "${1:-DEBUG}") )"
    printf '%s' "${terms[2]:-}"
}

__wt_escape_value() {
    local LC_ALL=C value="$1" output="" character token
    local -i index code
    for ((index = 0; index < ${#value}; ++index)); do
        character="${value:index:1}"
        printf -v code '%d' "'$character"
        if ((code <= 31)); then
            printf -v token '\\x%02x' "$code"
        elif ((code == 92)); then
            token='\\'
        elif ((code == 59)); then
            token='\x3b'
        else
            token="$character"
        fi
        output+="$token"
    done
    printf '%s' "$output"
}

__wt_restore_status() {
    return "$1"
}

__wt_report_cwd() {
    local directory="$PWD"
    case "$(uname -s)" in
    CYGWIN*|MINGW*|MSYS*)
        directory="$(cygpath -m -- "$PWD" 2>/dev/null)" || directory=""
        ;;
    esac
    printf '\e]633;P;Cwd=%s\a' "$(__wt_escape_value "$directory")"
}

__wt_first_prompt=0
__wt_in_command=0
__wt_in_prompt_command=0
__wt_original_ps1="$PS1"
__wt_custom_ps1=""

if declare -p PROMPT_COMMAND 2>/dev/null | grep -q 'declare \-a'; then
    __wt_original_prompt_commands=("${PROMPT_COMMAND[@]}")
elif [[ -n "${PROMPT_COMMAND:-}" ]]; then
    __wt_original_prompt_commands=("$PROMPT_COMMAND")
else
    __wt_original_prompt_commands=()
fi

__wt_wrap_prompt() {
    if [[ "$PS1" != "$__wt_custom_ps1" ]]; then
        __wt_original_ps1="$PS1"
        __wt_custom_ps1="${__wt_original_ps1}\[\e]633;B\a\]"
        PS1="$__wt_custom_ps1"
    fi
}

__wt_prompt_command() {
    local status="$?"
    local command
    __wt_in_prompt_command=1

    for command in "${__wt_original_prompt_commands[@]}"; do
        __wt_restore_status "$status"
        eval "${command:-}"
    done

    if [[ "$__wt_first_prompt" == 1 && "$__wt_in_command" == 1 ]]; then
        printf '\e]633;D;%s\a' "$status"
    fi
    __wt_in_command=0
    __wt_report_cwd
    printf '\e]633;A\a'
    __wt_wrap_prompt
    __wt_first_prompt=1
    __wt_in_prompt_command=0
    __wt_restore_status "$status"
}

__wt_initialize_prompt() {
    local status="$1"
    __wt_in_prompt_command=1
    __wt_in_command=0
    __wt_report_cwd
    printf '\e]633;A\a'
    __wt_wrap_prompt
    __wt_first_prompt=1
    __wt_in_prompt_command=0
    __wt_restore_status "$status"
}

__wt_original_debug_trap="$(__wt_get_trap DEBUG)"
__wt_debug_trap() {
    local status="$?"
    local command="$BASH_COMMAND"
    if [[ "$__wt_first_prompt" == 1 &&
          "$__wt_in_prompt_command" == 0 &&
          "$__wt_in_command" == 0 &&
          "$command" != __wt_* ]]; then
        __wt_in_command=1
        printf '\e]633;C\a'
    fi
    if [[ -n "$__wt_original_debug_trap" ]]; then
        __wt_restore_status "$status"
        eval "$__wt_original_debug_trap"
    fi
    __wt_restore_status "$status"
}

PROMPT_COMMAND=__wt_prompt_command
trap '__wt_debug_trap' DEBUG

if [[ "$__wt_login" == 1 && "$__wt_startup_complete" != 1 ]]; then
    __wt_original_exit_trap="$(__wt_get_trap EXIT)"
    __wt_logout() {
        local status="$?"
        [[ -r ~/.bash_logout ]] && . ~/.bash_logout
        [[ -r /etc/bash.bash_logout ]] && . /etc/bash.bash_logout
        if [[ -n "$__wt_original_exit_trap" ]]; then
            __wt_restore_status "$status"
            eval "$__wt_original_exit_trap"
        fi
        __wt_restore_status "$status"
    }
    trap '__wt_logout' EXIT
fi

unset WT_SHELL_INTEGRATION_ROOT
unset WT_SHELL_INTEGRATION_BASH_LOGIN
unset WT_SHELL_INTEGRATION_BASH_SKIP_PROFILE
unset WT_SHELL_INTEGRATION_BASH_SKIP_RC
