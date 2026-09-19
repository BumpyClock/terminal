# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

PROMPT_COMMAND="${WT_SHELL_INTEGRATION_ORIGINAL_PROMPT_COMMAND:-}"
export WT_SHELL_INTEGRATION_STARTUP_COMPLETE=1

. "$__wt_root/bash/windows-terminal.bash"
if declare -F __wt_initialize_prompt >/dev/null; then
    __wt_initialize_prompt "${__wt_status:-0}"
fi

unset WT_SHELL_INTEGRATION_STARTUP_COMPLETE
unset WT_SHELL_INTEGRATION_ORIGINAL_PROMPT_COMMAND
unset WT_SHELL_INTEGRATION_EXPECTED_PROMPT_COMMAND
unset __wt_bootstrap
unset __wt_root
__wt_restore_status "${__wt_status:-0}"
