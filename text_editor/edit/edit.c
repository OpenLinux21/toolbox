/*  mini-vi.c — minimal vi-like editor, pure C, POSIX only  */
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAXLINES 4096
#define MAXCOL   4096

/* ── state ── */
static struct termios orig;
static char  *L[MAXLINES];   /* lines (heap, no trailing \n) */
static int    nL = 0;        /* number of lines               */
static int    cy = 0, cx = 0;/* cursor row / col              */
static int    top = 0;       /* first visible row             */
static int    rows, cols;    /* terminal size                 */
static int    dirty = 0;
static char   fname[256];
static char   statusmsg[256];

/* ── terminal ── */
static void raw_on(void) {
    tcgetattr(STDIN_FILENO, &orig);
    struct termios r = orig;
    r.c_iflag &= ~(BRKINT|ICRNL|INPCK|ISTRIP|IXON);
    r.c_oflag &= ~OPOST;
    r.c_cflag |=  CS8;
    r.c_lflag &= ~(ECHO|ICANON|IEXTEN|ISIG);
    r.c_cc[VMIN] = 1; r.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &r);
}
static void raw_off(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig); }

static void ws(void) {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    rows = w.ws_row - 1;   /* reserve bottom row for status */
    cols = w.ws_col;
}

static void wr(const char *s, int n) { write(STDOUT_FILENO, s, n); }
static void wrs(const char *s)       { wr(s, strlen(s)); }

/* ── line helpers ── */
static void line_ensure(int n) {
    while (nL <= n) { L[nL] = malloc(1); L[nL][0] = '\0'; nL++; }
}
static int linelen(int n) { return n < nL ? (int)strlen(L[n]) : 0; }

static void line_insert_char(int y, int x, char c) {
    int len = linelen(y);
    char *nl = malloc(len + 2);
    memcpy(nl, L[y], x);
    nl[x] = c;
    memcpy(nl + x + 1, L[y] + x, len - x + 1);
    free(L[y]); L[y] = nl; dirty = 1;
}
static void line_delete_char(int y, int x) {
    int len = linelen(y);
    if (x < 0 || x >= len) return;
    memmove(L[y] + x, L[y] + x + 1, len - x);
    dirty = 1;
}
static void line_split(int y, int x) {   /* Enter */
    int len = linelen(y);
    char *tail = malloc(len - x + 1);
    memcpy(tail, L[y] + x, len - x + 1);
    L[y][x] = '\0';
    /* shift lines down */
    if (nL < MAXLINES) {
        memmove(&L[y+2], &L[y+1], (nL - y - 1) * sizeof(char *));
        L[y+1] = tail; nL++;
    } else free(tail);
    dirty = 1;
}
static void line_join(int y) {           /* backspace at col 0 */
    if (y == 0) return;
    int la = linelen(y-1), lb = linelen(y);
    char *nl = malloc(la + lb + 1);
    memcpy(nl, L[y-1], la);
    memcpy(nl + la, L[y], lb + 1);
    free(L[y-1]); free(L[y]);
    L[y-1] = nl;
    memmove(&L[y], &L[y+1], (nL - y - 1) * sizeof(char *));
    nL--; dirty = 1;
}

/* ── file I/O ── */
static void load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { line_ensure(0); snprintf(statusmsg, 256, "[New file]"); return; }
    char buf[4096]; int n; char *cur = malloc(1); cur[0]='\0'; int clen=0;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                cur[clen] = '\0';
                if (nL < MAXLINES) { L[nL++] = cur; }
                else free(cur);
                cur = malloc(1); cur[0]='\0'; clen = 0;
            } else {
                cur = realloc(cur, clen + 2);
                cur[clen++] = buf[i];
            }
        }
    }
    cur[clen] = '\0';
    if (nL < MAXLINES) L[nL++] = cur; else free(cur);
    close(fd);
    if (nL == 0) line_ensure(0);
    snprintf(statusmsg, 256, "\"%s\" %d lines", path, nL);
}
static int save(const char *path) {
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) { snprintf(statusmsg,256,"E: cannot write %s", path); return 0; }
    for (int i = 0; i < nL; i++) {
        write(fd, L[i], linelen(i));
        write(fd, "\n", 1);
    }
    close(fd);
    dirty = 0;
    snprintf(statusmsg, 256, "\"%s\" written %d lines", path, nL);
    return 1;
}

/* ── rendering ── */
static char rbuf[1<<18];
static int  rpos;
static void rap(const char *s, int n) {
    if (rpos + n >= (int)sizeof rbuf) { wr(rbuf, rpos); rpos = 0; }
    memcpy(rbuf + rpos, s, n); rpos += n;
}
static void raps(const char *s) { rap(s, strlen(s)); }
static void rflush(void) { if (rpos) { wr(rbuf, rpos); rpos = 0; } }

static void draw(void) {
    ws();
    /* number column width */
    int nw = 1;
    for (int t = nL; t >= 10; t /= 10) nw++;
    if (nw < 2) nw = 2;

    raps("\x1b[?25l");   /* hide cursor */
    raps("\x1b[H");      /* home */

    char tmp[64];
    for (int r = 0; r < rows; r++) {
        int li = top + r;
        raps("\x1b[2K");  /* erase line */
        if (li < nL) {
            /* line number in dim colour */
            raps("\x1b[2m");
            snprintf(tmp, sizeof tmp, "%*d ", nw, li + 1);
            raps(tmp);
            raps("\x1b[0m");
            int content_cols = cols - nw - 1;
            int len = linelen(li);
            int show = len < content_cols ? len : content_cols;
            rap(L[li], show);
        } else {
            raps("\x1b[2m~\x1b[0m");
        }
        raps("\r\n");
    }

    /* status bar */
    raps("\x1b[7m");   /* reverse */
    int mlen = strlen(statusmsg);
    snprintf(tmp, sizeof tmp, " %-*.*s", cols-1, cols-1, statusmsg);
    raps(tmp);
    raps("\x1b[0m");

    /* reposition cursor */
    int nw1 = nw + 1;  /* nw digits + 1 space */
    int scr_row = cy - top + 1;
    int scr_col = cx + nw1 + 1;
    snprintf(tmp, sizeof tmp, "\x1b[%d;%dH", scr_row, scr_col);
    raps(tmp);
    raps("\x1b[?25h");  /* show cursor */
    rflush();
}

/* ── command mode ── */
static void cmd_mode(void) {
    char cbuf[64]; int clen = 0;
    /* draw ':' in status */
    snprintf(statusmsg, 256, ":");
    draw();

    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;
        if (c == 27 || c == '\r') break;
        if (c == 127 || c == 8) {
            if (clen > 0) { cbuf[--clen] = '\0'; }
            else break;
        } else if (clen < 62) {
            cbuf[clen++] = c; cbuf[clen] = '\0';
        }
        snprintf(statusmsg, 256, ":%s", cbuf);
        draw();
    }
    cbuf[clen] = '\0';

    if (!strcmp(cbuf, "q")) {
        if (dirty) { snprintf(statusmsg,256,"No write since last change (:q! to override)"); return; }
        raw_off(); wrs("\x1b[2J\x1b[H"); exit(0);
    }
    if (!strcmp(cbuf, "q!")) {
        raw_off(); wrs("\x1b[2J\x1b[H"); exit(0);
    }
    if (!strcmp(cbuf, "w")) {
        save(fname);
    }
    if (!strcmp(cbuf, "wq") || !strcmp(cbuf, "x")) {
        if (save(fname)) { raw_off(); wrs("\x1b[2J\x1b[H"); exit(0); }
    }
    if (cbuf[0] == 'w' && cbuf[1] == ' ') {
        strncpy(fname, cbuf+2, 254);
        save(fname);
    }
}

/* ── clamp helpers ── */
static void clamp_cx(void) {
    int len = linelen(cy);
    if (cx > len) cx = len;
    if (cx < 0)   cx = 0;
}
static void scroll(void) {
    if (cy < top) top = cy;
    if (cy >= top + rows) top = cy - rows + 1;
}

/* ── main loop ── */
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: vi <file>\n"); return 1; }
    strncpy(fname, argv[1], 254);

    raw_on();
    ws();
    load(fname);
    if (nL == 0) line_ensure(0);

    while (1) {
        scroll(); draw();
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) continue;

        if (c == ':') { cmd_mode(); continue; }

        if (c == 27) {   /* escape sequence */
            unsigned char seq[3] = {0};
            if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
            if (seq[0] == '[') {
                if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;
                switch (seq[1]) {
                    case 'A': if (cy > 0) cy--;         break;  /* Up    */
                    case 'B': if (cy < nL-1) cy++;      break;  /* Down  */
                    case 'C': cx++;                     break;  /* Right */
                    case 'D': if (cx > 0) cx--;         break;  /* Left  */
                    case 'H': cx = 0;                   break;  /* Home  */
                    case 'F': cx = linelen(cy);         break;  /* End   */
                    case '5': read(STDIN_FILENO,&seq[2],1); /* PgUp */
                              top = (top - rows > 0) ? top - rows : 0;
                              cy  = top; break;
                    case '6': read(STDIN_FILENO,&seq[2],1); /* PgDn */
                              top = (top + rows < nL) ? top + rows : nL-1;
                              cy  = top; break;
                    case '3': read(STDIN_FILENO,&seq[2],1); /* Delete */
                              line_delete_char(cy, cx); break;
                }
            }
            clamp_cx(); continue;
        }

        if (c == 13 || c == 10) {  /* Enter */
            line_split(cy, cx);
            cy++; cx = 0; continue;
        }
        if (c == 127 || c == 8) {  /* Backspace */
            if (cx > 0) { cx--; line_delete_char(cy, cx); }
            else if (cy > 0) {
                int prev_len = linelen(cy-1);
                line_join(cy); cy--; cx = prev_len;
            }
            continue;
        }
        if (c == 9) {  /* Tab → 4 spaces */
            for (int i = 0; i < 4; i++) { line_insert_char(cy, cx, ' '); cx++; }
            continue;
        }
        if (c >= 32 && c < 127) {
            line_insert_char(cy, cx, c); cx++;
        }
        clamp_cx();
    }
    raw_off();
    return 0;
}
