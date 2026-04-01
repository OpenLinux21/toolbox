/*
 * edit.c - A simple terminal text editor
 * Uses termios for raw terminal I/O (no third-party deps)
 * Build: gcc -o edit edit.c
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* ── constants ─────────────────────────────────────────────────────────── */
#define VERSION   "1.0.0"
#define TAB_STOP  4

/* ANSI escape helpers */
#define ESC_CLEAR    "\x1b[2J"
#define ESC_HOME     "\x1b[H"
#define ESC_HIDE_CUR "\x1b[?25l"
#define ESC_SHOW_CUR "\x1b[?25h"
#define ESC_ERASE_EOL "\x1b[K"
#define ESC_REVERSE  "\x1b[7m"
#define ESC_NORMAL   "\x1b[m"

/* ── data structures ────────────────────────────────────────────────────── */
typedef struct {
    char  *data;   /* raw content */
    int    len;    /* length (no NUL) */
    char  *render; /* display content (tabs expanded) */
    int    rlen;
} Row;

typedef struct {
    /* cursor */
    int cx, cy;   /* file coordinates */
    int rx;       /* render-x (after tab expansion) */
    /* viewport */
    int rowoff, coloff;
    /* terminal size */
    int screen_rows, screen_cols;
    /* file */
    Row   *rows;
    int    nrows;
    char  *filename;
    int    dirty;        /* unsaved changes */
    /* status message */
    char   status[128];
} Editor;

static Editor E;
static struct termios orig_termios;

/* ── terminal ───────────────────────────────────────────────────────────── */
static void die(const char *msg) {
    write(STDOUT_FILENO, ESC_CLEAR ESC_HOME, 7);
    perror(msg);
    exit(1);
}

static void term_disable_raw(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void term_enable_raw(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    atexit(term_disable_raw);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned)OPOST;
    raw.c_cflag |=  (unsigned)CS8;
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

static int term_get_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
        return -1;
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/* ── key reading ────────────────────────────────────────────────────────── */
enum Key {
    KEY_NULL = 0,
    CTRL_C   = 3,
    CTRL_Q   = 17,
    CTRL_S   = 19,
    KEY_ESC  = 27,
    KEY_BACKSPACE = 127,
    /* extended */
    KEY_UP = 1000, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_PAGE_UP, KEY_PAGE_DOWN,
    KEY_HOME, KEY_END, KEY_DEL
};

static int read_key(void) {
    char c;
    int  n;
    while ((n = (int)read(STDIN_FILENO, &c, 1)) != 1) {
        if (n == -1 && errno != EAGAIN) die("read");
    }
    if (c != KEY_ESC) return (unsigned char)c;

    /* escape sequence */
    char seq[4];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return KEY_ESC;
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return KEY_ESC;

    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) != 1) return KEY_ESC;
            if (seq[2] == '~') {
                switch (seq[1]) {
                    case '1': return KEY_HOME;
                    case '3': return KEY_DEL;
                    case '4': return KEY_END;
                    case '5': return KEY_PAGE_UP;
                    case '6': return KEY_PAGE_DOWN;
                    case '7': return KEY_HOME;
                    case '8': return KEY_END;
                }
            }
        } else {
            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
            }
        }
    } else if (seq[0] == 'O') {
        switch (seq[1]) {
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
        }
    }
    return KEY_ESC;
}

/* ── dynamic append buffer ──────────────────────────────────────────────── */
typedef struct { char *b; int len; } Abuf;
#define ABUF_INIT {NULL, 0}

static void ab_append(Abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, (size_t)(ab->len + len));
    if (!new) return;
    memcpy(new + ab->len, s, (size_t)len);
    ab->b   = new;
    ab->len += len;
}

static void ab_free(Abuf *ab) { free(ab->b); }

/* ── row operations ─────────────────────────────────────────────────────── */
static void row_update_render(Row *row) {
    free(row->render);
    /* count tabs */
    int tabs = 0;
    for (int i = 0; i < row->len; i++) if (row->data[i] == '\t') tabs++;
    row->render = malloc((size_t)(row->len + tabs * (TAB_STOP - 1) + 1));
    if (!row->render) die("malloc");
    int idx = 0;
    for (int i = 0; i < row->len; i++) {
        if (row->data[i] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->data[i];
        }
    }
    row->render[idx] = '\0';
    row->rlen = idx;
}

static int row_cx_to_rx(Row *row, int cx) {
    int rx = 0;
    for (int i = 0; i < cx && i < row->len; i++) {
        if (row->data[i] == '\t') rx += TAB_STOP - (rx % TAB_STOP);
        else rx++;
    }
    return rx;
}

static void row_insert_char(Row *row, int at, char c) {
    if (at < 0 || at > row->len) at = row->len;
    row->data = realloc(row->data, (size_t)(row->len + 2));
    if (!row->data) die("realloc");
    memmove(row->data + at + 1, row->data + at, (size_t)(row->len - at));
    row->data[at] = c;
    row->len++;
    row_update_render(row);
}

static void row_delete_char(Row *row, int at) {
    if (at < 0 || at >= row->len) return;
    memmove(row->data + at, row->data + at + 1, (size_t)(row->len - at));
    row->len--;
    row_update_render(row);
}

static void row_append_string(Row *row, const char *s, int len) {
    row->data = realloc(row->data, (size_t)(row->len + len + 1));
    if (!row->data) die("realloc");
    memcpy(row->data + row->len, s, (size_t)len);
    row->len += len;
    row->data[row->len] = '\0';
    row_update_render(row);
}

/* ── editor row management ──────────────────────────────────────────────── */
static void insert_row(int at, const char *s, int len) {
    if (at < 0 || at > E.nrows) at = E.nrows;
    E.rows = realloc(E.rows, (size_t)(E.nrows + 1) * sizeof(Row));
    if (!E.rows) die("realloc");
    memmove(&E.rows[at + 1], &E.rows[at], (size_t)(E.nrows - at) * sizeof(Row));
    E.rows[at].len    = len;
    E.rows[at].data   = malloc((size_t)len + 1);
    if (!E.rows[at].data) die("malloc");
    memcpy(E.rows[at].data, s, (size_t)len);
    E.rows[at].data[len] = '\0';
    E.rows[at].render = NULL;
    E.rows[at].rlen   = 0;
    row_update_render(&E.rows[at]);
    E.nrows++;
    E.dirty++;
}

static void delete_row(int at) {
    if (at < 0 || at >= E.nrows) return;
    free(E.rows[at].data);
    free(E.rows[at].render);
    memmove(&E.rows[at], &E.rows[at + 1], (size_t)(E.nrows - at - 1) * sizeof(Row));
    E.nrows--;
    E.dirty++;
}

/* ── editor operations ──────────────────────────────────────────────────── */
static void editor_insert_char(char c) {
    if (E.cy == E.nrows) insert_row(E.nrows, "", 0);
    row_insert_char(&E.rows[E.cy], E.cx, c);
    E.cx++;
    E.dirty++;
}

static void editor_insert_newline(void) {
    if (E.cx == 0) {
        insert_row(E.cy, "", 0);
    } else {
        Row *row = &E.rows[E.cy];
        insert_row(E.cy + 1, row->data + E.cx, row->len - E.cx);
        row = &E.rows[E.cy]; /* realloc may have moved ptr */
        row->len = E.cx;
        row->data[row->len] = '\0';
        row_update_render(row);
    }
    E.cy++;
    E.cx = 0;
    E.dirty++;
}

static void editor_backspace(void) {
    if (E.cy == E.nrows) return;
    if (E.cx == 0 && E.cy == 0) return;
    if (E.cx > 0) {
        row_delete_char(&E.rows[E.cy], E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.rows[E.cy - 1].len;
        row_append_string(&E.rows[E.cy - 1], E.rows[E.cy].data, E.rows[E.cy].len);
        delete_row(E.cy);
        E.cy--;
    }
    E.dirty++;
}

/* ── file I/O ───────────────────────────────────────────────────────────── */
static void file_open(const char *fname) {
    free(E.filename);
    E.filename = strdup(fname);

    FILE *fp = fopen(fname, "r");
    if (!fp) {
        if (errno == ENOENT) {
            /* new file */
            snprintf(E.status, sizeof(E.status), "New file: %s", fname);
            return;
        }
        die("fopen");
    }

    char  *line   = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 &&
               (line[linelen-1] == '\n' || line[linelen-1] == '\r'))
            linelen--;
        insert_row(E.nrows, line, (int)linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
    snprintf(E.status, sizeof(E.status), "Opened: %s (%d lines)", fname, E.nrows);
}

static void file_save(void) {
    if (!E.filename) {
        snprintf(E.status, sizeof(E.status), "No filename.");
        return;
    }
    /* compute total size */
    int total = 0;
    for (int i = 0; i < E.nrows; i++) total += E.rows[i].len + 1;
    char *buf = malloc((size_t)total);
    if (!buf) { snprintf(E.status, sizeof(E.status), "Save failed: out of memory"); return; }
    char *p = buf;
    for (int i = 0; i < E.nrows; i++) {
        memcpy(p, E.rows[i].data, (size_t)E.rows[i].len);
        p += E.rows[i].len;
        *p++ = '\n';
    }
    int fd = open(E.filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) { free(buf); snprintf(E.status, sizeof(E.status), "Save failed: %s", strerror(errno)); return; }
    ssize_t written = write(fd, buf, (size_t)total);
    close(fd);
    free(buf);
    if (written != total) {
        snprintf(E.status, sizeof(E.status), "Save failed: wrote %zd/%d bytes", written, total);
    } else {
        E.dirty = 0;
        snprintf(E.status, sizeof(E.status), "Saved %d bytes to %s", total, E.filename);
    }
}

/* ── scrolling ──────────────────────────────────────────────────────────── */
static void editor_scroll(void) {
    E.rx = (E.cy < E.nrows) ? row_cx_to_rx(&E.rows[E.cy], E.cx) : 0;

    /* vertical */
    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screen_rows) E.rowoff = E.cy - E.screen_rows + 1;
    /* horizontal */
    if (E.rx < E.coloff) E.coloff = E.rx;
    if (E.rx >= E.coloff + E.screen_cols) E.coloff = E.rx - E.screen_cols + 1;
}

/* ── cursor movement ────────────────────────────────────────────────────── */
static void editor_move_cursor(int key) {
    Row *row = (E.cy < E.nrows) ? &E.rows[E.cy] : NULL;

    switch (key) {
        case KEY_LEFT:
            if (E.cx > 0) E.cx--;
            else if (E.cy > 0) { E.cy--; E.cx = E.rows[E.cy].len; }
            break;
        case KEY_RIGHT:
            if (row && E.cx < row->len) E.cx++;
            else if (row && E.cx == row->len && E.cy < E.nrows - 1) { E.cy++; E.cx = 0; }
            break;
        case KEY_UP:
            if (E.cy > 0) E.cy--;
            break;
        case KEY_DOWN:
            if (E.cy < E.nrows) E.cy++;
            break;
    }
    /* clamp cx */
    row = (E.cy < E.nrows) ? &E.rows[E.cy] : NULL;
    int rowlen = row ? row->len : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

/* ── rendering ──────────────────────────────────────────────────────────── */
#define HEADER_ROWS 3   /* title bar + separator + status bar */
#define FOOTER_ROWS 1   /* hint bar */

static void draw_rows(Abuf *ab) {
    int edit_rows = E.screen_rows - HEADER_ROWS - FOOTER_ROWS;
    for (int y = 0; y < edit_rows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.nrows) {
            ab_append(ab, "~", 1);
        } else {
            Row *r = &E.rows[filerow];
            int len = r->rlen - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screen_cols) len = E.screen_cols;
            ab_append(ab, len > 0 ? r->render + E.coloff : "", len > 0 ? len : 0);
        }
        ab_append(ab, ESC_ERASE_EOL "\r\n", 6);
    }
}

static void refresh_screen(void) {
    editor_scroll();
    Abuf ab = ABUF_INIT;
    ab_append(&ab, ESC_HIDE_CUR ESC_HOME, 9);

    /* ── title bar ── */
    {
        char title[E.screen_cols + 1];
        int n = snprintf(title, sizeof(title),
            " File: %s%s",
            E.filename ? E.filename : "[No Name]",
            E.dirty    ? " [+]"      : "");
        if (n > E.screen_cols) n = E.screen_cols;
        ab_append(&ab, ESC_REVERSE, 4);
        ab_append(&ab, title, n);
        for (int i = n; i < E.screen_cols; i++) ab_append(&ab, " ", 1);
        ab_append(&ab, ESC_NORMAL "\r\n", 6);
    }

    /* ── status bar (line/col/total) ── */
    {
        char stat[E.screen_cols + 1];
        int n = snprintf(stat, sizeof(stat),
            " Line %d/%d  Column %d",
            E.nrows ? E.cy + 1 : 0,
            E.nrows,
            E.rx + 1);
        if (n > E.screen_cols) n = E.screen_cols;
        ab_append(&ab, ESC_REVERSE, 4);
        ab_append(&ab, stat, n);
        /* right-side: status message */
        int msglen = (int)strlen(E.status);
        int avail  = E.screen_cols - n;
        if (msglen > avail) msglen = avail;
        if (avail > 0) {
            for (int i = msglen; i < avail; i++) ab_append(&ab, " ", 1);
            ab_append(&ab, E.status, msglen);
        }
        ab_append(&ab, ESC_NORMAL "\r\n", 6);
    }

    /* separator */
    for (int i = 0; i < E.screen_cols; i++) ab_append(&ab, "-", 1);
    ab_append(&ab, "\r\n", 2);

    /* content rows */
    draw_rows(&ab);

    /* ── hint bar ── */
    {
        ab_append(&ab, ESC_REVERSE, 4);
        const char *hint = " [Ctrl+S] Save  [Ctrl+Q] Quit  [Ctrl+C] Cancel"
                           "  [PgUp/PgDn] Scroll";
        int len = (int)strlen(hint);
        if (len > E.screen_cols) len = E.screen_cols;
        ab_append(&ab, hint, len);
        for (int i = len; i < E.screen_cols; i++) ab_append(&ab, " ", 1);
        ab_append(&ab, ESC_NORMAL, 3);
    }

    /* reposition cursor */
    char buf[32];
    int cur_row = (E.cy - E.rowoff) + HEADER_ROWS + 1; /* 1-based, after header */
    int cur_col = (E.rx - E.coloff) + 1;
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cur_row, cur_col);
    ab_append(&ab, buf, (int)strlen(buf));
    ab_append(&ab, ESC_SHOW_CUR, 6);

    write(STDOUT_FILENO, ab.b, (size_t)ab.len);
    ab_free(&ab);
}

/* ── quit prompt ────────────────────────────────────────────────────────── */
/* Returns 1 if OK to quit */
static int confirm_quit(void) {
    if (!E.dirty) return 1;

    /* show prompt in status area */
    snprintf(E.status, sizeof(E.status),
             "Unsaved changes! Save? (y/n/ESC)");
    refresh_screen();

    while (1) {
        int k = read_key();
        if (k == 'y' || k == 'Y') { file_save(); return 1; }
        if (k == 'n' || k == 'N') return 1;
        if (k == KEY_ESC || k == CTRL_C) {
            snprintf(E.status, sizeof(E.status), "Quit cancelled.");
            return 0;
        }
    }
}

/* ── input processing ───────────────────────────────────────────────────── */
static void process_key(void) {
    int k = read_key();
    int edit_rows = E.screen_rows - HEADER_ROWS - FOOTER_ROWS;

    switch (k) {
        case CTRL_Q:
            if (confirm_quit()) {
                write(STDOUT_FILENO, ESC_CLEAR ESC_HOME, 7);
                exit(0);
            }
            break;
        case CTRL_C:
            if (confirm_quit()) {
                write(STDOUT_FILENO, ESC_CLEAR ESC_HOME, 7);
                exit(0);
            }
            break;
        case CTRL_S:
            file_save();
            break;
        case KEY_UP:
        case KEY_DOWN:
        case KEY_LEFT:
        case KEY_RIGHT:
            editor_move_cursor(k);
            break;
        case KEY_PAGE_UP:
            E.cy = E.rowoff;
            for (int i = 0; i < edit_rows; i++) editor_move_cursor(KEY_UP);
            snprintf(E.status, sizeof(E.status),
                E.cy == 0 ? "Already at top." : "");
            break;
        case KEY_PAGE_DOWN:
            E.cy = E.rowoff + edit_rows - 1;
            if (E.cy > E.nrows) E.cy = E.nrows;
            for (int i = 0; i < edit_rows; i++) editor_move_cursor(KEY_DOWN);
            snprintf(E.status, sizeof(E.status),
                E.cy >= E.nrows ? "Already at bottom." : "");
            break;
        case KEY_HOME:
            E.cx = 0;
            break;
        case KEY_END:
            if (E.cy < E.nrows) E.cx = E.rows[E.cy].len;
            break;
        case KEY_DEL:
            editor_move_cursor(KEY_RIGHT);
            editor_backspace();
            break;
        case KEY_BACKSPACE:
        case 8: /* Ctrl+H */
            editor_backspace();
            break;
        case '\r':
            editor_insert_newline();
            break;
        case KEY_ESC:
            /* ignore */
            break;
        default:
            if (!iscntrl(k) && k < 256) editor_insert_char((char)k);
            break;
    }
}

/* ── init ───────────────────────────────────────────────────────────────── */
static void editor_init(void) {
    E.cx = E.cy = E.rx = 0;
    E.rowoff = E.coloff = 0;
    E.rows    = NULL;
    E.nrows   = 0;
    E.filename = NULL;
    E.dirty    = 0;
    E.status[0] = '\0';

    if (term_get_size(&E.screen_rows, &E.screen_cols) == -1)
        die("ioctl/TIOCGWINSZ");
}

/* ── main ───────────────────────────────────────────────────────────────── */
static void print_help(void) {
    puts("Usage: edit [options] [filename]\n"
         "\nOptions:"
         "\n  --help, -h        Show this help message."
         "\n  --version         Show the version of the editor."
         "\n  filename          Open the specified file for editing."
         "\n\nKeyboard Shortcuts:"
         "\n  Ctrl+S            Save the current file."
         "\n  Ctrl+Q            Quit (prompts to save if unsaved changes)."
         "\n  Ctrl+C            Cancel / quit."
         "\n  Arrow Up/Down     Move cursor up or down."
         "\n  Arrow Left/Right  Move cursor left or right."
         "\n  Page Up/Page Down Scroll the page up or down."
         "\n  Home              Move to start of line."
         "\n  End               Move to end of line."
         "\n  Backspace/Delete  Delete character.");
}

int main(int argc, char *argv[]) {
    /* argument parsing */
    const char *filename = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_help(); return 0;
        }
        if (!strcmp(argv[i], "--version")) {
            printf("edit version %s\n", VERSION); return 0;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\nTry 'edit --help'\n", argv[i]);
            return 1;
        }
        if (filename) {
            fprintf(stderr, "Too many filenames. Try 'edit --help'\n"); return 1;
        }
        filename = argv[i];
    }

    term_enable_raw();
    editor_init();

    if (filename) {
        file_open(filename);
    } else {
        snprintf(E.status, sizeof(E.status), "New buffer (no file)");
    }

    /* main loop */
    while (1) {
        refresh_screen();
        process_key();
    }
    return 0;
}
