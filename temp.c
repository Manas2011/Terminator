#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>

#define INPUT_SIZE 256
#define OUTPUT_SIZE 16384
#define MAX_TABS 10
#define MAX_COMMANDS 16
#define MAX_JOBS 32
#define MAX_POLL_FDS (1 + MAX_TABS * (MAX_JOBS + 1))
#define HISTORY_CAPACITY 10000

struct job
{
    int job_id;
    pid_t pgid;

    pid_t child_pids[MAX_COMMANDS];
    int child_exited[MAX_COMMANDS];
    int child_count;

    int pipe_fd;
    int running;
    int stopped;
    int child_stopped[MAX_COMMANDS];
    int active;

    char command[INPUT_SIZE];
};

struct shared_history;

struct tab
{
    char command[INPUT_SIZE];
    int length;
    int cursor_position;
    int history_nav; /* -1 means not browsing history */
    char history_draft[INPUT_SIZE];
    int reverse_search;
    char search_query[INPUT_SIZE];
    char search_saved_command[INPUT_SIZE];
    int search_offset;
    char search_match[INPUT_SIZE];
    int search_match_valid;
    int continuing_command;

    char output[OUTPUT_SIZE];
    size_t output_len;
    int output_scroll;
    int follow_output;

    int pipe_fd;
    pid_t child_pid;
    pid_t child_pid2;
    pid_t foreground_pgid;
    char foreground_command[INPUT_SIZE];
    int child1_exited;
    int child2_exited;
    int child_stopped[MAX_COMMANDS];
    int running;
    int should_exit;

    char cwd[PATH_MAX];

    pid_t child_pids[MAX_COMMANDS];
    int child_exited[MAX_COMMANDS];
    int child_count;
    int output_eof;

    struct job jobs[MAX_JOBS];
    int job_count;
    int next_job_id;

    /* Thread-mode command worker. X11 remains exclusively in the GUI thread. */
    pthread_t worker_thread;
    pthread_mutex_t worker_mutex;
    pthread_cond_t worker_cond;
    int worker_started;
    int worker_stop;
    int worker_command_pending;
    char worker_command[INPUT_SIZE];
    struct shared_history *history;

    /* Process-mode tab worker. Only the parent-side descriptors are used by the GUI. */
    pid_t worker_pid;
    int worker_command_fd;
    int worker_notify_fd;
};

struct redirection
{
    char input_file[INPUT_SIZE];
    char output_file[INPUT_SIZE];
    int has_input;
    int has_output;
    int append;
};

struct shared_history
{
    char commands[HISTORY_CAPACITY][INPUT_SIZE];
    int count;
    int next;
    sem_t lock;
};

static void history_save_file(struct shared_history *history);
static int history_loading_file = 0;

static void history_add(struct shared_history *history,
                        const char *command)
{

    while (sem_wait(&history->lock) == -1 && errno == EINTR)
    {
    }
    snprintf(history->commands[history->next],
             INPUT_SIZE, "%s", command);

    history->next = (history->next + 1) % HISTORY_CAPACITY;

    if (history->count < HISTORY_CAPACITY)
        history->count++;
    (void)sem_post(&history->lock);
    /* Persist shared history so it survives a Terminator restart. */
    if (!history_loading_file)
        history_save_file(history);
}

static int history_get(const struct shared_history *history,
                       int newest_offset,
                       char *result,
                       size_t result_size)
{
    if (history == NULL || result == NULL || result_size == 0)
        return -1;
    while (sem_wait((sem_t *)&history->lock) == -1 && errno == EINTR)
    {
    }
    if (newest_offset < 0 || newest_offset >= history->count)
    {
        (void)sem_post((sem_t *)&history->lock);
        return -1;
    }

    int index = (history->next - 1 - newest_offset + HISTORY_CAPACITY) % HISTORY_CAPACITY;

    snprintf(result, result_size, "%s",
             history->commands[index]);
    (void)sem_post((sem_t *)&history->lock);
    return 0;
}

static int history_count(struct shared_history *history)
{
    while (sem_wait(&history->lock) == -1 && errno == EINTR)
    {
    }
    int count = history->count;
    (void)sem_post(&history->lock);

    return count;
}

#define HISTORY_FILE_NAME ".terminator_history"
static void history_file_path(char *path, size_t size)
{
    const char *home = getenv("HOME");
    if (home && *home)
        snprintf(path, size, "%s/%s", home, HISTORY_FILE_NAME);
    else
        snprintf(path, size, "%s", HISTORY_FILE_NAME);
}

static void history_load_file(struct shared_history *history)
{
    char path[PATH_MAX];
    history_file_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp)
        return;
    char line[INPUT_SIZE];
    history_loading_file = 1;
    while (fgets(line, sizeof(line), fp))
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (*line)
            history_add(history, line);
    }
    history_loading_file = 0;
    fclose(fp);
}

static void history_save_file(struct shared_history *history)
{
    char path[PATH_MAX], temp[PATH_MAX + 16];
    history_file_path(path, sizeof(path));
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE *fp = fopen(temp, "w");
    if (!fp)
        return;
    int count = history_count(history);
    int first = count > HISTORY_CAPACITY ? HISTORY_CAPACITY : count;
    for (int i = first - 1; i >= 0; --i)
    {
        char entry[INPUT_SIZE];
        if (history_get(history, i, entry, sizeof(entry)) == 0)
            fprintf(fp, "%s\n", entry);
    }
    if (fclose(fp) == 0)
        (void)rename(temp, path);
    else
        (void)unlink(temp);
}

static void append_output(struct tab *t, const char *data, size_t n);

// static int longest_common_substring(const char *a, const char *b)
// {
//     int best = 0;
//     size_t na = strlen(a), nb = strlen(b);
//     for (size_t i = 0; i < na; ++i)
//         for (size_t j = 0; j < nb; ++j)
//         {
//             int k = 0;
//             while (i + (size_t)k < na && j + (size_t)k < nb && a[i + k] == b[j + k])
//                 k++;
//             if (k > best)
//                 best = k;
//         }
//     return best;
// }

/*static void history_search_print(struct tab *t, const char *term)
{
    struct shared_history *h = t->history;
    int total = h ? history_count(h) : 0;
    char entry[INPUT_SIZE];
    //Exact match wins; scan newest to oldest.
    for (int i = 0; i < total; ++i)
        if (history_get(h, i, entry, sizeof(entry)) == 0 && strcmp(entry, term) == 0)
        {
            append_output(t, entry, strlen(entry));
            append_output(t, "\n", 1);
            return;
        }
    int best = 0;
    for (int i = 0; i < total; ++i)
        if (history_get(h, i, entry, sizeof(entry)) == 0)
        {
            int n = longest_common_substring(entry, term);
            if (n > best)
                best = n;
        }
    if (best <= 2)
    {
        const char *msg = "No match for search term in history.\\n";
        append_output(t, msg, strlen(msg));
        return;
    }
    for (int i = total - 1; i >= 0; --i)
        if (history_get(h, i, entry, sizeof(entry)) == 0 && longest_common_substring(entry, term) == best)
        {
            append_output(t, entry, strlen(entry));
            append_output(t, "\n", 1);
        }
}*/

static void history_find_live_match(struct tab *t)
{
    t->search_match[0] = '\0';
    t->search_match_valid = 0;

    if (!t->history || t->search_query[0] == '\0')
        return;

    char entry[INPUT_SIZE];
    int total = history_count(t->history);

    /* Search newest to oldest. First substring match wins. */
    for (int i = 0; i < total; i++)
    {
        if (history_get(t->history, i, entry, sizeof(entry)) == 0 &&
            strstr(entry, t->search_query) != NULL)
        {
            snprintf(t->search_match, sizeof(t->search_match),
                     "%s", entry);

            t->search_match_valid = 1;
            t->search_offset = i;
            return;
        }
    }
}

static int all_children_stopped_or_exited(struct tab *t);
static void save_stopped_job(struct tab *t, const char *command);

static struct job *add_job(struct tab *t, pid_t pgid,
                           const char *command)
{
    for (int i = 0; i < MAX_JOBS; i++)
    {
        if (t->jobs[i].active)
            continue;

        struct job *job = &t->jobs[i];
        memset(job, 0, sizeof(*job));

        job->job_id = ++t->next_job_id;
        job->pgid = pgid;
        job->pipe_fd = -1;
        job->running = 1;
        job->active = 1;

        snprintf(job->command, sizeof(job->command),
                 "%s", command);

        t->job_count++;
        return job;
    }

    return NULL;
}

static void remove_job(struct tab *t, struct job *job)
{
    if (job == NULL || !job->active)
        return;

    if (job->pipe_fd >= 0)
    {
        close(job->pipe_fd);
        job->pipe_fd = -1;
    }

    memset(job, 0, sizeof(*job));
    job->pipe_fd = -1;

    if (t->job_count > 0)
        t->job_count--;
}

static void make_prompt(const struct tab *t,
                        char *prompt, size_t size)
{
    const char *user = getenv("USER");
    const char *home = getenv("HOME");

    if (user == NULL)
        user = "user";

    if (home != NULL &&
        strncmp(t->cwd, home, strlen(home)) == 0 &&
        (t->cwd[strlen(home)] == '/' ||
         t->cwd[strlen(home)] == '\0'))
    {

        snprintf(prompt, size, "%s@terminator:~%s$ ",
                 user, t->cwd + strlen(home));
    }
    else
    {
        snprintf(prompt, size, "%s@terminator:%s$ ",
                 user, t->cwd);
    }
}

/* Set only inside a process-mode tab worker. The GUI process never writes this fd. */
static int process_worker_notify_fd = -1;

static void notify_process_parent(void)
{
    if (process_worker_notify_fd >= 0)
    {
        const unsigned char byte = 1;
        ssize_t ignored = write(process_worker_notify_fd, &byte, sizeof(byte));
        (void)ignored; /* A full nonblocking notification pipe is already readable. */
    }
}

static void append_output(struct tab *t, const char *data, size_t n)
{
    int was_following = t->follow_output;
    if (n >= OUTPUT_SIZE)
    {
        data += n - (OUTPUT_SIZE - 1);
        n = OUTPUT_SIZE - 1;
        t->output_len = 0;
    }
    if (t->output_len + n >= OUTPUT_SIZE)
    {
        size_t remove = t->output_len + n - (OUTPUT_SIZE - 1);
        memmove(t->output, t->output + remove, t->output_len - remove);
        t->output_len -= remove;
    }
    memcpy(t->output + t->output_len, data, n);
    t->output_len += n;
    t->output[t->output_len] = '\0';
    if (was_following)
    {
        t->output_scroll = 0;
        t->follow_output = 1;
    }
    notify_process_parent();
}

static void append_command_to_history(struct tab *t)
{
    char prompt[PATH_MAX + 128];
    if (t->reverse_search)
        snprintf(prompt, sizeof(prompt), "(reverse-i-search)`%s': ", t->search_query);
    else
        make_prompt(t, prompt, sizeof(prompt));

    append_output(t, prompt, strlen(prompt));

    const char *line = t->command;
    const char *end = t->command + t->length;
    int first_line = 1;

    while (line < end)
    {
        const char *newline = memchr(line, '\n', (size_t)(end - line));
        const char *line_end = newline ? newline : end;

        if (!first_line)
            append_output(t, "> ", 2);

        append_output(t, line, (size_t)(line_end - line));
        append_output(t, "\n", 1);

        first_line = 0;
        line = newline ? newline + 1 : end;
    }

    /* Preserve a final empty line if the command ends with '\n'. */
    if (t->length > 0 && t->command[t->length - 1] == '\n')
    {
        append_output(t, "> ", 2);
        append_output(t, "\n", 1);
    }
}

static void draw_tabs(Display *dpy, Window win, GC gc,
                      int tab_count, int active_tab)
{
    const int tab_width = 68;
    const int tab_height = 30;

    for (int i = 0; i < tab_count; i++)
    {
        int x = i * tab_width;

        /* Outline every tab; the active tab gets a rectangle around it. */
        XDrawRectangle(dpy, win, gc, x + 2, 2,
                       (unsigned int)(tab_width - 5),
                       (unsigned int)(tab_height - 5));

        char label[16];
        snprintf(label, sizeof(label), "Tab %d", i + 1);
        XDrawString(dpy, win, gc, x + 10, 20, label,
                    (int)strlen(label));

        if (i == active_tab)
        {
            /* Underline the selected tab. */
            XDrawLine(dpy, win, gc, x + 5, tab_height - 2,
                      x + tab_width - 5, tab_height - 2);
        }
    }

    XDrawLine(dpy, win, gc, 0, tab_height + 2, 700, tab_height + 2);
}

static void redraw(Display *dpy, Window win, GC gc,
                   struct tab *t, int tab_count, int active_tab)
{
    char prompt[PATH_MAX + 128];
    make_prompt(t, prompt, sizeof(prompt));

    XClearWindow(dpy, win);
    draw_tabs(dpy, win, gc, tab_count, active_tab);

    /* Draw the most recent output lines below the tab bar. */
    int y = 52;
    const char *begin = t->output;
    const char *end = t->output + t->output_len;
    const char *line_starts[64];
    int line_lengths[64];
    int count = 0;

    while (begin < end)
    {
        const char *nl = memchr(begin, '\n', (size_t)(end - begin));
        const char *line_end = nl ? nl : end;

        if (count == 64)
        {
            memmove(line_starts, line_starts + 1,
                    63 * sizeof(line_starts[0]));
            memmove(line_lengths, line_lengths + 1,
                    63 * sizeof(line_lengths[0]));
            count = 63;
        }

        line_starts[count] = begin;
        line_lengths[count] = (int)(line_end - begin);
        count++;
        begin = nl ? nl + 1 : end;
    }

    int line_height = 18;
    int first_y = 52;
    int bottom_y = 625;

    int visible_lines = (bottom_y - first_y) / line_height - 1;
    int max_scroll = count > visible_lines
                         ? count - visible_lines
                         : 0;
    if (t->output_scroll < 0)
        t->output_scroll = 0;

    if (t->output_scroll > max_scroll)
        t->output_scroll = max_scroll;

    int first = t->follow_output
                    ? max_scroll
                    : max_scroll - t->output_scroll;

    if (first < 0)
        first = 0;
    for (int i = first; i < count && i < first + visible_lines; i++)
    {
        if (line_lengths[i] > 0 && y + line_height <= bottom_y - line_height)
        {
            XDrawString(dpy, win, gc, 10, y,
                        line_starts[i], line_lengths[i]);
        }
        y += line_height;
    }
    if (y + line_height > bottom_y)
        y = bottom_y - line_height;

    int input_y = y;
    // retrive font associated with GC, to locate cursor(variable width of chars)
    XFontStruct *input_font = XQueryFont(dpy, XGContextFromGC(gc));

    /* Split the command into logical input lines. */
    int line_count = 0;
    int line_start = 0;

    for (int i = 0; i <= t->length; i++)
    {
        if (i == t->length || t->command[i] == '\n')
        {
            if (line_count < INPUT_SIZE)
            {
                line_starts[line_count] = t->command + line_start;
                line_lengths[line_count] = i - line_start;
                line_count++;
            }

            line_start = i + 1;
        }
    }

    int visible_input_lines = line_count > 20 ? 20 : line_count;
    int first_visible = line_count - visible_input_lines;

    for (int i = first_visible; i < line_count; i++)
    {
        const char *line_prompt = (i == 0) ? prompt : "> ";
        int prompt_len = (int)strlen(line_prompt);
        int prompt_width = input_font
                               ? XTextWidth(input_font, line_prompt, prompt_len)
                               : prompt_len * 8;
        int input_x = 10 + prompt_width;

        XDrawString(dpy, win, gc, 10, input_y,
                    line_prompt, prompt_len);

        if (line_lengths[i] > 0)
        {
            XDrawString(dpy, win, gc, input_x, input_y,
                        line_starts[i], line_lengths[i]);
        }

        // cursor
        int absolute_start = (int)(line_starts[i] - t->command);
        int absolute_end = absolute_start + line_lengths[i];
        if (t->cursor_position >= absolute_start &&
            t->cursor_position <= absolute_end)
        {
            int column = t->cursor_position - absolute_start;
            int caret_x = input_x +
                          (input_font
                               ? XTextWidth(input_font, line_starts[i], column)
                               : column * 8);
            XDrawLine(dpy, win, gc, caret_x, input_y - 13,
                      caret_x, input_y + 2);
        }

        input_y += 18;
    }

    if (input_font)
        XFreeFontInfo(NULL, input_font, 1);

    XFlush(dpy);
}

static int parse_command(char *input, char **argv, int max_args)
{
    int argc = 0;
    char *src = input;
    char *dst = input;

    while (*src != '\0')
    {
        while (*src == ' ' || *src == '\t')
        {
            src++;
        }

        if (*src == '\0')
        {
            break;
        }

        if (argc >= max_args - 1)
        {
            return -1;
        }

        argv[argc++] = dst;

        char quote = '\0';

        while (*src != '\0')
        {
            if (quote == '\0' &&
                (*src == ' ' || *src == '\t'))
            {
                break;
            }

            if (quote == '\0' &&
                (*src == '\'' || *src == '"'))
            {
                quote = *src++;
                continue;
            }

            if (quote != '\0' && *src == quote)
            {
                quote = '\0';
                src++;
                continue;
            }

            if (*src == '\\' && quote != '\'')
            {
                if (src[1] == '\0')
                {
                    return -1;
                }

                if (quote == '"' &&
                    src[1] != '"' &&
                    src[1] != '\\' &&
                    src[1] != '$' &&
                    src[1] != '`')
                {
                    *dst++ = *src++;
                }
                else
                {
                    src++;
                }
            }

            *dst++ = *src++;
        }

        if (quote != '\0')
        {
            return -1;
        }
        if (*src == ' ' || *src == '\t')
        {
            src++;
        }

        *dst++ = '\0';

        while (*src == ' ' || *src == '\t')
        {
            src++;
        }
    }

    argv[argc] = NULL;
    return argc;
}

static int parse_redirections(
    const char *input,
    char *clean,
    size_t clean_size,
    struct redirection *redir)
{
    redir->input_file[0] = '\0';
    redir->output_file[0] = '\0';
    redir->has_input = 0;
    redir->has_output = 0;
    redir->append = 0;

    size_t i = 0;
    size_t j = 0;

    while (input[i] != '\0')
    {
        if (input[i] == '\'' || input[i] == '"')
        {
            char quote = input[i];

            if (j + 1 >= clean_size)
                return -1;

            clean[j++] = input[i++];

            while (input[i] != '\0')
            {
                if (j + 1 >= clean_size)
                    return -1;

                if (quote == '"' &&
                    input[i] == '\\' &&
                    input[i + 1] != '\0')
                {
                    clean[j++] = input[i++];
                    clean[j++] = input[i++];
                    continue;
                }

                char c = input[i++];
                clean[j++] = c;

                if (c == quote)
                    break;
            }

            continue;
        }

        if (input[i] == '\\' && input[i + 1] != '\0')
        {
            if (j + 2 >= clean_size)
                return -1;

            clean[j++] = input[i++];
            clean[j++] = input[i++];
            continue;
        }

        if (input[i] != '<' && input[i] != '>')
        {
            if (j + 1 >= clean_size)
                return -1;

            clean[j++] = input[i++];
            continue;
        }

        char op = input[i++];
        int append = 0;

        if (op == '>' && input[i] == '>')
        {
            append = 1;
            i++;
        }

        while (input[i] == ' ' || input[i] == '\t')
            i++;

        char *filename =
            (op == '<') ? redir->input_file : redir->output_file;

        int *present =
            (op == '<') ? &redir->has_input : &redir->has_output;

        if (*present)
        {
            return -1;
        }

        size_t k = 0;
        char quote = '\0';

        while (input[i] != '\0')
        {
            char c = input[i];

            if (quote == '\0' &&
                (c == ' ' || c == '\t' || c == '<' || c == '>'))
            {
                break;
            }

            if (c == '\\' && input[i + 1] != '\0' &&
                quote != '\'')
            {
                if (k + 2 >= INPUT_SIZE)
                    return -1;

                filename[k++] = input[i++];
                filename[k++] = input[i++];
                continue;
            }

            if (quote == '\0' && (c == '\'' || c == '"'))
            {
                quote = c;
            }
            else if (quote == c)
            {
                quote = '\0';
            }

            if (k + 1 >= INPUT_SIZE)
                return -1;

            filename[k++] = input[i++];
        }

        filename[k] = '\0';

        if (k == 0 || quote != '\0')
            return -1;

        *present = 1;

        if (op == '>')
            redir->append = append;
    }

    clean[j] = '\0';
    return 0;
}

static int split_pipeline(
    const char *input,
    char commands[][INPUT_SIZE],
    int max_commands)
{
    char quote = '\0';
    const char *start = input;
    int count = 0;

    for (const char *p = input;; p++)
    {
        if (*p == '\0' || (*p == '|' && quote == '\0'))
        {
            if (count >= max_commands)
                return -1;

            const char *begin = start;
            const char *end = p;

            while (begin < end &&
                   (*begin == ' ' || *begin == '\t'))
                begin++;

            while (end > begin &&
                   (end[-1] == ' ' || end[-1] == '\t'))
                end--;

            size_t len = (size_t)(end - begin);

            if (len == 0 || len >= INPUT_SIZE)
                return -1;

            memcpy(commands[count], begin, len);
            commands[count][len] = '\0';
            count++;

            if (*p == '\0')
                break;

            start = p + 1;
            continue;
        }

        if (quote == '\'')
        {
            if (*p == '\'')
                quote = '\0';
            continue;
        }

        if (quote == '"')
        {
            if (*p == '"')
                quote = '\0';
            else if (*p == '\\' && p[1] != '\0')
                p++;
            continue;
        }

        if (*p == '\'' || *p == '"')
        {
            quote = *p;
            continue;
        }

        if (*p == '\\' && p[1] != '\0')
        {
            p++;
            continue;
        }
    }

    if (quote != '\0')
        return -1;

    return count;
}

static void execute_child_command(char *command)
{
    char *argv[INPUT_SIZE / 2];
    char clean_command[INPUT_SIZE];
    struct redirection redir;

    if (parse_redirections(
            command,
            clean_command,
            sizeof(clean_command),
            &redir) == -1)
    {
        fprintf(stderr, "terminator: invalid redirection syntax\n");
        _exit(2);
    }

    int argc = parse_command(
        clean_command,
        argv,
        (int)(sizeof(argv) / sizeof(argv[0])));

    if (argc < 0)
    {
        fprintf(stderr, "terminator: invalid command syntax\n");
        _exit(2);
    }

    if (argc == 0)
        _exit(0);

    /* Input redirection overrides the current stdin. */
    if (redir.has_input)
    {
        char *filename_argv[2];
        int n = parse_command(redir.input_file, filename_argv, 2);

        if (n != 1 || filename_argv[0][0] == '\0')
        {
            fprintf(stderr, "terminator: invalid input redirection\n");
            _exit(2);
        }

        int file_fd = open(filename_argv[0], O_RDONLY);

        if (file_fd == -1)
        {
            perror("open input");
            _exit(1);
        }

        if (dup2(file_fd, STDIN_FILENO) == -1)
        {
            perror("dup2 input");
            close(file_fd);
            _exit(1);
        }

        close(file_fd);
    }

    /* Output redirection overrides the current stdout. */
    if (redir.has_output)
    {
        char *filename_argv[2];
        int n = parse_command(redir.output_file, filename_argv, 2);

        if (n != 1 || filename_argv[0][0] == '\0')
        {
            fprintf(stderr, "terminator: invalid output redirection\n");
            _exit(2);
        }

        int flags = O_WRONLY | O_CREAT;

        if (redir.append)
            flags |= O_APPEND;
        else
            flags |= O_TRUNC;

        int file_fd = open(filename_argv[0], flags, 0644);

        if (file_fd == -1)
        {
            perror("open output");
            _exit(1);
        }

        if (dup2(file_fd, STDOUT_FILENO) == -1)
        {
            perror("dup2 output");
            close(file_fd);
            _exit(1);
        }

        close(file_fd);
    }

    execvp(argv[0], argv);
    perror("execvp");
    _exit(127);
}

static int capture_command_output(
    const char *command,
    const char *cwd,
    char *output,
    size_t output_size)
{
    int fd[2];

    if (output_size == 0)
        return -1;

    output[0] = '\0';

    if (pipe(fd) == -1)
    {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();

    if (pid == -1)
    {
        perror("fork");
        close(fd[0]);
        close(fd[1]);
        return -1;
    }

    if (pid == 0)
    {
        close(fd[0]);

        if (dup2(fd[1], STDOUT_FILENO) == -1)
        {
            perror("dup2");
            close(fd[1]);
            _exit(1);
        }

        close(fd[1]);

        if (chdir(cwd) == -1)
        {
            perror("chdir");
            _exit(1);
        }

        char command_copy[INPUT_SIZE];

        snprintf(command_copy, sizeof(command_copy), "%s", command);

        execute_child_command(command_copy);

        _exit(127);
    }

    close(fd[1]);

    size_t used = 0;
    char buffer[1024];
    int read_error = 0;

    /*
     * Aane do data ko , buffer ki chinta mat karo parth ,
     buffer ko overflow kar jane do
     jab tak pipe ka dum nahi nikal jata . anytha deadlock ho jayga
     */
    for (;;)
    {
        ssize_t n = read(fd[0], buffer, sizeof(buffer));

        if (n == 0)
            break;

        if (n == -1)
        {
            if (errno == EINTR)
                continue;

            perror("read");
            read_error = 1;
            break;
        }

        size_t available = output_size - 1 - used;
        size_t to_copy = (size_t)n;

        if (to_copy > available)
            to_copy = available;

        if (to_copy > 0)
        {
            memcpy(output + used, buffer, to_copy);
            used += to_copy;
        }
    }

    close(fd[0]);
    output[used] = '\0';

    int status;
    pid_t waited;

    do
    {
        waited = waitpid(pid, &status, 0);
    } while (waited == -1 && errno == EINTR);

    if (waited == -1)
    {
        perror("waitpid");
        return -1;
    }

    if (read_error)
        return -1;

    while (used > 0 &&
           (output[used - 1] == '\n' || output[used - 1] == '\r'))
    {
        output[--used] = '\0';
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;

    return 0;
}

static int expand_command_substitutions(
    char *command,
    size_t command_size,
    const char *cwd)
{
    char expanded[INPUT_SIZE];

    size_t input_len = strlen(command);
    size_t used = 0;

    for (size_t i = 0; i < input_len;)
    {
        if (command[i] == '$' && command[i + 1] == '(')
        {
            size_t start = i + 2;
            size_t j = start;
            int depth = 1;
            char quote = '\0';

            /*
             * Find the matching closing parenthesis.
             * Ignore parentheses inside single or double quotes.
             */
            for (; j < input_len; j++)
            {
                char c = command[j];

                if (quote != '\0')
                {
                    if (c == quote)
                        quote = '\0';

                    continue;
                }

                if (c == '\'' || c == '"')
                {
                    quote = c;
                }
                else if (c == '(')
                {
                    depth++;
                }
                else if (c == ')')
                {
                    depth--;

                    if (depth == 0)
                        break;
                }
            }

            if (depth != 0 || quote != '\0')
            {
                fprintf(stderr,
                        "terminator: unmatched command substitution\n");
                return -1;
            }

            size_t inner_len = j - start;

            if (inner_len == 0 || inner_len >= INPUT_SIZE)
            {
                fprintf(stderr,
                        "terminator: invalid command substitution\n");
                return -1;
            }

            char inner_command[INPUT_SIZE];
            char captured[OUTPUT_SIZE];

            memcpy(inner_command, command + start, inner_len);
            inner_command[inner_len] = '\0';

            /* Expand any nested substitutions first. */
            if (expand_command_substitutions(
                    inner_command, sizeof(inner_command), cwd) == -1)
            {
                return -1;
            }

            if (capture_command_output(
                    inner_command, cwd, captured, sizeof(captured)) == -1)
            {
                fprintf(stderr,
                        "terminator: command substitution failed\n");
                return -1;
            }

            size_t captured_len = strlen(captured);

            if (used + captured_len >= sizeof(expanded))
            {
                fprintf(stderr,
                        "terminator: expanded command is too long\n");
                return -1;
            }

            memcpy(expanded + used, captured, captured_len);
            used += captured_len;

            i = j + 1;
        }
        else
        {
            if (used + 1 >= sizeof(expanded))
            {
                fprintf(stderr,
                        "terminator: expanded command is too long\n");
                return -1;
            }

            expanded[used++] = command[i++];
        }
    }

    expanded[used] = '\0';

    if (used >= command_size)
    {
        fprintf(stderr, "terminator: expanded command is too long\n");
        return -1;
    }

    memcpy(command, expanded, used + 1);

    return 0;
}

static int expand_environment_variables(
    char *command,
    size_t command_size)
{
    char expanded[INPUT_SIZE];
    size_t used = 0;
    char quote = '\0';

    for (size_t i = 0; command[i] != '\0';)
    {
        char c = command[i];

        /*
         * Track single and double quotes.
         * Variables inside single quotes remain literal.
         */
        if (c == '\'' && quote != '"')
        {
            quote = (quote == '\'') ? '\0' : '\'';

            if (used + 1 >= sizeof(expanded))
                return -1;

            expanded[used++] = command[i++];
            continue;
        }

        if (c == '"' && quote != '\'')
        {
            quote = (quote == '"') ? '\0' : '"';

            if (used + 1 >= sizeof(expanded))
                return -1;

            expanded[used++] = command[i++];
            continue;
        }

        /*
         * Preserve escaped characters outside single quotes.
         */
        if (c == '\\' && quote != '\'' &&
            command[i + 1] != '\0')
        {
            if (used + 2 >= sizeof(expanded))
                return -1;

            expanded[used++] = command[i++];
            expanded[used++] = command[i++];
            continue;
        }

        /*
         * Expand $NAME outside single quotes.
         */
        if (c == '$' && quote != '\'' &&
            (isalpha((unsigned char)command[i + 1]) ||
             command[i + 1] == '_'))
        {
            size_t start = i + 1;
            size_t end = start;

            while (isalnum((unsigned char)command[end]) ||
                   command[end] == '_')
            {
                end++;
            }

            size_t name_len = end - start;
            char name[INPUT_SIZE];

            if (name_len >= sizeof(name))
                return -1;

            memcpy(name, command + start, name_len);
            name[name_len] = '\0';

            const char *value = getenv(name);

            if (value == NULL)
                value = "";

            size_t value_len = strlen(value);

            if (used + value_len >= sizeof(expanded))
            {
                fprintf(stderr,
                        "terminator: expanded command is too long\n");
                return -1;
            }

            memcpy(expanded + used, value, value_len);
            used += value_len;
            i = end;
            continue;
        }

        if (used + 1 >= sizeof(expanded))
        {
            fprintf(stderr,
                    "terminator: expanded command is too long\n");
            return -1;
        }

        expanded[used++] = command[i++];
    }

    expanded[used] = '\0';

    if (used >= command_size)
    {
        fprintf(stderr,
                "terminator: expanded command is too long\n");
        return -1;
    }

    memcpy(command, expanded, used + 1);

    return 0;
}

static int start_pipeline(struct tab *t,
                          char commands[][INPUT_SIZE],
                          int count,
                          int background,
                          const char *original_command)
{
    if (count < 2 || count > MAX_COMMANDS)
        return -1;

    int pipes[MAX_COMMANDS - 1][2];
    int output_fd[2];
    pid_t pids[MAX_COMMANDS];
    int created = 0;
    int pipes_created = 0;

    if (pipe(output_fd) == -1)
    {
        perror("pipe");
        return -1;
    }

    for (int i = 0; i < count - 1; i++)
    {
        if (pipe(pipes[i]) == -1)
        {
            perror("pipe");

            for (int j = 0; j < pipes_created; j++)
            {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            close(output_fd[0]);
            close(output_fd[1]);
            return -1;
        }

        pipes_created++;
    }

    for (int i = 0; i < count; i++)
    {
        pid_t pid = fork();

        if (pid == -1)
        {
            perror("fork");

            for (int j = 0; j < pipes_created; j++)
            {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            close(output_fd[0]);
            close(output_fd[1]);

            for (int j = 0; j < created; j++)
                kill(pids[j], SIGTERM);

            for (int j = 0; j < created; j++)
            {
                while (waitpid(pids[j], NULL, 0) == -1 &&
                       errno == EINTR)
                {
                }
            }

            return -1;
        }

        if (pid == 0)
        {
            if (setpgid(0, i == 0 ? 0 : pids[0]) == -1)
            {
                perror("setpgid");
                _exit(127);
            }
            if (i > 0 &&
                dup2(pipes[i - 1][0], STDIN_FILENO) == -1)
            {
                perror("dup2 stdin");
                _exit(127);
            }

            int out_fd = (i < count - 1)
                             ? pipes[i][1]
                             : output_fd[1];

            if (dup2(out_fd, STDOUT_FILENO) == -1 ||
                dup2(output_fd[1], STDERR_FILENO) == -1)
            {
                perror("dup2 output");
                _exit(127);
            }

            for (int j = 0; j < pipes_created; j++)
            {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            close(output_fd[0]);
            close(output_fd[1]);

            if (chdir(t->cwd) == -1)
            {
                perror("chdir");
                _exit(127);
            }

            char command_copy[INPUT_SIZE];

            snprintf(command_copy, sizeof(command_copy), "%s", commands[i]);

            execute_child_command(command_copy);
        }

        if (setpgid(pid, i == 0 ? pid : pids[0]) == -1 &&
            errno != EACCES && errno != ESRCH)
        {
            perror("setpgid");
        }
        pids[created++] = pid;
    }

    for (int i = 0; i < pipes_created; i++)
    {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    close(output_fd[1]);

    if (background)
    {
        struct job *job = add_job(t, pids[0], original_command);

        if (job == NULL)
        {
            fprintf(stderr, "terminator: maximum background jobs reached\n");
            close(output_fd[0]);

            for (int i = 0; i < count; i++)
                kill(pids[i], SIGTERM);

            for (int i = 0; i < count; i++)
            {
                while (waitpid(pids[i], NULL, 0) == -1 &&
                       errno == EINTR)
                {
                }
            }

            return -1;
        }

        job->pipe_fd = output_fd[0];
        job->child_count = count;

        for (int i = 0; i < count; i++)
        {
            job->child_pids[i] = pids[i];
            job->child_exited[i] = 0;
        }

        return 0;
    }

    /* Foreground pipeline: preserve existing tracking. */
    t->pipe_fd = output_fd[0];
    t->child_count = count;
    t->output_eof = 0;
    t->foreground_pgid = pids[0];
    snprintf(t->foreground_command, sizeof(t->foreground_command),
             "%s", original_command);

    for (int i = 0; i < count; i++)
    {
        t->child_pids[i] = pids[i];
        t->child_exited[i] = 0;
    }

    t->child_pid = pids[0];
    t->child_pid2 = (count > 1) ? pids[1] : -1;
    t->child1_exited = 0;
    t->child2_exited = (count == 1);
    t->running = 1;

    return 0;
}

static int start_command(struct tab *t,
                         int background,
                         const char *original_command)
{
    int fd[2];
    if (pipe(fd) == -1)
    {
        perror("pipe");
        return -1;
    }

    char command_copy[INPUT_SIZE];
    snprintf(command_copy, sizeof(command_copy), "%s", original_command);

    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork");
        close(fd[0]);
        close(fd[1]);
        return -1;
    }
    t->child_pid2 = -1;
    if (pid == 0)
    {
        if (setpgid(0, 0) == -1)
        {
            perror("setpgid");
            _exit(127);
        }
        close(fd[0]);
        if (dup2(fd[1], STDOUT_FILENO) == -1 ||
            dup2(fd[1], STDERR_FILENO) == -1)
        {
            perror("dup2");
            _exit(127);
        }
        close(fd[1]);
        if (chdir(t->cwd) == -1)
        {
            perror("chdir");
            _exit(127);
        }
        execute_child_command(command_copy);
    }

    if (setpgid(pid, pid) == -1 && errno != EACCES && errno != ESRCH)
        perror("setpgid");

    close(fd[1]);

    if (background)
    {
        struct job *job = add_job(t, pid, original_command);

        if (job == NULL)
        {
            fprintf(stderr, "terminator: maximum background jobs reached\n");
            close(fd[0]);
            kill(pid, SIGTERM);

            while (waitpid(pid, NULL, 0) == -1 &&
                   errno == EINTR)
            {
            }

            return -1;
        }

        job->pipe_fd = fd[0];
        job->child_count = 1;
        job->child_pids[0] = pid;
        job->child_exited[0] = 0;

        return 0;
    }

    /* Foreground command: preserve existing tracking. */
    t->pipe_fd = fd[0];
    t->child_pid = pid;
    t->child_pid2 = -1;

    t->child_count = 1;
    t->child_pids[0] = pid;
    t->child_exited[0] = 0;

    t->child1_exited = 0;
    t->child2_exited = 1;
    t->output_eof = 0;
    t->foreground_pgid = pid;
    snprintf(t->foreground_command, sizeof(t->foreground_command),
             "%s", original_command);

    return 0;
}

static int change_directory(struct tab *t, const char *path)
{
    char target[PATH_MAX];
    char resolved[PATH_MAX];

    /* No argument: go to HOME. */
    if (path == NULL || path[0] == '\0')
    {
        path = getenv("HOME");

        if (path == NULL || path[0] == '\0')
        {
            snprintf(t->output, sizeof(t->output),
                     "cd: HOME not set\n");
            t->output_len = strlen(t->output);
            return -1;
        }
    }

    /* Construct an absolute target path. */
    if (path[0] == '/')
    {
        if (snprintf(target, sizeof(target), "%s", path) >= (int)sizeof(target))
        {
            snprintf(t->output, sizeof(t->output),
                     "cd: path too long\n");
            t->output_len = strlen(t->output);
            return -1;
        }
    }
    else
    {
        if (snprintf(target, sizeof(target), "%s/%s",
                     t->cwd, path) >= (int)sizeof(target))
        {
            snprintf(t->output, sizeof(t->output),
                     "cd: path too long\n");
            t->output_len = strlen(t->output);
            return -1;
        }
    }

    /* Resolve the path without changing the GUI's cwd. */
    if (realpath(target, resolved) == NULL)
    {
        int saved_errno = errno;

        snprintf(t->output, sizeof(t->output),
                 "cd: %s: %s\n", path, strerror(saved_errno));
        t->output_len = strlen(t->output);
        return -1;
    }

    /* Ensure the destination is a directory. */
    struct stat st;

    if (stat(resolved, &st) == -1 || !S_ISDIR(st.st_mode))
    {
        snprintf(t->output, sizeof(t->output),
                 "cd: %s: Not a directory\n", path);
        t->output_len = strlen(t->output);
        return -1;
    }

    /* Store the new directory for this tab only. */
    if (strlen(resolved) >= sizeof(t->cwd))
    {
        snprintf(t->output, sizeof(t->output),
                 "cd: path too long\n");
        t->output_len = strlen(t->output);
        return -1;
    }

    strcpy(t->cwd, resolved);

    /* Clear the previous error/output after success. */
    t->output[0] = '\0';
    t->output_len = 0;

    return 0;
}

static int reap_children(struct tab *t)
{
    int all_exited = 1;

    for (int i = 0; i < t->child_count; i++)
    {
        if (t->child_exited[i])
            continue;

        pid_t pid = t->child_pids[i];

        if (pid <= 0)
        {
            t->child_exited[i] = 1;
            continue;
        }

        int status;
        pid_t result;

        do
        {
            result = waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
        } while (result == -1 && errno == EINTR);

        if (result == pid)
        {
            if (WIFSTOPPED(status))
            {
                t->child_stopped[i] = 1;
                all_exited = 0;
            }
            else if (WIFCONTINUED(status))
            {
                t->child_stopped[i] = 0;
                all_exited = 0;
            }
            else if (WIFEXITED(status) || WIFSIGNALED(status))
            {
                t->child_exited[i] = 1;
            }
            else
            {
                all_exited = 0;
            }
        }
        else if (result == -1 && errno == ECHILD)
        {
            t->child_exited[i] = 1;
        }
        else
        {
            /* result == 0: no state change to report. */
            all_exited = 0;

            if (result == -1)
                perror("waitpid");
        }
    }
    if (t->running && all_children_stopped_or_exited(t))
    {
        int has_stopped_child = 0;

        for (int i = 0; i < t->child_count; i++)
        {
            if (t->child_stopped[i])
            {
                has_stopped_child = 1;
                break;
            }
        }

        if (has_stopped_child)
        {
            save_stopped_job(t, t->foreground_command);
            return 1;
        }
    }

    /*
     * Finish only when output reached EOF and all children
     * have exited or been reaped.
     */
    if (t->output_eof && all_exited)
    {
        t->running = 0;
        t->child_pid = -1;
        t->child_pid2 = -1;
        t->child1_exited = 1;
        t->child2_exited = 1;
        t->child_count = 0;
        t->foreground_pgid = -1;

        return 1;
    }

    return 0;
}

static int all_children_stopped_or_exited(struct tab *t)
{
    for (int i = 0; i < t->child_count; i++)
    {
        if (!t->child_exited[i] && !t->child_stopped[i])
            return 0;
    }

    return 1;
}

static void save_stopped_job(struct tab *t, const char *command)
{
    if (t->child_count == 0 || t->foreground_pgid <= 0)
        return;

    struct job *job = add_job(t, t->foreground_pgid, command);

    if (job == NULL)
    {
        const char *msg = "\nUnable to save stopped job.\n";
        append_output(t, msg, strlen(msg));
        return;
    }

    job->pipe_fd = t->pipe_fd;
    job->child_count = t->child_count;

    for (int i = 0; i < t->child_count; i++)
    {
        job->child_pids[i] = t->child_pids[i];
        job->child_exited[i] = t->child_exited[i];
        job->child_stopped[i] = t->child_stopped[i];
    }

    job->running = 0;
    job->stopped = 1;

    /* Transfer ownership of the pipe to the job. */
    t->pipe_fd = -1;

    t->child_pid = -1;
    t->child_pid2 = -1;
    t->foreground_pgid = -1;
    t->child_count = 0;
    t->running = 0;

    char msg[INPUT_SIZE + 64];
    snprintf(msg, sizeof(msg), "\n[%d]+ Stopped %s\n",
             job->job_id, job->command);
    append_output(t, msg, strlen(msg));
}

static int extract_background(char *command)
{
    char quote = '\0';
    int escaped = 0;
    int last_non_space = -1;

    for (int i = 0; command[i] != '\0'; i++)
    {
        char c = command[i];

        if (escaped)
        {
            escaped = 0;
            if (c != ' ' && c != '\t')
                last_non_space = i;
            continue;
        }

        if (c == '\\' && quote != '\'')
        {
            escaped = 1;
            if (c != ' ' && c != '\t')
                last_non_space = i;
            continue;
        }

        if (quote != '\0')
        {
            if (c == quote)
                quote = '\0';

            if (c != ' ' && c != '\t')
                last_non_space = i;

            continue;
        }

        if (c == '\'' || c == '"')
        {
            quote = c;
            last_non_space = i;
            continue;
        }

        if (c != ' ' && c != '\t')
            last_non_space = i;
    }

    if (quote != '\0' || escaped || last_non_space < 0 ||
        command[last_non_space] != '&')
        return 0;

    command[last_non_space] = '\0';

    while (last_non_space > 0 &&
           (command[last_non_space - 1] == ' ' ||
            command[last_non_space - 1] == '\t'))
    {
        command[--last_non_space] = '\0';
    }

    return 1;
}

static void reap_background_jobs(struct tab *t)
{
    for (int j = 0; j < MAX_JOBS; j++)
    {
        struct job *job = &t->jobs[j];

        if (!job->active)
            continue;

        int all_exited = 1;

        for (int k = 0; k < job->child_count; k++)
        {
            if (job->child_exited[k])
                continue;

            int status;
            pid_t result;

            do
            {
                result = waitpid(job->child_pids[k],
                                 &status, WNOHANG);
            } while (result == -1 && errno == EINTR);

            if (result == job->child_pids[k] ||
                (result == -1 && errno == ECHILD))
            {
                job->child_exited[k] = 1;
            }
            else
            {
                all_exited = 0;

                if (result == -1)
                    perror("waitpid background");
            }
        }

        if (all_exited)
            job->running = 0;

        if (all_exited && job->pipe_fd < 0)
        {
            char msg[INPUT_SIZE + 64];

            snprintf(msg, sizeof(msg),
                     "\n[%d] completed: %s\n",
                     job->job_id, job->command);

            append_output(t, msg, strlen(msg));
            remove_job(t, job);
        }
    }
}

static int foreground_job(struct tab *t, int job_id)
{
    if (t->running)
    {
        const char *msg =
            "A foreground command is already running.\n";
        append_output(t, msg, strlen(msg));
        return -1;
    }

    struct job *job = NULL;

    for (int i = 0; i < MAX_JOBS; i++)
    {
        if (t->jobs[i].active &&
            t->jobs[i].job_id == job_id)
        {
            job = &t->jobs[i];
            break;
        }
    }

    if (job == NULL)
    {
        const char *msg = "fg: no such job\n";
        append_output(t, msg, strlen(msg));
        return -1;
    }

    if (!job->running && !job->stopped)
    {
        const char *msg = "fg: job has already finished\n";
        append_output(t, msg, strlen(msg));
        return -1;
    }

    t->foreground_pgid = job->pgid;
    t->child_count = job->child_count;

    for (int i = 0; i < job->child_count; i++)
    {
        t->child_pids[i] = job->child_pids[i];
        t->child_exited[i] = job->child_exited[i];
    }

    t->pipe_fd = job->pipe_fd;
    t->output_eof = (job->pipe_fd < 0);
    t->running = 1;

    /* Transfer pipe ownership before remove_job() closes it. */
    job->pipe_fd = -1;

    if (job->stopped)
    {
        if (kill(-job->pgid, SIGCONT) == -1)
        {
            perror("fg: SIGCONT");
            t->running = 0;
            return -1;
        }

        job->stopped = 0;
        job->running = 1;
    }

    remove_job(t, job);

    return 0;
}

static int background_job(struct tab *t, int job_id)
{
    struct job *job = NULL;

    for (int i = 0; i < MAX_JOBS; i++)
    {
        if (t->jobs[i].active &&
            t->jobs[i].job_id == job_id)
        {
            job = &t->jobs[i];
            break;
        }
    }

    if (job == NULL)
    {
        const char *msg = "bg: no such job\n";
        append_output(t, msg, strlen(msg));
        return -1;
    }

    if (!job->stopped)
    {
        const char *msg = "bg: job is not stopped\n";
        append_output(t, msg, strlen(msg));
        return -1;
    }

    if (kill(-job->pgid, SIGCONT) == -1)
    {
        perror("bg: SIGCONT");
        return -1;
    }

    for (int i = 0; i < job->child_count; i++)
    {
        if (!job->child_exited[i])
            job->child_stopped[i] = 0;
    }

    job->stopped = 0;
    job->running = 1;

    char msg[INPUT_SIZE + 64];
    snprintf(msg, sizeof(msg),
             "[%d] continued in background: %s\n",
             job->job_id, job->command);

    append_output(t, msg, strlen(msg));

    return 0;
}

static void list_jobs(struct tab *t)
{
    int found = 0;
    char line[INPUT_SIZE + 64];

    for (int i = 0; i < MAX_JOBS; i++)
    {
        struct job *job = &t->jobs[i];

        if (!job->active)
            continue;

        found = 1;

        const char *status;

        if (job->stopped)
            status = "Stopped";
        else if (job->running)
            status = "Running";
        else
            status = "Done";

        snprintf(line, sizeof(line),
                 "[%d] %-8s %s\n",
                 job->job_id, status, job->command);

        append_output(t, line, strlen(line));
    }

    if (!found)
    {
        const char *msg = "No active background jobs.\n";
        append_output(t, msg, strlen(msg));
    }
}

static int command_is_incomplete(const char *command)
{
    char quote = '\0';
    int paren_depth = 0;
    int in_backticks = 0;
    int escaped = 0;

    for (int i = 0; command[i] != '\0'; i++)
    {
        char c = command[i];

        if (escaped)
        {
            escaped = 0;
            continue;
        }

        if (c == '\\' && quote != '\'')
        {
            if (command[i + 1] == '\0')
                return 1;

            escaped = 1;
            continue;
        }

        if (quote == '\'')
        {
            if (c == '\'')
                quote = '\0';
            continue;
        }

        if (c == '`')
        {
            in_backticks = !in_backticks;
            continue;
        }

        if (quote == '"')
        {
            if (c == '"')
                quote = '\0';
            else if (c == '$' && command[i + 1] == '(')
            {
                paren_depth++;
                i++;
            }
            else if (c == '(' && paren_depth > 0)
                paren_depth++;
            else if (c == ')' && paren_depth > 0)
                paren_depth--;

            continue;
        }

        if (c == '\'' || c == '"')
        {
            quote = c;
            continue;
        }

        if (c == '$' && command[i + 1] == '(')
        {
            paren_depth++;
            i++;
        }
        else if (c == '(' && paren_depth > 0)
            paren_depth++;
        else if (c == ')' && paren_depth > 0)
            paren_depth--;
    }

    return quote != '\0' || paren_depth > 0 || in_backticks;
}

#define MAX_MULTIWATCH_COMMANDS 8
#define MULTIWATCH_INTERVAL_SECONDS 2

struct watch_entry
{
    char command[INPUT_SIZE];
    pid_t pid;
    int fd;
    time_t restart_at;
};

/* Parse: multiWatch ["cmd one", "cmd two"] */
static int parse_multiwatch(const char *input,
                            char commands[][INPUT_SIZE], int max_commands)
{
    const char *p = input;
    while (isspace((unsigned char)*p))
        p++;
    if (strncmp(p, "multiWatch", 10) != 0 ||
        (p[10] != '\0' && !isspace((unsigned char)p[10]) && p[10] != '['))
        return 0;
    p += 10;
    while (isspace((unsigned char)*p))
        p++;
    if (*p++ != '[')
        return -1;
    int count = 0;
    for (;;)
    {
        while (isspace((unsigned char)*p))
            p++;
        if (*p == ']')
        {
            p++;
            while (isspace((unsigned char)*p))
                p++;
            return (*p == '\0' && count > 0) ? count : -1;
        }
        if (*p != '"' || count >= max_commands)
            return -1;
        p++;
        size_t used = 0;
        while (*p && *p != '"')
        {
            char c = *p++;
            if (c == '\\' && *p)
            {
                char next = *p;
                if (next == '"' || next == '\\')
                {
                    c = next;
                    p++;
                }
                else
                {
                    if (used + 1 >= INPUT_SIZE)
                        return -1;
                    commands[count][used++] = c;
                    continue;
                }
            }
            if (used + 1 >= INPUT_SIZE)
                return -1;
            commands[count][used++] = c;
        }
        if (*p != '"' || used == 0)
            return -1;
        p++;
        commands[count][used] = '\0';
        count++;
        while (isspace((unsigned char)*p))
            p++;
        if (*p == ',')
        {
            p++;
            continue;
        }
        if (*p == ']')
            continue;
        return -1;
    }
}

/* Execute a watched command inside the supervisor. Pipeline children inherit
 * the supervisor's process group so Ctrl+C reaches the entire multiWatch tree. */
static void run_watch_command(const char *command, int output_fd, const char *cwd)
{
    char parts[MAX_COMMANDS][INPUT_SIZE];
    int count = split_pipeline(command, parts, MAX_COMMANDS);
    if (count <= 0)
        _exit(2);
    if (chdir(cwd) == -1)
    {
        dprintf(output_fd, "multiWatch: chdir: %s\n", strerror(errno));
        _exit(1);
    }
    if (count == 1)
    {
        if (dup2(output_fd, STDOUT_FILENO) == -1 || dup2(output_fd, STDERR_FILENO) == -1)
            _exit(127);
        close(output_fd);
        char copy[INPUT_SIZE];
        snprintf(copy, sizeof(copy), "%s", parts[0]);
        execute_child_command(copy);
    }

    int pipes[MAX_COMMANDS - 1][2];
    pid_t pids[MAX_COMMANDS];
    int made = 0;
    for (int i = 0; i < count - 1; i++)
        if (pipe(pipes[i]) == -1)
        {
            dprintf(output_fd, "multiWatch: pipe: %s\n", strerror(errno));
            _exit(1);
        }

    for (int i = 0; i < count; i++)
    {
        pid_t pid = fork();
        if (pid == -1)
        {
            dprintf(output_fd, "multiWatch: fork: %s\n", strerror(errno));
            for (int j = 0; j < made; j++)
                kill(pids[j], SIGTERM);
            for (int j = 0; j < made; j++)
                while (waitpid(pids[j], NULL, 0) < 0 && errno == EINTR)
                {
                }
            _exit(1);
        }
        if (pid == 0)
        {
            if (i > 0 && dup2(pipes[i - 1][0], STDIN_FILENO) == -1)
                _exit(127);
            int out = (i < count - 1) ? pipes[i][1] : output_fd;
            if (dup2(out, STDOUT_FILENO) == -1 || dup2(output_fd, STDERR_FILENO) == -1)
                _exit(127);
            for (int j = 0; j < count - 1; j++)
            {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            if (output_fd > STDERR_FILENO)
                close(output_fd);
            char copy[INPUT_SIZE];
            snprintf(copy, sizeof(copy), "%s", parts[i]);
            execute_child_command(copy);
        }
        pids[made++] = pid;
    }
    for (int i = 0; i < count - 1; i++)
    {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    close(output_fd);
    for (int i = 0; i < made; i++)
        while (waitpid(pids[i], NULL, 0) < 0 && errno == EINTR)
        {
        }
    _exit(0);
}

static pid_t launch_watch_entry(struct watch_entry *entry, const char *cwd)
{
    int fd[2];
    if (pipe(fd) == -1)
        return -1;
    pid_t pid = fork();
    if (pid == -1)
    {
        close(fd[0]);
        close(fd[1]);
        return -1;
    }
    if (pid == 0)
    {
        close(fd[0]);
        run_watch_command(entry->command, fd[1], cwd);
    }
    close(fd[1]);
    int flags = fcntl(fd[0], F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd[0], F_SETFL, flags | O_NONBLOCK);
    entry->pid = pid;
    entry->fd = fd[0];
    time_t now = time(NULL);
    dprintf(STDOUT_FILENO, "\"%s\", %lld:\n----------------------------------------------------\n",
            entry->command, (long long)now);
    return pid;
}

static void multiwatch_supervisor(char commands[][INPUT_SIZE], int count,
                                  const char *cwd)
{
    struct watch_entry entries[MAX_MULTIWATCH_COMMANDS];
    memset(entries, 0, sizeof(entries));
    for (int i = 0; i < count; i++)
    {
        snprintf(entries[i].command, sizeof(entries[i].command), "%s", commands[i]);
        entries[i].pid = -1;
        entries[i].fd = -1;
        if (launch_watch_entry(&entries[i], cwd) == -1)
            dprintf(STDOUT_FILENO, "multiWatch: failed to start: %s\n", entries[i].command);
    }

    for (;;)
    {
        struct pollfd pfds[MAX_MULTIWATCH_COMMANDS];
        int map[MAX_MULTIWATCH_COMMANDS];
        nfds_t nfds = 0;
        time_t now = time(NULL);
        for (int i = 0; i < count; i++)
        {
            if (entries[i].fd >= 0)
            {
                pfds[nfds] = (struct pollfd){entries[i].fd, POLLIN | POLLHUP | POLLERR, 0};
                map[nfds++] = i;
            }
        }
        int timeout = 500;
        for (int i = 0; i < count; i++)
            if (entries[i].pid == -1 && entries[i].restart_at > now)
            {
                long ms = (long)(entries[i].restart_at - now) * 1000L;
                if (ms < timeout)
                    timeout = (int)ms;
            }
        int ready = poll(pfds, nfds, timeout);
        if (ready < 0 && errno != EINTR)
            break;
        for (nfds_t k = 0; k < nfds; k++)
        {
            if (!(pfds[k].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            int i = map[k];
            char buffer[1024];
            ssize_t n;
            do
            {
                n = read(entries[i].fd, buffer, sizeof(buffer));
                if (n > 0)
                {
                    ssize_t off = 0;
                    while (off < n)
                    {
                        ssize_t w = write(STDOUT_FILENO, buffer + off, (size_t)(n - off));
                        if (w < 0)
                        {
                            if (errno == EINTR)
                                continue;
                            break;
                        }
                        off += w;
                    }
                }
            } while (n > 0);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            {
                close(entries[i].fd);
                entries[i].fd = -1;
                if (entries[i].pid > 0)
                    while (waitpid(entries[i].pid, NULL, 0) < 0 && errno == EINTR)
                    {
                    }
                entries[i].pid = -1;
                entries[i].restart_at = time(NULL) + MULTIWATCH_INTERVAL_SECONDS;
                dprintf(STDOUT_FILENO, "\n----------------------------------------------------\n");
            }
        }
        now = time(NULL);
        for (int i = 0; i < count; i++)
        {
            if (entries[i].pid == -1 && entries[i].restart_at > 0 && now >= entries[i].restart_at)
            {
                if (launch_watch_entry(&entries[i], cwd) == -1)
                {
                    dprintf(STDOUT_FILENO, "multiWatch: failed to restart: %s\n", entries[i].command);
                    entries[i].restart_at = now + MULTIWATCH_INTERVAL_SECONDS;
                }
            }
        }
    }
    _exit(0);
}

static int start_multiwatch(struct tab *t, char commands[][INPUT_SIZE], int count,
                            const char *original_command)
{
    int fd[2];
    if (pipe(fd) == -1)
    {
        perror("multiWatch pipe");
        return -1;
    }
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("multiWatch fork");
        close(fd[0]);
        close(fd[1]);
        return -1;
    }
    if (pid == 0)
    {
        if (setpgid(0, 0) == -1)
            _exit(127);
        close(fd[0]);
        if (dup2(fd[1], STDOUT_FILENO) == -1 || dup2(fd[1], STDERR_FILENO) == -1)
            _exit(127);
        close(fd[1]);
        multiwatch_supervisor(commands, count, t->cwd);
    }
    if (setpgid(pid, pid) == -1 && errno != EACCES && errno != ESRCH)
        perror("multiWatch setpgid");
    close(fd[1]);
    t->pipe_fd = fd[0];
    t->child_pid = pid;
    t->child_pid2 = -1;
    t->child_count = 1;
    t->child_pids[0] = pid;
    t->child_exited[0] = 0;
    t->child1_exited = 0;
    t->child2_exited = 1;
    t->output_eof = 0;
    t->foreground_pgid = pid;
    t->running = 1;
    snprintf(t->foreground_command, sizeof(t->foreground_command), "%s", original_command);
    return 0;
}

/* Execute one submitted command. This routine deliberately does not call Xlib. */
/* Accept both assignment-style job specs (%%1) and plain IDs (1). */
static int parse_job_id(const char *arg, int *job_id)
{
    if (arg == NULL || job_id == NULL)
        return -1;

    if (*arg == '%')
        arg++;
    if (*arg == '\0')
        return -1;

    errno = 0;
    char *endptr = NULL;
    long id = strtol(arg, &endptr, 10);
    if (errno != 0 || endptr == arg || *endptr != '\0' ||
        id <= 0 || id > INT_MAX)
        return -1;

    *job_id = (int)id;
    return 0;
}

static void execute_tab_command(struct tab *t, const char *submitted,
                                struct shared_history *history)
{
    char command_copy[INPUT_SIZE];
    snprintf(command_copy, sizeof(command_copy), "%s", submitted);
    history_add(history, command_copy);
    if (t->running)
    {
        const char *msg = "Wait for the current command to finish.\n";
        append_output(t, msg, strlen(msg));
        return;
    }

    char watch_commands[MAX_MULTIWATCH_COMMANDS][INPUT_SIZE];
    int watch_count = parse_multiwatch(command_copy, watch_commands, MAX_MULTIWATCH_COMMANDS);
    if (watch_count != 0)
    {
        if (watch_count < 0)
        {
            const char *msg = "Usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n";
            append_output(t, msg, strlen(msg));
            return;
        }
        if (start_multiwatch(t, watch_commands, watch_count, command_copy) == -1)
        {
            const char *msg = "multiWatch: failed to start\n";
            append_output(t, msg, strlen(msg));
        }
        return;
    }

    int background = extract_background(command_copy);
    if (expand_environment_variables(command_copy, sizeof(command_copy)) == -1)
        return;
    if (expand_command_substitutions(command_copy, sizeof(command_copy), t->cwd) == -1)
        return;

    char commands[MAX_COMMANDS][INPUT_SIZE];
    int command_count = split_pipeline(command_copy, commands, MAX_COMMANDS);
    if (command_count < 0)
    {
        const char *msg = "terminator: invalid pipeline syntax\n";
        append_output(t, msg, strlen(msg));
        return;
    }

    if (command_count == 1)
    {
        char parse_copy[INPUT_SIZE];
        snprintf(parse_copy, sizeof(parse_copy), "%s", command_copy);
        char *argv[INPUT_SIZE / 2];
        int argc = parse_command(parse_copy, argv,
                                 (int)(sizeof(argv) / sizeof(argv[0])));
        if (argc < 0)
        {
            const char *msg = "terminator: invalid command syntax\n";
            append_output(t, msg, strlen(msg));
        }
        else if (argc == 0)
        {
            return;
        }
        else if (strcmp(argv[0], "cd") == 0)
        {
            if (argc > 2)
            {
                const char *msg = "cd: too many arguments\n";
                append_output(t, msg, strlen(msg));
            }
            else
                change_directory(t, argc == 2 ? argv[1] : NULL);
        }
        else if (argc == 1 && strcmp(argv[0], "history") == 0)
        {
            char entry[INPUT_SIZE];
            int count = history_count(history);
            int shown = count < 1000 ? count : 1000;
            for (int i = shown - 1; i >= 0; i--)
            {
                if (history_get(history, i, entry, sizeof(entry)) == 0)
                {
                    char line[INPUT_SIZE + 32];
                    snprintf(line, sizeof(line), "%d  %s\n", shown - i, entry);
                    append_output(t, line, strlen(line));
                }
            }
        }
        else if (argc == 1 && strcmp(argv[0], "jobs") == 0)
            list_jobs(t);
        else if (argc >= 1 && strcmp(argv[0], "bg") == 0)
        {
            if (argc != 2)
            {
                const char *msg = "Usage: bg <job_id>\n";
                append_output(t, msg, strlen(msg));
            }
            else
            {
                int id;
                if (parse_job_id(argv[1], &id) == -1)
                {
                    const char *msg = "bg: invalid job ID (use %1 or 1)\n";
                    append_output(t, msg, strlen(msg));
                }
                else
                    background_job(t, id);
            }
        }
        else if (argc >= 1 && strcmp(argv[0], "fg") == 0)
        {
            if (argc != 2)
            {
                const char *msg = "Usage: fg <job_id>\n";
                append_output(t, msg, strlen(msg));
            }
            else
            {
                int id;
                if (parse_job_id(argv[1], &id) == -1)
                {
                    const char *msg = "fg: invalid job ID (use %1 or 1)\n";
                    append_output(t, msg, strlen(msg));
                }
                else
                    foreground_job(t, id);
            }
        }
        else if (start_command(t, background, command_copy) == 0)
        {
            if (!background)
                t->running = 1;
            else
            {
                char msg[INPUT_SIZE + 64];
                snprintf(msg, sizeof(msg), "[%d] started in background\n", t->next_job_id);
                append_output(t, msg, strlen(msg));
            }
        }
        else
        {
            const char *msg = "terminator: failed to start command\n";
            append_output(t, msg, strlen(msg));
        }
    }
    else if (start_pipeline(t, commands, command_count, background, command_copy) == -1)
    {
        const char *msg = "terminator: failed to start pipeline\n";
        append_output(t, msg, strlen(msg));
    }
    else if (background)
    {
        char msg[INPUT_SIZE + 64];
        snprintf(msg, sizeof(msg), "[%d] started in background\n", t->next_job_id);
        append_output(t, msg, strlen(msg));
    }
}

static void *tab_worker_main(void *arg)
{
    struct tab *t = (struct tab *)arg;
    for (;;)
    {
        pthread_mutex_lock(&t->worker_mutex);
        while (!t->worker_command_pending && !t->worker_stop)
            pthread_cond_wait(&t->worker_cond, &t->worker_mutex);
        if (t->worker_stop && !t->worker_command_pending)
        {
            pthread_mutex_unlock(&t->worker_mutex);
            break;
        }
        char command[INPUT_SIZE];
        snprintf(command, sizeof(command), "%s", t->worker_command);
        t->worker_command_pending = 0;
        pthread_mutex_unlock(&t->worker_mutex);

        execute_tab_command(t, command, t->history);
    }
    return NULL;
}

static int start_tab_worker(struct tab *t, struct shared_history *history)
{
    t->history = history;
    if (pthread_mutex_init(&t->worker_mutex, NULL) != 0)
        return -1;
    if (pthread_cond_init(&t->worker_cond, NULL) != 0)
    {
        pthread_mutex_destroy(&t->worker_mutex);
        return -1;
    }
    t->worker_stop = 0;
    t->worker_command_pending = 0;
    if (pthread_create(&t->worker_thread, NULL, tab_worker_main, t) != 0)
    {
        pthread_cond_destroy(&t->worker_cond);
        pthread_mutex_destroy(&t->worker_mutex);
        return -1;
    }
    t->worker_started = 1;
    return 0;
}

static void stop_tab_worker(struct tab *t)
{
    if (!t->worker_started)
        return;
    pthread_mutex_lock(&t->worker_mutex);
    t->worker_stop = 1;
    t->worker_command_pending = 0; /* discard work not yet started during shutdown */
    pthread_cond_signal(&t->worker_cond);
    pthread_mutex_unlock(&t->worker_mutex);
    pthread_join(t->worker_thread, NULL);
    pthread_cond_destroy(&t->worker_cond);
    pthread_mutex_destroy(&t->worker_mutex);
    t->worker_started = 0;
}

static int submit_tab_command(struct tab *t, const char *command)
{
    if (!t->worker_started)
        return -1;
    pthread_mutex_lock(&t->worker_mutex);
    if (t->worker_command_pending || t->worker_stop)
    {
        pthread_mutex_unlock(&t->worker_mutex);
        return -1;
    }
    snprintf(t->worker_command, sizeof(t->worker_command), "%s", command);
    t->worker_command_pending = 1;
    pthread_cond_signal(&t->worker_cond);
    pthread_mutex_unlock(&t->worker_mutex);
    return 0;
}

/* Read one complete fixed-size command message from a pipe. */
static int read_command_message(int fd, char command[INPUT_SIZE])
{
    size_t have = 0;
    while (have < INPUT_SIZE)
    {
        ssize_t n = read(fd, command + have, INPUT_SIZE - have);
        if (n == 0)
            return 0;
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        have += (size_t)n;
    }
    command[INPUT_SIZE - 1] = '\0';
    return 1;
}

/* Each process-mode tab child runs its own command dispatcher and reaps its jobs.
 * It never calls Xlib; output is written to shared tab state and notifications go
 * to the GUI parent through a pipe. */
static void process_tab_worker_main(struct tab *t, int command_fd, int notify_fd)
{
    process_worker_notify_fd = notify_fd;
    (void)fcntl(notify_fd, F_SETFL, fcntl(notify_fd, F_GETFL, 0) | O_NONBLOCK);

    for (;;)
    {
        struct pollfd pfds[2 + MAX_JOBS];
        int kinds[2 + MAX_JOBS]; //-1 command pipe, -2 foreground, >=0 job slot
        int count = 0;
        pfds[count] = (struct pollfd){command_fd, POLLIN | POLLHUP, 0};
        kinds[count++] = -1;
        if (t->running && t->pipe_fd >= 0)
        {
            pfds[count] = (struct pollfd){t->pipe_fd, POLLIN | POLLHUP | POLLERR, 0};
            kinds[count++] = -2;
        }
        for (int j = 0; j < MAX_JOBS; ++j)
        {
            if (t->jobs[j].active && t->jobs[j].pipe_fd >= 0)
            {
                pfds[count] = (struct pollfd){t->jobs[j].pipe_fd, POLLIN | POLLHUP | POLLERR, 0};
                kinds[count++] = j;
            }
        }

        int ready = poll(pfds, (nfds_t)count, 100);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            perror("tab worker poll");
            break;
        }

        int shutdown_requested = 0;
        for (int i = 0; i < count; ++i)
        {
            if (!pfds[i].revents)
                continue;
            if (kinds[i] == -1)
            {
                if (pfds[i].revents & POLLIN)
                {
                    char command[INPUT_SIZE];
                    int rc = read_command_message(command_fd, command);
                    if (rc <= 0)
                        shutdown_requested = 1;
                    else
                        execute_tab_command(t, command, t->history);
                }
                else if (pfds[i].revents & (POLLHUP | POLLERR))
                    shutdown_requested = 1;
                continue;
            }

            int *fdp = (kinds[i] == -2) ? &t->pipe_fd : &t->jobs[kinds[i]].pipe_fd;
            if (*fdp < 0)
                continue;
            char buffer[512];
            ssize_t n = read(*fdp, buffer, sizeof(buffer));
            if (n > 0)
                append_output(t, buffer, (size_t)n);
            else if (n == 0 || (errno != EINTR && errno != EAGAIN && n < 0))
            {
                close(*fdp);
                *fdp = -1;
                if (kinds[i] == -2)
                    t->output_eof = 1;
            }
        }

        if (t->running)
            (void)reap_children(t);
        reap_background_jobs(t);
        // notify_process_parent();

        if (shutdown_requested)
            break;
    }

    /* A tab worker owns its command children. On shutdown, terminate and reap them. */
    if (t->foreground_pgid > 0)
        (void)kill(-t->foreground_pgid, SIGTERM);
    for (int i = 0; i < t->child_count; ++i)
        if (t->child_pids[i] > 0 && !t->child_exited[i])
            (void)kill(t->child_pids[i], SIGTERM);
    for (int j = 0; j < MAX_JOBS; ++j)
    {
        if (t->jobs[j].active && t->jobs[j].pgid > 0)
            (void)kill(-t->jobs[j].pgid, SIGTERM);
        for (int k = 0; k < t->jobs[j].child_count; ++k)
            if (t->jobs[j].child_pids[k] > 0 && !t->jobs[j].child_exited[k])
                (void)kill(t->jobs[j].child_pids[k], SIGTERM);
    }
    for (int i = 0; i < t->child_count; ++i)
        if (t->child_pids[i] > 0)
            while (waitpid(t->child_pids[i], NULL, 0) < 0 && errno == EINTR)
            {
            }
    for (int j = 0; j < MAX_JOBS; ++j)
        for (int k = 0; k < t->jobs[j].child_count; ++k)
            if (t->jobs[j].child_pids[k] > 0)
                while (waitpid(t->jobs[j].child_pids[k], NULL, 0) < 0 && errno == EINTR)
                {
                }
    if (t->pipe_fd >= 0)
        close(t->pipe_fd);
    t->pipe_fd = -1;
    for (int j = 0; j < MAX_JOBS; ++j)
    {
        if (t->jobs[j].pipe_fd >= 0)
            close(t->jobs[j].pipe_fd);
        t->jobs[j].pipe_fd = -1;
        t->jobs[j].active = 0;
    }
    t->running = 0;
    t->child_pid = -1;
    t->child_pid2 = -1;
    t->foreground_pgid = -1;
    t->child_count = 0;
    t->output_eof = 0;
    close(command_fd);
    close(notify_fd);
    _exit(0);
}

static int start_process_tab_worker(struct tab *t, struct tab *all_tabs,
                                    int tab_count, struct shared_history *history,
                                    int xfd)
{
    int commands[2] = {-1, -1};
    int notifications[2] = {-1, -1};
    if (pipe(commands) == -1 || pipe(notifications) == -1)
    {
        perror("pipe tab worker");
        if (commands[0] >= 0)
        {
            close(commands[0]);
            close(commands[1]);
        }
        if (notifications[0] >= 0)
        {
            close(notifications[0]);
            close(notifications[1]);
        }
        return -1;
    }
    t->history = history;
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork tab worker");
        close(commands[0]);
        close(commands[1]);
        close(notifications[0]);
        close(notifications[1]);
        return -1;
    }
    if (pid == 0)
    {
        close(commands[1]);
        close(notifications[0]);
        if (xfd >= 0)
            close(xfd); /* The child never owns or uses X11. */
        for (int i = 0; i < tab_count; ++i)
        {
            if (&all_tabs[i] == t)
                continue;
            if (all_tabs[i].worker_command_fd >= 0)
                close(all_tabs[i].worker_command_fd);
            if (all_tabs[i].worker_notify_fd >= 0)
                close(all_tabs[i].worker_notify_fd);
        }
        if (t->cwd[0] != '\0' && chdir(t->cwd) == -1)
            perror("tab worker chdir");
        process_tab_worker_main(t, commands[0], notifications[1]);
        _exit(0);
    }
    close(commands[0]);
    close(notifications[1]);
    (void)fcntl(notifications[0], F_SETFL, fcntl(notifications[0], F_GETFL, 0) | O_NONBLOCK);
    t->worker_pid = pid;
    t->worker_command_fd = commands[1];
    t->worker_notify_fd = notifications[0];
    return 0;
}

static int submit_process_tab_command(struct tab *t, const char *command)
{
    if (t->worker_pid <= 0 || t->worker_command_fd < 0)
        return -1;
    char message[INPUT_SIZE] = {0};
    snprintf(message, sizeof(message), "%s", command);
    size_t sent = 0;
    while (sent < sizeof(message))
    {
        ssize_t n = write(t->worker_command_fd, message + sent, sizeof(message) - sent);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static void stop_process_tab_worker(struct tab *t)
{
    if (t->worker_command_fd >= 0)
    {
        close(t->worker_command_fd); /* EOF tells the worker to shut down. */
        t->worker_command_fd = -1;
    }
    if (t->worker_pid > 0)
    {
        int status;
        pid_t r;
        do
        {
            r = waitpid(t->worker_pid, &status, 0);
        } while (r < 0 && errno == EINTR);
        if (r < 0 && errno != ECHILD)
            perror("waitpid tab worker");
        t->worker_pid = -1;
    }
    if (t->worker_notify_fd >= 0)
    {
        close(t->worker_notify_fd);
        t->worker_notify_fd = -1;
    }
}

int main(int argc, char *argv[])
{
    int use_process = 0;
    if (argc == 2 && strcmp(argv[1], "useProcess=True") == 0)
    {
        use_process = 1;
    }
    else if (argc != 1)
    {
        fprintf(stderr, "Usage: %s [useProcess=True]\n", argv[0]);
        return 1;
    }
    // fprintf(stderr, "Execution mode: %s\n",
    //     use_process ? "process" : "thread");
    signal(SIGPIPE, SIG_IGN); /* A closed tab worker must not terminate the GUI. */
    Display *display = XOpenDisplay(NULL);
    if (display == NULL)
    {
        fprintf(stderr, "Failed to connect to X server\n");
        return 1;
    }

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    Window win = XCreateSimpleWindow(
        display, root, 100, 100, 700, 650, 1,
        WhitePixel(display, screen),
        BlackPixel(display, screen));
    XSelectInput(display, win,
                 ExposureMask | KeyPressMask | ButtonPressMask);
    GC gc = XCreateGC(display, win, 0, NULL);

    XColor green_color;
    Colormap cmap = DefaultColormap(display, screen);
    if (XParseColor(display, cmap, "#00FF00", &green_color) &&
        XAllocColor(display, cmap, &green_color))
    {
        XSetForeground(display, gc, green_color.pixel);
    }

    XMapWindow(display, win);
    XFlush(display);
    int xfd = ConnectionNumber(display);

    struct tab *tabs;
    if (use_process)
    {
        tabs = mmap(NULL, sizeof(struct tab) * MAX_TABS,
                    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (tabs == MAP_FAILED)
        {
            perror("mmap shared tabs");
            XFreeGC(display, gc);
            XDestroyWindow(display, win);
            XCloseDisplay(display);
            return 1;
        }
        memset(tabs, 0, sizeof(struct tab) * MAX_TABS);
    }
    else
    {
        static struct tab thread_tabs[MAX_TABS];
        tabs = thread_tabs;
        memset(tabs, 0, sizeof(struct tab) * MAX_TABS);
    }
    int tab_count = 1;
    int active_tab = 0;
    tabs[0].child_pid = -1;
    tabs[0].pipe_fd = -1;
    tabs[0].child2_exited = 1;
    tabs[0].worker_pid = -1;
    tabs[0].worker_command_fd = -1;
    tabs[0].worker_notify_fd = -1;
    tabs[0].history_nav = -1;
    for (int ti = 1; ti < MAX_TABS; ++ti)
    {
        tabs[ti].worker_pid = -1;
        tabs[ti].worker_command_fd = -1;
        tabs[ti].worker_notify_fd = -1;
        tabs[ti].pipe_fd = -1;
    }

    struct shared_history *history;
    if (use_process)
    {
        history = mmap(NULL, sizeof(*history), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (history == MAP_FAILED)
        {
            perror("mmap shared history");
            return 1;
        }
        memset(history, 0, sizeof(*history));
        if (sem_init(&history->lock, 1, 1) == -1)
        {
            perror("sem_init");
            munmap(history, sizeof(*history));
            return 1;
        }
    }
    else
    {
        static struct shared_history thread_history;
        history = &thread_history;
        memset(history, 0, sizeof(*history));
        if (sem_init(&history->lock, 0, 1) == -1)
        {
            perror("sem_init");
            return 1;
        }
    }

    history_load_file(history);

    if (getcwd(tabs[0].cwd, sizeof(tabs[0].cwd)) == NULL)
    {
        perror("getcwd");
        return 1;
    }

    if ((!use_process && start_tab_worker(&tabs[0], history) == -1) ||
        (use_process && start_process_tab_worker(&tabs[0], tabs, tab_count, history, xfd) == -1))
    {
        fprintf(stderr, "Failed to start initial tab worker\n");
        sem_destroy(&history->lock);
        if (use_process)
        {
            munmap(history, sizeof(*history));
            munmap(tabs, sizeof(struct tab) * MAX_TABS);
        }
        XFreeGC(display, gc);
        XDestroyWindow(display, win);
        XCloseDisplay(display);
        return 1;
    }

    for (;;)
    {
        size_t output_len_before[MAX_TABS] = {0};
        for (int oi = 0; oi < tab_count; oi++)
            output_len_before[oi] = tabs[oi].output_len;

        /* fd 0 is the X11 connection; the rest are active tab pipes. */
        struct pollfd fds[MAX_POLL_FDS];
        int fd_to_tab[MAX_POLL_FDS];
        int fd_to_job[MAX_POLL_FDS];
        int nfds = 1;

        fds[0].fd = xfd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fd_to_tab[0] = -1;

        /* In process mode the parent polls each tab worker's notification pipe.
         * The worker itself owns command-output pipes and reaps its children. */
        for (int i = 0; i < tab_count; i++)
        {
            if (use_process)
            {
                if (tabs[i].worker_notify_fd >= 0)
                {
                    fds[nfds] = (struct pollfd){tabs[i].worker_notify_fd, POLLIN | POLLHUP | POLLERR, 0};
                    fd_to_tab[nfds] = i;
                    fd_to_job[nfds] = -2;
                    nfds++;
                }
                continue;
            }
            if (tabs[i].running && tabs[i].pipe_fd >= 0)
            {
                fds[nfds].fd = tabs[i].pipe_fd;
                fds[nfds].events = POLLIN;
                fds[nfds].revents = 0;
                fd_to_tab[nfds] = i;
                fd_to_job[nfds] = -1;
                nfds++;
            }
            for (int j = 0; j < MAX_JOBS; j++)
            {
                struct job *job = &tabs[i].jobs[j];
                if (!job->active || job->pipe_fd < 0)
                    continue;
                fds[nfds].fd = job->pipe_fd;
                fds[nfds].events = POLLIN;
                fds[nfds].revents = 0;
                fd_to_tab[nfds] = i;
                fd_to_job[nfds] = j;
                nfds++;
            }
        }

        int ready = poll(fds, (nfds_t)nfds, 100);
        if (ready == -1)
        {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (fds[0].revents & POLLIN)
        {
            while (XPending(display))
            {
                XEvent event;
                XNextEvent(display, &event);

                if (event.type == Expose && event.xexpose.count == 0)
                {
                    redraw(display, win, gc, &tabs[active_tab],
                           tab_count, active_tab);
                }
                else if (event.type == ButtonPress)
                {
                    struct tab *t = &tabs[active_tab];

                    if (event.xbutton.button == Button4)
                    {
                        /* Scroll upward into older output. */
                        t->output_scroll += 3;
                        t->follow_output = 0;
                    }
                    else if (event.xbutton.button == Button5)
                    {
                        /* Scroll downward toward newer output. */
                        t->output_scroll -= 3;

                        if (t->output_scroll <= 0)
                        {
                            t->output_scroll = 0;
                            t->follow_output = 1;
                        }
                    }

                    redraw(display, win, gc, t, tab_count, active_tab);
                }

                else if (event.type == KeyPress)
                {
                    KeySym key = XLookupKeysym(&event.xkey, 0);
                    int ctrl = (event.xkey.state & ControlMask) != 0;
                    int shift = (event.xkey.state & ShiftMask) != 0;

                    if (ctrl && key == XK_t)
                    {
                        if (tab_count < MAX_TABS)
                        {
                            tabs[tab_count] = (struct tab){0};
                            tabs[tab_count].pipe_fd = -1;
                            tabs[tab_count].child_pid = -1;
                            tabs[tab_count].worker_pid = -1;
                            tabs[tab_count].worker_command_fd = -1;
                            tabs[tab_count].worker_notify_fd = -1;
                            tabs[tab_count].history_nav = -1;
                            if (getcwd(tabs[tab_count].cwd,
                                       sizeof(tabs[tab_count].cwd)) == NULL)
                            {
                                perror("getcwd");
                                tabs[tab_count].cwd[0] = '\0';
                            }
                            int worker_rc = use_process
                                                ? start_process_tab_worker(&tabs[tab_count], tabs, tab_count + 1, history, xfd)
                                                : start_tab_worker(&tabs[tab_count], history);
                            if (worker_rc == -1)
                            {
                                const char *msg = use_process
                                                      ? "Failed to create tab worker process.\n"
                                                      : "Failed to create tab worker thread.\n";
                                append_output(&tabs[active_tab], msg, strlen(msg));
                            }
                            else
                            {
                                active_tab = tab_count;
                                tab_count++;
                                redraw(display, win, gc, &tabs[active_tab],
                                       tab_count, active_tab);
                            }
                        }
                        else
                        {
                            const char *msg = "Maximum number of tabs reached.\n";
                            append_output(&tabs[active_tab], msg, strlen(msg));
                            redraw(display, win, gc, &tabs[active_tab],
                                   tab_count, active_tab);
                        }
                    }
                    else if (ctrl && (key == XK_Tab || key == XK_ISO_Left_Tab))
                    {
                        if (tab_count > 1)
                        {
                            if (shift || key == XK_ISO_Left_Tab)
                            {
                                active_tab = (active_tab + tab_count - 1) % tab_count;
                            }
                            else
                            {
                                active_tab = (active_tab + 1) % tab_count;
                            }
                            redraw(display, win, gc, &tabs[active_tab],
                                   tab_count, active_tab);
                        }
                    }
                    else
                    {
                        struct tab *currentTab = &tabs[active_tab];

                        /* Assignment Task 9: query-based Ctrl+R history search. */
                        if (currentTab->reverse_search)
                        {
                            char chars[32];
                            int n = XLookupString(&event.xkey, chars,
                                                  sizeof(chars), NULL, NULL);

                            if (key == XK_Escape ||
                                (ctrl && (key == XK_g || key == XK_G)))
                            {
                                /* Cancel search and restore the previous input. */
                                currentTab->reverse_search = 0;

                                snprintf(currentTab->command,
                                         sizeof(currentTab->command), "%s",
                                         currentTab->search_saved_command);

                                currentTab->length = (int)strlen(currentTab->command);
                                currentTab->cursor_position = currentTab->length;
                            }
                            else if (key == XK_Return)
                            {
                                if (currentTab->search_match_valid)
                                {
                                    /* Execute the currently selected history command. */
                                    snprintf(currentTab->command,
                                             sizeof(currentTab->command), "%s",
                                             currentTab->search_match);

                                    currentTab->length = (int)strlen(currentTab->command);
                                    currentTab->cursor_position = currentTab->length;

                                    currentTab->reverse_search = 0;

                                    char command_copy[INPUT_SIZE];
                                    snprintf(command_copy, sizeof(command_copy), "%s",
                                             currentTab->command);

                                    append_command_to_history(currentTab);

                                    int submit_rc = use_process
                                                        ? submit_process_tab_command(currentTab, command_copy)
                                                        : submit_tab_command(currentTab, command_copy);

                                    if (submit_rc == -1)
                                    {
                                        const char *msg =
                                            "Tab worker is busy or unavailable; try again.\n";

                                        append_output(currentTab, msg, strlen(msg));
                                    }

                                    currentTab->command[0] = '\0';
                                    currentTab->length = 0;
                                    currentTab->cursor_position = 0;
                                    currentTab->history_nav = -1;
                                }
                                else
                                {
                                    append_output(currentTab,
                                                  "No matching command in history.\n", 34);

                                    currentTab->reverse_search = 0;

                                    snprintf(currentTab->command,
                                             sizeof(currentTab->command), "%s",
                                             currentTab->search_saved_command);

                                    currentTab->length = (int)strlen(currentTab->command);
                                    currentTab->cursor_position = currentTab->length;
                                }
                            }
                            else if (key == XK_BackSpace)
                            {
                                size_t qlen = strlen(currentTab->search_query);

                                if (qlen > 0)
                                    currentTab->search_query[qlen - 1] = '\0';
                            }
                            else if (n > 0 && !ctrl && !shift)
                            {
                                size_t qlen = strlen(currentTab->search_query);

                                if (qlen + (size_t)n < sizeof(currentTab->search_query))
                                {
                                    int printable = 1;

                                    for (int i = 0; i < n; i++)
                                    {
                                        if ((unsigned char)chars[i] < 32 ||
                                            (unsigned char)chars[i] > 126)
                                            printable = 0;
                                    }

                                    if (printable)
                                    {
                                        memcpy(currentTab->search_query + qlen,
                                               chars, (size_t)n);

                                        currentTab->search_query[qlen + (size_t)n] = '\0';
                                    }
                                }
                            }

                            if (currentTab->reverse_search)
                            {
                                history_find_live_match(currentTab);

                                if (currentTab->search_match_valid)
                                {
                                    snprintf(currentTab->command, sizeof(currentTab->command),
                                             "(reverse-i-search)`%.80s': %.150s",
                                             currentTab->search_query,
                                             currentTab->search_match);
                                }
                                else
                                {
                                    snprintf(currentTab->command, sizeof(currentTab->command),
                                             "(reverse-i-search)`%.80s': (no match)",
                                             currentTab->search_query);
                                }

                                currentTab->length = (int)strlen(currentTab->command);
                                currentTab->cursor_position = currentTab->length;
                            }

                            redraw(display, win, gc, currentTab, tab_count, active_tab);
                            continue;
                        }
                        else if (ctrl && (key == XK_r || key == XK_R))
                        {
                            currentTab->reverse_search = 1;
                            currentTab->search_query[0] = '\0';
                            currentTab->search_match[0] = '\0';
                            currentTab->search_match_valid = 0;
                            currentTab->search_offset = 0;
                            snprintf(currentTab->search_saved_command, sizeof(currentTab->search_saved_command), "%s", currentTab->command);
                            snprintf(currentTab->command, sizeof(currentTab->command), "Enter search term: ");
                            currentTab->length = (int)strlen(currentTab->command);
                            currentTab->cursor_position = currentTab->length;
                            redraw(display, win, gc, currentTab, tab_count, active_tab);
                        }
                        /* Line-editing and shell hotkeys. */
                        else if (key == XK_Left)
                        {
                            if (currentTab->cursor_position > 0)
                                currentTab->cursor_position--;
                            redraw(display, win, gc, currentTab, tab_count, active_tab);
                        }
                        else if (key == XK_Right)
                        {
                            if (currentTab->cursor_position < currentTab->length)
                                currentTab->cursor_position++;
                            redraw(display, win, gc, currentTab, tab_count, active_tab);
                        }
                        else if (ctrl && (key == XK_a || key == XK_A))
                        {
                            currentTab->cursor_position = 0;
                            redraw(display, win, gc, currentTab, tab_count, active_tab);
                        }
                        else if (ctrl && (key == XK_e || key == XK_E))
                        {
                            currentTab->cursor_position = currentTab->length;
                            redraw(display, win, gc, currentTab, tab_count, active_tab);
                        }
                        else if (ctrl && (key == XK_u || key == XK_U))
                        {
                            int nremove = currentTab->cursor_position;
                            memmove(currentTab->command,
                                    currentTab->command + nremove,
                                    (size_t)(currentTab->length - nremove) + 1);
                            currentTab->length -= nremove;
                            currentTab->cursor_position = 0;
                            redraw(display, win, gc, currentTab, tab_count, active_tab);
                        }
                        else if (ctrl && (key == XK_k || key == XK_K))
                        {
                            currentTab->length = currentTab->cursor_position;
                            currentTab->command[currentTab->length] = '\0';
                            redraw(display, win, gc, currentTab, tab_count, active_tab);
                        }
                        else if (ctrl && (key == XK_w || key == XK_W))
                        {
                            int start = currentTab->cursor_position;
                            while (start > 0 && currentTab->command[start - 1] == ' ')
                                start--;
                            while (start > 0 && currentTab->command[start - 1] != ' ' &&
                                   currentTab->command[start - 1] != '\n' &&
                                   currentTab->command[start - 1] != '\t')
                                start--;
                            memmove(currentTab->command + start,
                                    currentTab->command + currentTab->cursor_position,
                                    (size_t)(currentTab->length - currentTab->cursor_position) + 1);
                            currentTab->length -= currentTab->cursor_position - start;
                            currentTab->cursor_position = start;
                            redraw(display, win, gc, currentTab, tab_count, active_tab);
                        }
                        else if (ctrl && (key == XK_l || key == XK_L))
                        {
                            currentTab->output[0] = '\0';
                            currentTab->output_len = 0;
                            currentTab->output_scroll = 0;
                            currentTab->follow_output = 1;
                            redraw(display, win, gc, currentTab, tab_count, active_tab);
                        }
                        else if (key == XK_Up)
                        {
                            if (currentTab->history_nav == -1)
                            {
                                snprintf(currentTab->history_draft, sizeof(currentTab->history_draft), "%s", currentTab->command);
                                currentTab->history_nav = 0;
                            }
                            else
                                currentTab->history_nav++;
                            char hist[INPUT_SIZE];
                            if (currentTab->history && history_get(currentTab->history, currentTab->history_nav, hist, sizeof(hist)) == 0)
                            {
                                snprintf(currentTab->command, sizeof(currentTab->command), "%s", hist);
                                currentTab->length = (int)strlen(currentTab->command);
                                currentTab->cursor_position = currentTab->length;
                            }
                            else if (currentTab->history_nav > 0)
                                currentTab->history_nav--;
                            redraw(display, win, gc, currentTab, tab_count, active_tab);
                        }
                        else if (key == XK_Down)
                        {
                            if (currentTab->history_nav > 0)
                            {
                                currentTab->history_nav--;
                                char hist[INPUT_SIZE];
                                if (history_get(currentTab->history, currentTab->history_nav, hist, sizeof(hist)) == 0)
                                {
                                    snprintf(currentTab->command, sizeof(currentTab->command), "%s", hist);
                                    currentTab->length = (int)strlen(currentTab->command);
                                    currentTab->cursor_position = currentTab->length;
                                }
                            }
                            else if (currentTab->history_nav == 0)
                            {
                                currentTab->history_nav = -1;
                                snprintf(currentTab->command, sizeof(currentTab->command), "%s", currentTab->history_draft);
                                currentTab->length = (int)strlen(currentTab->command);
                                currentTab->cursor_position = currentTab->length;
                            }
                            redraw(display, win, gc, currentTab, tab_count, active_tab);
                        }
                        else if (ctrl && (key == XK_d || key == XK_D))
                        {
                            if (currentTab->length == 0)
                            {
                                currentTab->should_exit = 1;
                            }
                            else if (currentTab->cursor_position < currentTab->length)
                            {
                                memmove(currentTab->command + currentTab->cursor_position,
                                        currentTab->command + currentTab->cursor_position + 1,
                                        (size_t)(currentTab->length - currentTab->cursor_position));
                                currentTab->length--;
                            }
                            redraw(display, win, gc, currentTab, tab_count, active_tab);
                        }
                        else if (key == XK_Return)
                        {
                            if (currentTab->length == 0)
                                continue;

                            if (command_is_incomplete(currentTab->command))
                            {
                                size_t len = strlen(currentTab->command);

                                if (len + 2 < sizeof(currentTab->command))
                                {
                                    /*
                                     * A trailing backslash escapes the newline.
                                     * Remove the backslash and join the next line
                                     * directly to the current command.
                                     */
                                    if (len > 0 && currentTab->command[len - 1] == '\\')
                                    {
                                        /* Keep the newline for display, but remove the backslash. */
                                        currentTab->command[len - 1] = '\n';
                                        currentTab->command[len] = '\0';
                                        currentTab->length = (int)len;
                                    }
                                    else
                                    {
                                        currentTab->command[len] = '\n';
                                        currentTab->command[len + 1] = '\0';
                                        currentTab->length = (int)(len + 1);
                                    }
                                }

                                currentTab->continuing_command = 1;

                                redraw(display, win, gc, currentTab, tab_count, active_tab);
                                continue;
                            }
                            currentTab->continuing_command = 0;
                            if (strcmp(currentTab->command, "exit") == 0 &&
                                !currentTab->running)
                            {
                                currentTab->should_exit = 1;
                                break;
                            }

                            {
                                char command_copy[INPUT_SIZE];

                                snprintf(command_copy, sizeof(command_copy), "%s",
                                         currentTab->command);
                                /* Join lines continued with a backslash. */
                                /* Remove the newline used for backslash continuation. */
                                for (size_t i = 0; command_copy[i] != '\0'; i++)
                                {
                                    if (command_copy[i] == '\n' &&
                                        currentTab->continuing_command)
                                    {
                                        memmove(&command_copy[i],
                                                &command_copy[i + 1],
                                                strlen(&command_copy[i + 1]) + 1);
                                    }
                                }

                                append_command_to_history(currentTab);
                                int submit_rc = use_process
                                                    ? submit_process_tab_command(currentTab, command_copy)
                                                    : submit_tab_command(currentTab, command_copy);
                                if (submit_rc == -1)
                                {
                                    const char *msg = "Tab worker is busy or unavailable; try again.\n";
                                    append_output(currentTab, msg, strlen(msg));
                                }
                            }

                            currentTab->command[0] = '\0';
                            currentTab->length = 0;
                            currentTab->cursor_position = 0;
                            currentTab->history_nav = -1;

                            redraw(display, win, gc, currentTab,
                                   tab_count, active_tab);
                        }

                        else if (key == XK_BackSpace)
                        {
                            if (currentTab->cursor_position > 0)
                            {
                                int pos = currentTab->cursor_position;
                                memmove(currentTab->command + pos - 1,
                                        currentTab->command + pos,
                                        (size_t)(currentTab->length - pos) + 1);
                                currentTab->length--;
                                currentTab->cursor_position--;
                            }
                            redraw(display, win, gc, currentTab,
                                   tab_count, active_tab);
                        }
                        else
                        {
                            char chars[32];
                            int n = XLookupString(&event.xkey, chars,
                                                  sizeof(chars), NULL, NULL);

                            KeySym keysym = XLookupKeysym(&event.xkey, 0);

                            if (ctrl && shift && (keysym == XK_c || keysym == XK_C))
                            {
                                /* Basic X11 cut-buffer copy of the current command line. */
                                XStoreBytes(display, currentTab->command, currentTab->length);
                                XFlush(display);
                                continue;
                            }

                            if (ctrl && shift && (keysym == XK_v || keysym == XK_V))
                            {
                                int paste_len = 0;
                                char *paste = XFetchBytes(display, &paste_len);
                                if (paste && paste_len > 0 &&
                                    currentTab->length + paste_len < INPUT_SIZE)
                                {
                                    memmove(currentTab->command + currentTab->cursor_position + paste_len,
                                            currentTab->command + currentTab->cursor_position,
                                            (size_t)(currentTab->length - currentTab->cursor_position) + 1);
                                    memcpy(currentTab->command + currentTab->cursor_position,
                                           paste, (size_t)paste_len);
                                    currentTab->length += paste_len;
                                    currentTab->cursor_position += paste_len;
                                    currentTab->history_nav = -1;
                                    XFree(paste);
                                    redraw(display, win, gc, currentTab, tab_count, active_tab);
                                }
                                else if (paste)
                                {
                                    XFree(paste);
                                }
                                continue;
                            }

                            if ((event.xkey.state & ControlMask) &&
                                !(event.xkey.state & ShiftMask) &&
                                (keysym == XK_c || keysym == XK_C))
                            {
                                if (currentTab->running &&
                                    currentTab->foreground_pgid > 0)
                                {
                                    if (kill(-currentTab->foreground_pgid, SIGINT) == -1)
                                    {
                                        perror("kill foreground process group");
                                    }
                                }
                                else
                                {
                                    /* No foreground command: handle copy or ignore Ctrl+C. */
                                }

                                /* X11 does not provide a terminal driver to redraw the prompt for us. */
                                redraw(display, win, gc, currentTab, tab_count, active_tab);
                                break;
                            }

                            if (ctrl && (key == XK_z || key == XK_Z))
                            {
                                if (currentTab->running &&
                                    currentTab->foreground_pgid > 0)
                                {
                                    if (kill(-currentTab->foreground_pgid, SIGTSTP) == -1)
                                    {
                                        perror("kill foreground process group");
                                    }
                                }

                                /* Show the prompt immediately; the worker will record the stopped job. */
                                redraw(display, win, gc, currentTab, tab_count, active_tab);
                                break;
                            }

                            if (n > 0 && currentTab->length + n < INPUT_SIZE)
                            {
                                int printable = 1;
                                for (int i = 0; i < n; i++)
                                {
                                    if (chars[i] < 32 || chars[i] > 126)
                                    {
                                        printable = 0;
                                        break;
                                    }
                                }
                                if (printable)
                                {
                                    if (currentTab->length + n < INPUT_SIZE)
                                    {
                                        memmove(currentTab->command + currentTab->cursor_position + n,
                                                currentTab->command + currentTab->cursor_position,
                                                (size_t)(currentTab->length - currentTab->cursor_position) + 1);
                                        memcpy(currentTab->command + currentTab->cursor_position,
                                               chars, (size_t)n);
                                        currentTab->length += n;
                                        currentTab->cursor_position += n;
                                        currentTab->command[currentTab->length] = '\0';
                                        currentTab->history_nav = -1;
                                    }
                                    redraw(display, win, gc, currentTab,
                                           tab_count, active_tab);
                                }
                            }
                        }
                    }
                }
            }
        }

        if (tabs[active_tab].should_exit && use_process)
        {
            int other_tab_busy = 0;
            for (int bi = 0; bi < tab_count; ++bi)
            {
                if (bi == active_tab)
                    continue;
                if (tabs[bi].running)
                    other_tab_busy = 1;
                for (int bj = 0; bj < MAX_JOBS; ++bj)
                    if (tabs[bi].jobs[bj].active)
                        other_tab_busy = 1;
            }
            if (other_tab_busy)
            {
                const char *msg = "Close other tabs only after their foreground/background jobs finish.\n";
                append_output(&tabs[active_tab], msg, strlen(msg));
                tabs[active_tab].should_exit = 0;
                redraw(display, win, gc, &tabs[active_tab], tab_count, active_tab);
                continue;
            }
        }

        if (tabs[active_tab].should_exit)
        {
            int closing_tab = active_tab;

            /* The process worker owns its output descriptors; do not close those
             * numeric fd values in the GUI parent. */
            if (!use_process && tabs[closing_tab].pipe_fd >= 0)
            {
                close(tabs[closing_tab].pipe_fd);
                tabs[closing_tab].pipe_fd = -1;
            }

            /* The tab array is compacted below. Stop workers first because each
             * worker holds a stable pointer to its tab slot. */
            if (!use_process)
            {
                for (int wi = 0; wi < tab_count; wi++)
                    stop_tab_worker(&tabs[wi]);
            }
            else
            {
                for (int wi = 0; wi < tab_count; wi++)
                    stop_process_tab_worker(&tabs[wi]);
            }

            /* Shift the tabs after the closed tab one position to the left. */
            if (closing_tab < tab_count - 1)
            {
                memmove(&tabs[closing_tab],
                        &tabs[closing_tab + 1],
                        (size_t)(tab_count - closing_tab - 1) *
                            sizeof(tabs[0]));
            }

            tab_count--;

            /* No tabs remain: now terminate the application. */
            if (tab_count == 0)
                break;

            {
                int workers_ok = 1;
                for (int wi = 0; wi < tab_count; wi++)
                {
                    int rc = use_process
                                 ? start_process_tab_worker(&tabs[wi], tabs, tab_count, history, xfd)
                                 : start_tab_worker(&tabs[wi], history);
                    if (rc == -1)
                    {
                        workers_ok = 0;
                        break;
                    }
                }
                if (!workers_ok)
                {
                    fprintf(stderr, "Failed to restart tab workers after closing a tab\n");
                    break;
                }
            }

            /* Select a valid tab after removing the old active tab. */
            if (active_tab >= tab_count)
                active_tab = tab_count - 1;

            redraw(display, win, gc, &tabs[active_tab],
                   tab_count, active_tab);

            /* Rebuild the poll descriptors for the updated tab array. */
            continue;
        }

        /* Handle output from every pipe that poll reported ready. */
        /* Handle foreground and background output pipes. */
        for (int i = 1; i < nfds; i++)
        {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;

            int tab_index = fd_to_tab[i];
            int job_index = fd_to_job[i];

            struct tab *t = &tabs[tab_index];

            if (use_process && job_index == -2)
            {
                unsigned char notifications[128];
                while (read(fds[i].fd, notifications, sizeof(notifications)) > 0)
                {
                }
                if (fds[i].revents & (POLLHUP | POLLERR))
                {
                    if (t->worker_notify_fd >= 0)
                        close(t->worker_notify_fd);
                    t->worker_notify_fd = -1;
                    if (t->worker_pid > 0)
                    {
                        (void)waitpid(t->worker_pid, NULL, WNOHANG);
                        t->worker_pid = -1;
                    }
                }
                if (tab_index == active_tab)
                    redraw(display, win, gc, t, tab_count, active_tab);
                continue;
            }

            int *pipe_fd;

            if (job_index == -1)
                pipe_fd = &t->pipe_fd;
            else
                pipe_fd = &t->jobs[job_index].pipe_fd;

            if (*pipe_fd < 0)
                continue;

            char buf[512];
            ssize_t n = read(*pipe_fd, buf, sizeof(buf));

            if (n > 0)
            {
                append_output(t, buf, (size_t)n);

                if (tab_index == active_tab)
                {
                    redraw(display, win, gc, t,
                           tab_count, active_tab);
                }
            }
            else if (n == 0)
            {
                close(*pipe_fd);
                *pipe_fd = -1;

                if (job_index == -1)
                {
                    t->output_eof = 1;
                    reap_children(t);
                }
            }
            else if (errno != EINTR && errno != EAGAIN)
            {
                perror("read");

                close(*pipe_fd);
                *pipe_fd = -1;

                if (job_index == -1)
                {
                    t->output_eof = 1;
                    reap_children(t);
                }
            }
        }

        for (int i = 0; !use_process && i < tab_count; i++)
        {
            if (tabs[i].running && reap_children(&tabs[i]))
            {
                if (i == active_tab)
                {
                    redraw(display, win, gc, &tabs[i],
                           tab_count, active_tab);
                }
            }

            int old_output_len = (int)tabs[i].output_len;

            reap_background_jobs(&tabs[i]);

            if (i == active_tab &&
                (int)tabs[i].output_len != old_output_len)
            {
                redraw(display, win, gc, &tabs[i],
                       tab_count, active_tab);
            }
        }

        if (active_tab < tab_count &&
            tabs[active_tab].output_len != output_len_before[active_tab])
        {
            redraw(display, win, gc, &tabs[active_tab], tab_count, active_tab);
        }
    }

    /* Stop tab workers before cleaning up their command state. */
    for (int i = 0; i < tab_count; i++)
    {
        if (use_process)
            stop_process_tab_worker(&tabs[i]);
        else
            stop_tab_worker(&tabs[i]);
    }

    /* Clean up commands still running in any tab. */
    for (int i = 0; i < tab_count; i++)
    {
        struct tab *t = &tabs[i];

        if (use_process)
        {
            t->running = 0;
            continue; /* Process worker has already cleaned its command children. */
        }
        if (t->pipe_fd >= 0)
        {
            close(t->pipe_fd);
            t->pipe_fd = -1;
        }

        for (int j = 0; j < t->child_count; j++)
        {
            pid_t pid = t->child_pids[j];

            if (pid <= 0 || t->child_exited[j])
                continue;

            /* Request graceful termination. */
            if (kill(pid, SIGTERM) == -1 && errno != ESRCH)
                perror("kill");

            /* Give the child up to 500 ms to exit. */
            int status;
            int reaped = 0;

            for (int attempt = 0; attempt < 50; attempt++)
            {
                pid_t result = waitpid(pid, &status, WNOHANG);

                if (result == pid || (result == -1 && errno == ECHILD))
                {
                    reaped = 1;
                    break;
                }

                if (result == -1 && errno != EINTR)
                {
                    perror("waitpid");
                    reaped = 1;
                    break;
                }

                struct timespec delay = {0, 10000000L}; /* 10 ms */
                nanosleep(&delay, NULL);
            }

            /* Force termination if the child is still running. */
            if (!reaped)
            {
                if (kill(pid, SIGKILL) == -1 && errno != ESRCH)
                    perror("kill");

                while (waitpid(pid, NULL, 0) == -1)
                {
                    if (errno == EINTR)
                        continue;

                    if (errno != ECHILD)
                        perror("waitpid");

                    break;
                }
            }

            t->child_exited[j] = 1;
        }

        t->running = 0;
        t->child_pid = -1;
        t->child_pid2 = -1;
        t->child_count = 0;
    }

    sem_destroy(&history->lock);
    if (use_process)
    {
        munmap(history, sizeof(*history));
        munmap(tabs, sizeof(struct tab) * MAX_TABS);
    }
    XFreeGC(display, gc);
    XDestroyWindow(display, win);
    XCloseDisplay(display);
    return 0;
}
