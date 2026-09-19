# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

ZDOTDIR="$WT_SHELL_INTEGRATION_USER_ZDOTDIR"
[[ -r "$ZDOTDIR/.zprofile" ]] && source "$ZDOTDIR/.zprofile"
WT_SHELL_INTEGRATION_USER_ZDOTDIR="${ZDOTDIR:-$HOME}"
ZDOTDIR="$WT_SHELL_INTEGRATION_PROXY_ZDOTDIR"
