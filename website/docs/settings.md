---
title: Settings
---

# Settings

**Tools > Settings...** (**Cmd+,**, or Settings in the macOS application menu) opens the Settings screen. Changes apply at once and are kept for every workspace. **Esc** or **Close** leaves it.

## General

- **Where Lume keeps things**: the folder that holds preferences, connections (never passwords: those stay in the operating system's secret store) and settings. Files mentioned elsewhere in these docs (`keymap.txt`, `semantic.json`, `git.txt`...) live there.
- **Workspace**: workspaces are chosen from the toolbar's Workspace menu. See [Workspaces](workspace/workspaces.md).

## Appearance

- **Theme**: Lume Dark (the default), Lume Light, Match the system (Lume Dark or Lume Light, following your operating system, also when it changes) and several other colour schemes, each shown with a swatch. The Command Palette has the same choices under "Theme:". Reports and the files you export from an ER diagram keep their own colours.
- **Editor font size**: 90 %, 100 %, 110 %, 125 %, 150 % (also **Cmd+=**, **Cmd+-**, **Cmd+0**).
- **Compact panel headers**.
- **Reduce motion (no smooth scrolling or animation)**.

## Editor

The **SQL formatter** rules, used by Format in the object editor and when the assistant's SQL is shown formatted:

- **Keywords**: UPPER, lower, As written.
- **Indent**: 2, 4 or 8 spaces; **Indent with tabs**.
- **One clause per line (SELECT / FROM / WHERE ...)**.

## Connections

- **Reaching the database server**: 10 s (default), 30 s, 1 min or 2 min. PostgreSQL, MySQL / MariaDB, SQL Server and Oracle (as an EZConnect `connect_timeout`, with Instant Client 19 or newer; a TNS alias keeps its own `sqlnet.ora` setting). Applies to the next connect.
- **Opening an SSH tunnel**: 20 s (default), 1 min or 2 min.

- **Completion > List every object I can see, in a second session** (on by default): see [Every object you can see](sql/editor.md#every-object-you-can-see). Off, completion proposes your own schema's tables only, and no second session is opened.

Connections themselves are edited from the Explorer (**Edit connection**).

## Query Execution

- **Timeout**: each statement run from the editor. **No limit** (default), 30 s, 5 min or 30 min. A statement that runs longer is cancelled, as with Stop; the Output says it was the timeout.
- **Rows per fetch**: the first batch of a result (any number, or 100 / 500 / 1,000 / 5,000).
- **Memory for results**: 64 MB, 256 MB, 1 GB or 4 GB for all result tabs together; older tabs give their rows back first.
- **Continue after an error**: otherwise a script stops at the first failing statement.
- **Protection**: a summary of the rules that always apply per connection ([Environments and read-only](connections/environments.md)).

## Results & Format

- **Numbers and dates in the grid** (display only): **Thousands separator**, **Decimals** (As stored, 0, 2, 4), **Dates** (ISO, DMY, MDY), **Time zone of timestamps** (As stored, UTC, Local).
- **Execution Output**: **Open the Output when a statement fails**; how many entries **Query History** keeps (500 by default).

## Schema Explorer

- **Reading the catalog** (columns, indexes, properties, search): 5 s (default), 15 s, 30 s or 2 min.
- **Listing a schema's tables**: 2 min, 10 min (default), 30 min or 1 h. The schema shows the seconds elapsed and a **Cancel** while it waits.

The tree's own options (what is shown, page size, pinned schemas) are per connection, in the Explorer's **Options** menu.

## Reports

- Language and number / date format of **new** reports: Português (pt-BR) or English (en-US). Each report keeps its own.
- **Timeout**: each report query, 30 s (default), 2 min or 10 min.

## Source Control

- **Pull strategy**: Fast-forward only, Merge, Rebase.
- **Timeouts**: fetch, pull, push and other operations on the remote, 2 min (default), 10 min or 30 min; **Clone**, 30 min (default), 1 h or 2 h.
- **Identity**: the name and e-mail your commits use, read from your Git configuration (Lume never changes it).

## Lume Intelligence

Provider and model, fast model, streamed answers and this session's tokens, what each connection allows (with **Forget**) and the glossary. See [Set up a provider](intelligence/setup.md#settings).

## Privacy

- **Share anonymous usage statistics**: off unless you turn it on.
- **Crash reports**: **Ask each time** (default), **Always send** or **Never send**.
- **Check for updates automatically**: on by default.
- **Exactly what is sent when Lume starts**: the literal text, shown on the screen.

See [Privacy](privacy.md) for what each one sends.

## Not in Settings yet

A light theme, a confirmation for `UPDATE` / `DELETE` without `WHERE`, editor tab width and word wrap, completion options, export defaults, a configurable NULL display, a shortcuts screen ([edit `keymap.txt`](workspace/navigation.md#custom-shortcuts) instead), reopening the last session at startup, and a default transaction mode per connection.
