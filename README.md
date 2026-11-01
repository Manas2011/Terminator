# Terminator — A Custom Terminal

Terminator is a Linux terminal emulator and command interpreter written in C. It creates its own graphical terminal window using X11/Xlib and supports multiple independent tabs. Commands are executed through process creation and pipes, while the application manages command input, output, history, and job control.

## Features

- **X11 terminal interface:** graphical terminal window with a prompt, command input, output display, and scrolling.
- **Multiple tabs:** open and switch between independent terminal tabs; each tab maintains its own working directory and running jobs.
- **Two execution modes:**
  - **Thread mode (default):** one application process with a worker thread for each tab.
  - **Process mode:** the GUI remains in the parent process, with a separate worker process for each tab.
- **Command execution:** external commands are launched using `fork()` and `execvp()`.
- **Pipelines:** connect multiple commands using `|`.
- **I/O redirection:** support input redirection (`<`), output redirection (`>`), and append redirection (`>>`).
- **Command expansion:** environment-variable expansion and command substitution using `$(...)` and backticks.
- **Multiline input:** supports continuation lines, multiline quotes, and nested/multiline command substitutions.
- **Built-in commands:** `cd`, `history`, `jobs`, `fg`, `bg`, and `multiWatch`.
- **Job control:** run commands in the background with `&`, inspect jobs with `jobs`, and manage them with `fg` and `bg`.
- **Signal handling:** Ctrl+C interrupts the active foreground job and Ctrl+Z suspends it without terminating the Terminator application.
- **Searchable command history:** shared history across tabs, persistent history storage, history navigation, and Ctrl+R reverse search.
- **Line-editing shortcuts:** cursor movement, deletion shortcuts, screen clearing, and X11 cut-buffer copy/paste.
- **Parallel command monitoring:** `multiWatch` runs multiple commands concurrently, collects their output, and periodically restarts completed commands.

## Requirements

Terminator is intended for Linux and requires:

- GCC or another compatible C compiler
- GNU Make
- X11 development headers and libraries
- POSIX threads
- POSIX semaphores and memory-mapping support

On Ubuntu or Debian-based distributions, install the required packages with:

```bash
sudo apt update
sudo apt install build-essential libx11-dev
```

A working X server is also required to display the graphical terminal window. On a Linux desktop this is normally available in the session. When using WSL, configure an X server or a supported WSLg environment.

## Build

Clone the repository and move into the project directory:

```bash
git clone <your-repository-url>
cd Terminator
```

Build the application:

```bash
make
```

The Makefile builds the executable named `terminator`.

To remove generated object files and the executable:

```bash
make clean
```

## Run

### Thread mode (default)

```bash
./terminator
```

The GUI process creates a worker thread for each tab. The worker handles command execution while the main thread handles X11 events and drawing.

### Process mode

```bash
./terminator useProcess=True
```

The GUI runs in the parent process. Each tab is handled by a separate worker process, with pipes used for command submission and notifications. Shared tab state and command history are maintained in shared memory.

## Usage

Type commands in the terminal window and press Enter. Examples:

```bash
pwd
ls -l
cd /tmp
cat file.txt | sort | head
sort < input.txt
ls > listing.txt
echo "Current directory: $(pwd)"
sleep 30 &
jobs
```

Run several commands concurrently with `multiWatch`:

```bash
multiWatch ["date +%T", "ls | wc -l"]
```

Press Ctrl+C to stop `multiWatch` and return to the prompt.

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| Left / Right | Move the cursor within the command line |
| Ctrl+A / Ctrl+E | Move to the beginning / end of the line |
| Up / Down | Browse command history |
| Ctrl+U / Ctrl+K | Delete from the cursor to the beginning / end of the line |
| Ctrl+W | Delete the previous word |
| Ctrl+L | Clear the terminal output |
| Ctrl+D | Close the current tab / send EOF |
| Ctrl+R | Start reverse history search |
| Ctrl+Shift+C / Ctrl+Shift+V | Copy / paste using the X11 cut buffer |
| Ctrl+T | Open a new tab |
| Ctrl+C | Interrupt the active foreground job |
| Ctrl+Z | Suspend the active foreground job |
| Mouse wheel | Scroll through terminal output |

## History

Terminator maintains a shared command history with a capacity of 10,000 entries. The `history` command displays up to the most recent 1,000 entries. History is shared across tabs and stored in `~/.terminator_history`.

Press Ctrl+R to enter a search term. The search checks for an exact command first and otherwise uses the longest common substring match.

## Project structure

```text
Terminator/
├── temp.c
├── Makefile
├── README.md
├── DESIGN.md
├── tests/          # optional test scripts
└── screenshots/    # optional screenshots
```

## Future enhancements

Potential extensions include file-name auto-completion, a configurable key-binding system, and a more complete Bash-script interpretation layer.

## Notes

- Each tab has its own current working directory and job state.
- The X11 interface is managed by the GUI thread/process; tab workers do not draw directly to the X11 window.
- The project is designed for Linux and depends on an active X11-compatible display environment.
