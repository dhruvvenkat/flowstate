# flowstate

flowstate is a terminal-first code editor with AI-assisted explain, fix, refactor, and build-error workflows. AI suggestions are shown in a scratch buffer first, and code changes move into a reviewable patch preview before you apply them.

## Build

```bash
cmake -S . -B build
cmake --build build
```

Run the test suite:

```bash
ctest --test-dir build --output-on-failure
```

## Run

Basic usage:

```bash
./build/flowstate <file>
```

Choose an AI provider:

```bash
./build/flowstate <file> --no-ai
./build/flowstate <file> --ai openai
./build/flowstate <file> --ai codex
```

AI is disabled by default. You can also pass `--ai` with no value to keep AI disabled for
the session.

Attach a build command so `Ctrl+T` and `:build` work:

```bash
./build/flowstate src/main.cpp --build "cmake --build build"
```

You can also set the default provider with:

```bash
export FLOWSTATE_AI_MODE=codex
```

## Core Workflow

1. Open a file.
2. Move the cursor with the arrow keys, `Home`, `End`, `PageUp`, and `PageDown`.
3. Hold `Shift` while moving with the arrow keys, `Home`, or `End` to select text.
4. Press `Ctrl+E` to explain the current line or selection.
5. Press `Ctrl+R` to ask for a fix patch.
6. Review the AI scratch buffer or patch preview.

## Important Shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl+S` | Save the current file |
| `Ctrl+Q` | Quit. If the file is dirty, press it twice |
| `Ctrl+/` | Toggle line comments for the selected lines or current line |
| Mouse wheel | Scroll the active buffer |
| `Shift+Arrow` | Extend the current selection by character or line |
| `Shift+Home` / `Shift+End` | Extend the current selection to the start or end of the line |
| `Tab` | Insert spaces to the next 4-space indentation stop |
| `Ctrl+C` | Copy the current selection, or the current line if nothing is selected |
| `Ctrl+X` | Cut the current selection, or the current line if nothing is selected |
| `Ctrl+V` | Paste the internal editor clipboard at the cursor, or replace the current selection |
| `Ctrl+Z` | Undo the last file change |
| `Ctrl+Y` | Redo the last undone file change |
| `Ctrl+F` | Open the find prompt for the current file |
| `Ctrl+O` | Open the file picker |
| `Ctrl+E` | Explain the current line or selection |
| `Ctrl+R` | Generate a fix patch |
| `Ctrl+T` | Run the configured build command |
| `Esc` | Leave AI scratch, patch preview, or build output and return to the file buffer |
| `Alt+E` | Reopen the AI scratch buffer |
| `Alt+P` | Reopen patch preview if a patch is ready |
| `Alt+A` | Accept the current hunk in patch preview |
| `Alt+R` | Reject the current hunk in patch preview |
| `Alt+C` | Enter command mode |
| `Alt+D` | Toggle previous-line peek when the cursor is on a blue or red Git gutter marker |
| `Alt+I` | Request C++ IntelliSense completions with clangd |

Notes:

- Only one AI request runs at a time.
- Pressing `Esc` while AI is still running sends you back to the file buffer and leaves the request running in the background.
- `Enter` copies the current line indentation. `Backspace` inside leading indentation moves back one 4-space indentation stop.
- In the file picker, type to filter, use arrow keys to select, press `Enter` to open, or `Esc` to cancel.
- Cut, copy, and paste currently use an internal flowstate clipboard rather than the system clipboard.

## C++ IntelliSense

flowstate can request C/C++/header completions from `clangd`.

Requirements:

- `clangd` must be installed and available on `PATH`, or `FLOWSTATE_CLANGD_PATH` must point to it.
- Project-aware completions work best when CMake writes `compile_commands.json`.
- `--cpp-standard <standard>` or `FLOWSTATE_CPP_STANDARD=<standard>` sets clangd fallback parsing for C++ files without compile commands.

Completions open automatically while typing C++ identifiers and after `.`, `->`, and `::`. `Alt+I` can still request completions manually. Use arrow keys to select, `Enter` or `Tab` to accept, and `Esc` to cancel.

## Command Mode

Press `Alt+C` to open command mode, type a command, then press `Enter`.

| Command | Action |
| --- | --- |
| `:open <file>` | Open another file. If it does not exist, flowstate starts with an empty buffer for that path |
| `:write` | Save the current file |
| `:quit` | Quit. If the file is dirty, run it twice |
| `:build` | Run the configured build command |
| `:find <text>` | Find text in the current file, wrapping from the cursor |
| `:goto <line>` | Jump to a 1-based line number |
| `:ai explain` | Explain the current line or selection |
| `:ai fix` | Generate a fix patch |
| `:ai refactor` | Generate a refactor patch |
| `:ai error` | Explain the latest build failure |
| `:patch accept` | Accept the current patch hunk |
| `:patch reject` | Reject the current patch hunk |
| `:patch accept-all` | Accept all pending hunks |
| `:patch reject-all` | Reject all pending hunks |

Press `Esc` in command mode to cancel it.

## AI Views

### AI Scratch

The AI scratch buffer is where streamed AI output appears first.

- While AI is thinking, the top line shows the request label and a rotating spinner.
- For explain and error requests, the final text stays in the scratch buffer.
- For fix and refactor requests, flowstate attempts to extract a unified diff when the response completes.

### Patch Preview

If AI returns a valid patch for the current file, flowstate switches to patch preview.

- Use `Alt+A` and `Alt+R` to accept or reject the current hunk.
- Use `:patch accept-all` or `:patch reject-all` for bulk actions.
- After applying or rejecting hunks, the preview refreshes automatically.

### Build Output

Build output opens in its own buffer after `Ctrl+T` or `:build`.

- If no build command was configured, the build output explains that directly.
- `:ai error` uses the latest captured build output.

## AI Provider Setup

AI is disabled by default. Use `--ai openai`, `--ai codex`, or `FLOWSTATE_AI_MODE` to enable
AI for a session. Use `--no-ai` to force AI off even when you normally enable a provider.

### OpenAI

Required:

```bash
export FLOWSTATE_OPENAI_API_KEY=...
```

Optional:

```bash
export FLOWSTATE_OPENAI_MODEL=gpt-4.1-mini
export FLOWSTATE_OPENAI_BASE_URL=https://api.openai.com/v1/responses
```

Run:

```bash
./build/flowstate <file> --ai openai
```

### Codex

flowstate’s Codex mode uses a persistent local `codex app-server` session.

Requirements:

- `codex` must be installed and available on `PATH`
- your Codex CLI must already be authenticated

Optional:

```bash
export FLOWSTATE_CODEX_MODEL=...
export FLOWSTATE_CODEX_PROFILE=...
```

Run:

```bash
./build/flowstate <file> --ai codex
```

## Status Bar

The status bar shows:

- current buffer name
- detected language
- save state
- active AI provider
- active view
- cursor line and column
- AI request state such as `CONNECTING`, `STREAMING`, `PARSING PATCH`, `FAILED`, or `COMPLETE`

## Git Gutter

Inside a Git worktree, flowstate marks changes since the last commit in the line-number gutter:

- Green dashed bar: added lines
- Blue dashed bar: modified lines
- Red triangle: deleted lines, anchored where the deletion occurred

Clean files render the normal solid gutter separator.

Put the cursor on a blue modified line or red triangle line and press `Alt+D` to show or hide the previous lines inline. Git peeks are visual only; they are not part of the editable file buffer.

## Tips

- Use `Shift+Arrow`, then `Ctrl+E` when you want AI to focus on a specific region.
- Use `Ctrl+T` before `:ai error` so flowstate has fresh build output to analyze.
- If a patch is ready but you are back in the file buffer, press `Alt+P`.
- If an AI request finishes while you are back in the file buffer, press `Alt+E` to reopen the scratch buffer.
