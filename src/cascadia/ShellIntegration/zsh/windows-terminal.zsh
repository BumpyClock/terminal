# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

if [[ -n "${WT_SHELL_INTEGRATION_ACTIVE:-}" ]]; then
    return
fi
export WT_SHELL_INTEGRATION_ACTIVE=1

autoload -Uz add-zsh-hook

__wt_escape_value() {
    emulate -L zsh
    local LC_ALL=C value="$1" output="" character token
    local -i index code
    for ((index = 1; index <= ${#value}; ++index)); do
        character="${value[index]}"
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
    print -rn -- "$output"
}

typeset -g __wt_first_prompt=0
typeset -g __wt_in_command=0
typeset -g __wt_original_prompt="$PROMPT"
typeset -g __wt_custom_prompt=""

__wt_wrap_prompt() {
    if [[ "$PROMPT" != "$__wt_custom_prompt" ]]; then
        __wt_original_prompt="$PROMPT"
        __wt_custom_prompt="${__wt_original_prompt}%{\e]633;B\a%}"
        PROMPT="$__wt_custom_prompt"
    fi
}

__wt_precmd() {
    local status="$?"
    if [[ "$__wt_first_prompt" == 1 && "$__wt_in_command" == 1 ]]; then
        printf '\e]633;D;%s\a' "$status"
    fi
    __wt_in_command=0
    printf '\e]633;P;Cwd=%s\a' "$(__wt_escape_value "$PWD")"
    printf '\e]633;A\a'
    __wt_wrap_prompt
    __wt_first_prompt=1
    return "$status"
}

__wt_preexec() {
    __wt_in_command=1
    printf '\e]633;C\a'
}

add-zsh-hook precmd __wt_precmd
add-zsh-hook preexec __wt_preexec

unset WT_SHELL_INTEGRATION_ROOT
unset WT_SHELL_INTEGRATION_POSIX_ROOT
unset WT_SHELL_INTEGRATION_USER_ZDOTDIR
unset WT_SHELL_INTEGRATION_PROXY_ZDOTDIR
unset WT_SHELL_INTEGRATION_ZSHENV_LOADED
