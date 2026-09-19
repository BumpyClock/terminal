# Git status bar

The status bar shows Git information for the focused terminal pane. It is enabled
by default and reserves a row below the terminal, including when there is no Git
information to display. Changing directories or switching tabs does not add or
remove that row.

## Information shown

The left-aligned summary groups the branch, available ahead/behind commit
counts, net tracked-line additions and deletions, and icons for staged,
unstaged, untracked, and conflicted changes. It covers the entire containing Git
worktree, not just the current subdirectory.

Line totals compare tracked content against HEAD, or the empty tree before the
first commit. They are not the sum of staged and unstaged diffs: editing a staged
line again does not count it twice. A staged change that is undone in the working
file can therefore have zero net line changes while both state icons remain.

Untracked files do not contribute to added-line totals. Binary changes have no
numeric line counts, but remain indicated by state icons. Unresolved conflicts
show the conflict state without fabricated aggregate totals. Entering a nested
repository or submodule selects that repository's own worktree.

Detached HEAD and branches without commits have distinct labels. The tooltip
and accessible description retain exact line totals and identify the state
icons.
Ahead/behind counts use existing local upstream references. Terminal does not
fetch, stage, commit, switch branches, or otherwise change the repository. Diff
collection does not invoke external diff or text-conversion helpers.

If the directory is unknown, there is no repository, Git is unavailable, or a
query fails, the Git item is empty. It does not retain a different pane's or
directory's repository. Directory observations come from shell prompt
integration, not continuous inspection of a running command. While a command
runs, eligible foreground polling uses the last shell-reported filesystem
context. The bar and its tooltip do not display freshness or observation times.

## Supported shell launches

Automatic integration applies to recognized direct interactive launches:

- PowerShell 7 and Windows PowerShell 5.1.
- Git for Windows Bash from a recognized Git for Windows installation.
- Bash and Zsh in a named WSL2 distribution.

For WSL, Terminal retains the distribution and any explicitly selected user.
It runs Git inside that environment instead of using Windows Git against a
Linux directory. A named distribution's default-shell launch is supported when
bounded discovery identifies Bash or Zsh. A bare `wsl.exe` invocation without a
known distribution is not automatically integrated.

Command payloads, unsupported wrappers, remote SSH sessions, containers, and
unrecognized shells are left unchanged. There is no universal guarantee for
nested shells or custom startup code.

Terminal installs its reporting hooks for the launched session without editing
user profiles, configuration, or registry settings. It does not weaken
PowerShell execution policy. If policy prevents integration, or custom startup
code replaces the initial Bash prompt hook, the shell remains usable and the
Git item stays empty.

Git for Windows must be available for local Windows repositories. WSL
repositories need Git inside the selected distribution. The feature does not
install Git or WSL. Git must support `--no-lazy-fetch` (available in Git 2.45)
so missing objects cannot cause a background network fetch.

## Visibility and refresh

Use **Settings > Appearance > Show status bar**, or set the global JSON property:

```json
"showStatusBar": false
```

Hiding the bar stops its Git scheduling. New sessions launched while it is
disabled do not receive integration solely for this feature. Hooks already
loaded in a running shell remain until that shell exits. Reopen an unintegrated
session after enabling the setting; Terminal does not type a bootstrap command
into an existing shell.

Focus mode hides the bar. Fullscreen alone retains it, and zoomed panes still
use the single window-level bar. Non-terminal panes have no Git item.

Status refreshes after prompt return, context changes, and app activation, with
periodic checks every five seconds for the eligible foreground pane. Inactive
tabs and background windows are not polled. Queries are bounded and slow or
failed queries use backoff rather than blocking the terminal.

## Development boundaries

`inc/ShellContext.h` contains only environment identity and shell reports; the
terminal, adapter, and connection layers do not depend on Git or presentation
models. Launch policy belongs to `TerminalConnection/ShellIntegrationLaunch.h`.

The app's `GitStatus.h` describes a query by environment and directory, and a
result by repository data or a distinct failure. The provider runs one
asynchronous query; all Git subprocesses share the same deadline, output budget,
read-only options, and failure handling. Change categories are presence flags,
independent of net line totals. There are no file-count or freshness fields.

`TerminalPage` binds the exact focused terminal pane. `StatusBarCoordinator`
alone owns scheduling and request lifetime. Replacing context or losing query
eligibility cancels the current request, whose slot remains occupied until
completion. Its completion ID rejects duplicates; its cancellation token rejects
obsolete results, including a rebind to the same directory. Git data does not
carry copies of connection IDs, shell sequences, or scheduler generations.
The native control only renders supplied snapshots.

Bootstrap assets are listed explicitly in `ShellIntegration.build.items`,
including Zsh dotfiles. Both packaged and unpackaged layouts must deploy them
under `ShellIntegration` next to the application.

Changes to these boundaries should retain coverage for shell startup behavior,
real Git repository fixtures, stale-result rejection, polling suppression,
stable layout, settings, and accessibility. WSL execution and Linux-child
cancellation require a WSL-enabled test environment; mocked launch arguments
are not substitutes for those runtime checks.
