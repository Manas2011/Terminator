# Terminator — Design Document

## 1. Introduction

Terminator is a terminal emulator and command interpreter implemented in C for Linux. It uses Xlib to create and manage its graphical interface, and POSIX process and synchronization facilities to execute commands in multiple independent tabs.

The implementation provides two execution models: a thread-based model and a process-based model. Both expose the same terminal functionality while using different mechanisms to isolate tab workers and communicate with the GUI.

## 2. High-level architecture

The application is organized around three cooperating responsibilities:

1. **GUI and input handling:** creates the X11 window, draws the active tab, receives keyboard and mouse events, and updates the terminal display.
2. **Tab workers:** receive submitted commands, dispatch built-ins, launch external commands, collect output, and maintain job state.
3. **Shared history:** stores commands in a bounded circular buffer and provides synchronized access to all tabs.

The `struct tab` data structure holds the state associated with an individual tab, including its input buffer, cursor position, output buffer, scroll position, working directory, foreground command state, background jobs, and worker communication state.

The shared history is represented by `struct shared_history`, which contains a fixed-capacity command array, a count, a circular-buffer index, and a semaphore used to coordinate access.

## 3. X11 GUI and event loop

The GUI is initialized using `XOpenDisplay`, `XCreateSimpleWindow`, `XSelectInput`, and `XMapWindow`. The application selects exposure, key-press, and button-press events. Text is drawn with Xlib drawing functions, and the terminal is redrawn from the active tab's stored output and input state.

The event loop uses `poll()` to wait for activity on the X11 connection and relevant communication/output descriptors. X11 events are then consumed with `XPending()` and `XNextEvent()`.

Important event handling includes:

- **Expose:** redraw the window when the X server requests a repaint.
- **KeyPress:** update the command buffer, move the cursor, invoke shortcuts, switch tabs, or submit a command.
- **ButtonPress:** handle mouse-wheel scrolling.
- **Output readiness:** read available command output and update the relevant tab buffer.

The output buffer and scroll offset allow the application to redraw the terminal and show earlier output. The `follow_output` state controls whether new output keeps the view at the bottom.

Only the GUI side interacts with Xlib. Worker threads and worker processes do not draw directly to the X11 window.

## 4. Tab management

Each tab has its own `struct tab` instance. The application supports up to ten tabs. A tab maintains its own current working directory, command line, output, foreground process information, and background-job table.

Ctrl+T creates a new tab. The active-tab index determines which tab is currently drawn and receives keyboard input. Switching the active tab does not require terminating the workers associated with other tabs.

## 5. Thread mode

Thread mode is the default execution mode:

```bash
./terminator
```

The application remains a single process. Each tab has a worker thread created with `pthread_create()`.

A worker waits for a command using a mutex and condition variable:

- The GUI-side code locks the tab's worker mutex.
- It copies the submitted command into the worker command buffer and marks a command as pending.
- It signals the condition variable.
- The worker wakes, copies the command locally, clears the pending flag, releases the mutex, and executes the command.

The worker uses `pthread_cond_wait()` rather than continuously checking for work. The GUI thread remains responsible for X11 operations.

The shared history is in the same process and is protected by a semaphore initialized for thread sharing.

## 6. Process mode

Process mode is selected with:

```bash
./terminator useProcess=True
```

In this mode, the GUI remains in the parent process and each tab has a worker process created with `fork()`.

The parent and workers communicate through pipes:

- A command pipe carries submitted commands from the GUI parent to a tab worker.
- A notification pipe informs the GUI parent that a worker has state or output changes to report.
- The worker handles command execution, reads command output, and reaps its child processes.
- The GUI parent polls notification descriptors and redraws the appropriate tab when needed.

Tab state is allocated with shared anonymous memory mapping (`mmap()` with `MAP_SHARED | MAP_ANONYMOUS`) so that the parent and worker processes can access the shared tab data.

The shared history is also allocated in shared memory. Its semaphore is initialized with process sharing enabled, allowing all tab worker processes to synchronize access to the same history.

The worker processes do not use Xlib. This keeps all drawing and X11 event handling in the GUI parent.

## 7. Command parsing and execution

Submitted commands pass through a command-processing path that handles command expansion, pipeline parsing, redirection parsing, built-in dispatch, and external command launch.

### 7.1 External commands

External commands are launched using `fork()` and `execvp()`. The child process prepares its file descriptors and then replaces its process image with the requested program. The parent tracks the child and collects its output through a pipe.

### 7.2 Built-in commands

Commands that need to modify or inspect the state of the current tab are handled by the command dispatcher. The implemented built-ins include:

- `cd` — changes the working directory of the current tab.
- `history` — displays recent shared command history.
- `jobs` — lists tracked background jobs.
- `fg` — brings a job to the foreground.
- `bg` — resumes a stopped job in the background.
- `multiWatch` — starts the parallel command-monitoring facility.

### 7.3 Environment variables and command substitution

The command-processing path expands environment variables and supports command substitution using `$(...)` and backticks. The command-substitution implementation captures the output of the nested command and uses it in the command being processed.

The multiline-input logic checks for incomplete constructs such as open quotes, command substitutions, and continuation backslashes. When more input is required, the interface accepts continuation lines.

## 8. Pipelines and I/O redirection

A pipeline is split into individual command stages. For a pipeline containing N commands, the implementation creates the required inter-stage pipes and launches the command processes.

The child processes use `dup2()` to connect standard input and standard output to the appropriate pipe ends. Unused pipe descriptors are closed so that the pipeline can make progress and detect end-of-file correctly.

The redirection parser recognizes:

- `<` for input redirection.
- `>` for truncating output redirection.
- `>>` for appending output.

The child opens the requested file with `open()` and uses `dup2()` to connect it to standard input or standard output before executing the command.

## 9. Process groups, signals, and job control

Commands and pipeline stages are organized into process groups. This allows the shell to signal a complete foreground pipeline or background job rather than addressing only one process.

The tab tracks foreground process information and a bounded table of background jobs. Job records include a job ID, process-group ID, child process IDs, execution state, and associated command text.

The job-control commands operate on these records:

- `jobs` displays active jobs.
- `fg` resumes a selected job in the foreground.
- `bg` resumes a stopped job in the background.

Ctrl+C sends an interrupt to the active foreground job. Ctrl+Z suspends the foreground job. The Terminator GUI itself remains running so the user can continue working in the tab.

Child processes are monitored and reaped using `waitpid()` so that completed processes do not remain as zombies.

## 10. multiWatch

`multiWatch` accepts a list of command strings and runs them concurrently. Each monitored command is launched independently, and its output is captured through a pipe.

A supervisor process polls the command output pipes. When output becomes available, it prints the command label, a Unix timestamp, and the captured output using the required separator format. When a monitored command finishes, the supervisor launches it again after the configured interval (two seconds by default).

The monitoring process uses `poll()` rather than busy-waiting. Ctrl+C is used to stop the monitoring process group while leaving the Terminator application available.

## 11. Command history

History is represented as a circular buffer with a capacity of 10,000 commands. The `next` field identifies the slot where the next command will be written, while `count` tracks the number of valid entries.

A semaphore protects reads and writes to the history structure. This is important because multiple tab workers can access the same history concurrently.

The history subsystem supports:

- Adding commands to shared history.
- Retrieving entries relative to the newest command.
- Up/down history navigation in the input handler.
- Displaying up to 1,000 recent commands with `history`.
- Persisting history in the user's home directory as `~/.terminator_history`.
- Ctrl+R search, which checks for an exact command and then uses longest-common-substring matching.

The same history structure is shared by all tabs in both execution modes.

## 12. Synchronization and inter-process communication summary

| Context | Mechanism | Purpose |
|---|---|---|
| Thread-mode command handoff | `pthread_mutex_t` | Protect the pending-command state and buffer |
| Thread-mode worker wake-up | `pthread_cond_t` | Sleep until a command is submitted |
| Shared history | POSIX semaphore | Serialize history access |
| Process-mode tab state | `mmap()` with shared mapping | Share tab state between parent and worker |
| Process-mode command submission | Pipe | Send commands from GUI to tab worker |
| Process-mode worker notification | Pipe | Notify GUI about worker updates |
| Command output | Pipe | Capture child-process output |
| Pipeline stages | Pipes and `dup2()` | Connect one command's output to the next command's input |
| Event and descriptor monitoring | `poll()` | Wait for X11, worker, and command-output activity |

## 13. Design decisions

- **One GUI owner:** X11 operations remain in the main GUI thread/process. This avoids concurrent drawing and event handling from tab workers.
- **Separate tab state:** each tab has an independent working directory, input/output state, and job table.
- **Shared history:** history is deliberately shared across tabs and protected with a semaphore.
- **Two execution modes:** thread mode demonstrates synchronization within one process, while process mode demonstrates process creation, shared memory, and pipe-based IPC.
- **Descriptor-driven waiting:** `poll()` is used to wait for available events and output rather than repeatedly checking descriptors in a tight loop.
- **Bounded structures:** fixed limits are used for tabs, command buffers, pipeline stages, jobs, and history storage.

## 14. Future enhancements

Possible extensions include file-name auto-completion, configurable key bindings, and a more comprehensive Bash-script interpretation layer.
