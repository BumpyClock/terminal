# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

ZDOTDIR="$WT_SHELL_INTEGRATION_USER_ZDOTDIR"
[[ -r "$ZDOTDIR/.zshrc" ]] && source "$ZDOTDIR/.zshrc"
WT_SHELL_INTEGRATION_USER_ZDOTDIR="${ZDOTDIR:-$HOME}"
source "$WT_SHELL_INTEGRATION_POSIX_ROOT/zsh/windows-terminal.zsh"
ZDOTDIR="$WT_SHELL_INTEGRATION_USER_ZDOTDIR"
