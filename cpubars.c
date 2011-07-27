// -*- c-file-style: "bsd" -*-

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include <curses.h>
#include <term.h>

/******************************************************************
 * Utilities
 */

#define MIN(a, b) ({                            \
        __typeof__(a) __a = (a);                \
        __typeof__(b) __b = (b);                \
        (__a < __b) ? __a : __b;                \
        })

#define MAX(a, b) ({                            \
        __typeof__(a) __a = (a);                \
        __typeof__(b) __b = (b);                \
        (__a > __b) ? __a : __b;                \
        })

#define SWAP(a, b) ({                           \
        __typeof__(a) __a = (a);                \
        (a) = (b);                              \
        (b) = __a;                              \
        })

static void term_reset(void);

void
panic(const char *fmt, ...)
{
        va_list ap;

        term_reset();
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputs("\n", stderr);
        exit(-1);
}

void
epanic(const char *fmt, ...)
{
        va_list ap;

        term_reset();
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputs(": ", stderr);
        fputs(strerror(errno), stderr);
        fputs("\n", stderr);
        exit(-1);
}

ssize_t
readn_str(int fd, char *buf, size_t count)
{
        count -= 1;
        off_t pos = 0;
        while (1) {
                ssize_t r = read(fd, buf + pos, count - pos);
                if (r < 0)
                        return r;
                else if (r == 0)
                        break;
                pos += r;
        }
        buf[pos] = 0;
        return pos;
}

char *
read_all(const char *path)
{
        FILE *fp = fopen(path, "rb");
        if (!fp)
                epanic("failed to open %s", path);
        if (fseek(fp, 0, SEEK_END) < 0)
                epanic("failed to seek in %s", path);
        long len = ftell(fp);
        char *buf = malloc(len + 1);
        if (!buf)
                epanic("read_all");
        rewind(fp);
        size_t rlen = fread(buf, 1, len, fp);
        if ((errno = ferror(fp)))
                epanic("failed to read %s", path);
        buf[rlen] = 0;
        fclose(fp);
        return buf;
}

int
cpuset_max(const char *cpuset)
{
        // Since all we care about is the max, we can cut a lot of
        // corners
        int max = 0;
        const char *p = cpuset;
        while (*p) {
                if (isspace(*p) || *p == ',' || *p == '-')
                        ++p;
                else if (!isdigit(*p))
                        panic("invalid cpu set: %s", cpuset);
                else {
                        char *end;
                        int cpu = strtol(p, &end, 10);
                        p = end;
                        if (cpu > max)
                                max = cpu;
                }
        }
        return max;
}

uint64_t
time_usec(void)
{
        struct timeval tv;
        gettimeofday(&tv, 0);
        return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}


/******************************************************************
 * Stat parser
 */

struct cpustat
{
        bool online;
        unsigned long long user, nice, sys, iowait, irq, softirq;
};

struct cpustats
{
        int online, max;
        unsigned long long real;
        struct cpustat avg;
        struct cpustat *cpus;
};

// File descriptors for /proc/stat and /proc/loadavg
static int cpustats_fd, cpustats_load_fd;
// Maximum number of CPU's this system supports
static int cpustats_cpus;
// A buffer large enough to read cpustats_cpus worth of /proc/stat
static char *cpustats_buf;
static int cpustats_buf_size;

void
cpustats_init(void)
{
        if ((cpustats_fd = open("/proc/stat", O_RDONLY)) < 0)
                epanic("failed to open /proc/stat");
        if ((cpustats_load_fd = open("/proc/loadavg", O_RDONLY)) < 0)
                epanic("failed to open /proc/loadavg");

        // Find the maximum number of CPU's we'll need
        char *poss = read_all("/sys/devices/system/cpu/possible");
        cpustats_cpus = cpuset_max(poss) + 1;
        free(poss);

        // Allocate a big buffer to read /proc/stat in to
        cpustats_buf_size = cpustats_cpus * 128;
        if (!(cpustats_buf = malloc(cpustats_buf_size)))
                epanic("allocating cpustats file buffer");
}

void
cpustats_loadavg(float load[3])
{
        if ((lseek(cpustats_load_fd, 0, SEEK_SET)) < 0)
                epanic("failed to seek /proc/loadavg");
        if ((readn_str(cpustats_load_fd, cpustats_buf, cpustats_buf_size)) < 0)
                epanic("failed to read /proc/loadavg");
        if (sscanf(cpustats_buf, "%f %f %f",
                   &load[0], &load[1], &load[2]) != 3)
                epanic("failed to parse /proc/loadavg");
}

struct cpustats*
cpustats_alloc(void)
{
        struct cpustats *res = malloc(sizeof *res);
        if (!res)
                epanic("allocating cpustats");
        memset(res, 0, sizeof *res);
        res->cpus = malloc(cpustats_cpus * sizeof *res->cpus);
        if (!res->cpus)
                epanic("allocating per-CPU cputats");
        memset(res->cpus, 0, cpustats_cpus * sizeof *res->cpus);
        return res;
}

void
cpustats_read(struct cpustats *out)
{
        // On kernels prior to 2.6.37, this can take a long time on
        // large systems because updating IRQ counts is slow.  See
        //   https://lkml.org/lkml/2010/9/29/259

        int i;
        for (i = 0; i < cpustats_cpus; i++)
                out->cpus[i].online = false;

        if ((lseek(cpustats_fd, 0, SEEK_SET)) < 0)
                epanic("failed to seek /proc/stat");
        if ((readn_str(cpustats_fd, cpustats_buf, cpustats_buf_size)) < 0)
                epanic("failed to read /proc/stat");

        out->online = out->max = 0;
        out->real = time_usec() * sysconf(_SC_CLK_TCK) / 1000000;
        char *pos = cpustats_buf;
        while (strncmp(pos, "cpu", 3) == 0) {
                pos += 3;

                struct cpustat *st;
                int cpu = -1;
                if (*pos == ' ') {
                        // Aggregate line
                        st = &out->avg;
                } else if (isdigit(*pos)) {
                        cpu = strtol(pos, &pos, 10);
                        if (cpu >= cpustats_cpus)
                                goto next;
                        st = &out->cpus[cpu];
                } else {
                        goto next;
                }

                // Earlier versions of Linux only reported user, nice,
                // sys, and idle.
                st->iowait = st->irq = st->softirq = 0;
                unsigned long long toss;
                if (sscanf(pos, " %llu %llu %llu %llu %llu %llu %llu",
                           &st->user, &st->nice, &st->sys, &toss,
                           &st->iowait, &st->irq, &st->softirq) < 4)
                        continue;
                st->online = true;
                if (cpu != -1)
                        out->online++;
                if (cpu > out->max)
                        out->max = cpu;

        next:
                // Go to the next line
                while (*pos && *pos != '\n')
                        pos++;
                if (*pos) pos++;
        }
}

static bool
cpustats_subtract1(struct cpustat *out,
                   const struct cpustat *a, const struct cpustat *b)
{
        out->online = a->online && b->online;
        if (out->online) {
#define SUB(field) out->field = a->field - b->field
                SUB(user);
                SUB(nice);
                SUB(sys);
                SUB(iowait);
                SUB(irq);
                SUB(softirq);
#undef SUB
        }
}

void
cpustats_subtract(struct cpustats *out,
                  const struct cpustats *a, const struct cpustats *b)
{
        out->online = out->max = 0;
        out->real = a->real - b->real;
        cpustats_subtract1(&out->avg, &a->avg, &b->avg);

        int i;
        for (i = 0; i < cpustats_cpus; i++) {
                cpustats_subtract1(&out->cpus[i], &a->cpus[i], &b->cpus[i]);
                if (out->cpus[i].online) {
                        out->online++;
                        out->max = i;
                }
        }
}

// Test if `a' and `b' have the same set of online CPU's.
bool
cpustats_sets_equal(const struct cpustats *a, const struct cpustats *b)
{
        if (a->max != b->max || a->online != b->online)
                return false;

        int i;
        for (i = 0; i < a->max; i++)
                if (a->cpus[i].online != b->cpus[i].online)
                        return false;
        return true;
}

/******************************************************************
 * Terminal
 */

static sig_atomic_t term_need_resize;
static struct termios term_init_termios;
static bool term_initialized;

static void
term_on_sigwinch(int sig)
{
        term_need_resize = 1;
}

static void
term_reset(void)
{
        if (!term_initialized)
                return;
        // Leave invisible mode
        putp(cursor_normal);
        // Leave cursor mode
        putp(exit_ca_mode);
        fflush(stdout);
        // Reset terminal modes
        tcsetattr(0, TCSADRAIN, &term_init_termios);
        term_initialized = false;
}

void
term_init(void)
{
        setupterm(NULL, 1, NULL);
        if (tcgetattr(0, &term_init_termios) < 0)
                epanic("failed to get terminal attributes");

        // Handle terminal resize
        struct sigaction act = {
                .sa_handler = term_on_sigwinch
        };
        if (sigaction(SIGWINCH, &act, NULL) < 0)
                epanic("failed to install SIGWINCH handler");

        atexit(term_reset);
        term_initialized = true;

        // Enter cursor mode
        putp(enter_ca_mode);
        // Enter invisible mode
        putp(cursor_invisible);
        // Disable echo and enter canonical (aka cbreak) mode so we
        // get input without waiting for newline
        struct termios tc = term_init_termios;
        tc.c_lflag &= ~(ICANON | ECHO);
        tc.c_iflag &= ~ICRNL;
        tc.c_lflag |= ISIG;
        tc.c_cc[VMIN] = 1;
        tc.c_cc[VTIME] = 0;
        if (tcsetattr(0, TCSAFLUSH, &tc) < 0)
                epanic("failed to set terminal attributes");
}

// Handle any terminal resize that has happened since the last
// `term_init' or `term_check_resize'.  Return true if there was a
// resize.
bool
term_check_resize(void)
{
        if (!term_need_resize)
                return false;

        term_need_resize = 0;
        // restartterm is overkill, but appears to be the only way to
        // get ncurses to update the terminal size when using the
        // low-level routines.
        restartterm(NULL, 1, NULL);
        return true;
}

/******************************************************************
 * UI
 */

static const struct ui_stat
{
        const char *name;
        int color;
        int offset;
} ui_stats[] = {
#define FIELD(name, color) {#name, color, offsetof(struct cpustat, name)}
        FIELD(nice, COLOR_GREEN), FIELD(user, COLOR_BLUE),
        FIELD(sys, COLOR_RED), FIELD(iowait, COLOR_CYAN),
        FIELD(irq, COLOR_MAGENTA), FIELD(softirq, COLOR_YELLOW),
        // We set the color of the sentinel stat to 0xff so we can
        // safely refer to ui_stats[NSTATS].color as the last, "idle"
        // segment of a bar.
        {NULL, 0xff, 0}
#undef FIELD
};
#define NSTATS (sizeof(ui_stats)/sizeof(ui_stats[0]) - 1)

// If we have too many bars to fit on the screen, we divide the screen
// into "panes".  Wrapping the display into these panes is handled by
// the final output routine.
static struct ui_pane
{
        // start is the "length dimension" of the start of this pane
        // (for vertical bars, the row, relative to the bottom).
        // barpos is the first barpos that appears in this pane (for
        // vertical bars, the column).  width is the size of this pane
        // in the width dimension (for vertical bars, the number of
        // columns).
        int start, barpos, width;
} *ui_panes;
static int ui_num_panes;

static struct ui_bar
{
        int start, width, cpu;
} *ui_bars;
static int ui_num_bars;

// The layout of ui_display, etc is independent of final display
// layout, hence we avoid the terms "row", "column", "x", and "y".
// Rather, bar display is laid out as
//
//           len
//           012345678 <- ui_bar_length
//  barpos 0 |--bar--|
//         1
//         2 |--bar--|
//         ^- ui_bar_width
static int ui_bar_length, ui_bar_width;
// ui_display, ui_fore, and ui_back are 2-D arrays that should be
// indexed using UIXY.  ui_display stores indexes into ui_chars.
// ui_fore and ui_back store color codes or 0xff for default
// attributes.
static unsigned char *ui_display, *ui_fore, *ui_back;
#define UIXY(array, barpos, len) (array[(barpos)*ui_bar_length + (len)])

#define NCHARS 8
static char ui_chars[NCHARS][MB_LEN_MAX];
static bool ui_ascii;

void
ui_init(bool force_ascii)
{
        // Cell character 0 is always a space
        strcpy(ui_chars[0], " ");

#ifdef __STDC_ISO_10646__
        if (force_ascii) {
                ui_ascii = true;
                return;
        }

        // Encode Unicode cell characters using system locale
        char *origLocale = setlocale(LC_CTYPE, NULL);
        setlocale(LC_CTYPE, "");

        int ch;
        mbstate_t mbs;
        memset(&mbs, 0, sizeof mbs);
        for (ch = 1; ch < NCHARS; ch++) {
                int len = wcrtomb(ui_chars[ch], 0x2580 + ch, &mbs);
                if (len == -1 || !mbsinit(&mbs)) {
                        ui_ascii = true;
                        break;
                }
                ui_chars[ch][len] = 0;
        }

        // Restore the original locale
        setlocale(LC_CTYPE, origLocale);
#else
        ui_ascii = true;
#endif
}

static void
ui_init_panes(int n)
{
        free(ui_panes);
        ui_num_panes = n;
        if (!(ui_panes = malloc(n * sizeof *ui_panes)))
                epanic("allocating panes");
        
}

void
ui_layout(struct cpustats *cpus)
{
        int i;

        putp(exit_attribute_mode);
        putp(clear_screen);

        // Draw key at the top
        const struct ui_stat *si;
        for (si = ui_stats; si->name; si++) {
                putp(tiparm(set_a_background, si->color));
                printf("  ");
                putp(exit_attribute_mode);
                printf(" %s ", si->name);
        }

        // Create one pane by default
        ui_init_panes(1);
        ui_panes[0].barpos = 0;

        // Create bar info
        free(ui_bars);
        ui_num_bars = cpus->online + 1;
        ui_bars = malloc(ui_num_bars * sizeof *ui_bars);
        if (!ui_bars)
                epanic("allocating bars");

        // Create average bar
        ui_bars[0].start = 0;
        ui_bars[0].width = 3;
        ui_bars[0].cpu = -1;

        // Lay out labels
        char buf[16];
        snprintf(buf, sizeof buf, "%d", cpus->max);
        int length = strlen(buf);
        int label_len;
        int w = COLS - 4;

        if ((length + 1) * cpus->online < w) {
                // Lay out the labels horizontally
                ui_panes[0].start = 1;
                ui_bar_length = MAX(0, LINES - ui_panes[0].start - 2);
                label_len = 1;
                putp(tiparm(cursor_address, LINES, 0));
                int bar = 1;
                for (i = 0; i <= cpus->max; ++i) {
                        if (cpus->cpus[i].online) {
                                ui_bars[bar].start = 4 + (bar-1)*(length+1);
                                ui_bars[bar].width = length;
                                ui_bars[bar].cpu = i;
                                bar++;
                        }
                }
        } else {
                // Lay out the labels vertically
                int pad = 0, count = cpus->online;
                ui_panes[0].start = length;
                ui_bar_length = MAX(0, LINES - ui_panes[0].start - 2);
                label_len = length;

                if (cpus->online * 2 < w) {
                        // We have space for padding
                        pad = 1;
                } else if (cpus->online >= w && COLS >= 2) {
                        // We don't have space for all of them
                        int totalw = 4 + cpus->online;
                        ui_init_panes((totalw + COLS - 2) / (COLS - 1));
                        int plength = (LINES - 2) / ui_num_panes;
                        for (i = 0; i < ui_num_panes; ++i) {
                                ui_panes[i].start =
                                        (ui_num_panes-i-1) * plength + length;
                                ui_panes[i].barpos = i * (COLS - 1);
                                ui_panes[i].width = COLS - 1;
                        }
                        ui_bar_length = MAX(0, plength - length);
                }

                int bar = 1;
                for (i = 0; i <= cpus->max; ++i) {
                        if (cpus->cpus[i].online) {
                                ui_bars[bar].start = 4 + (bar-1)*(pad+1);
                                ui_bars[bar].width = 1;
                                ui_bars[bar].cpu = i;
                                bar++;
                        }
                }
        }

        // Allocate bar display buffers
        free(ui_display);
        free(ui_fore);
        free(ui_back);
        ui_bar_width = ui_bars[ui_num_bars-1].start + ui_bars[ui_num_bars-1].width;
        if (!(ui_display = malloc(ui_bar_length * ui_bar_width)))
                epanic("allocating display buffer");
        if (!(ui_fore = malloc(ui_bar_length * ui_bar_width)))
                epanic("allocating foreground buffer");
        if (!(ui_back = malloc(ui_bar_length * ui_bar_width)))
                epanic("allocating background buffer");

        if (ui_ascii) {
                // ui_display and ui_fore don't change in ASCII mode
                memset(ui_display, 0, ui_bar_length * ui_bar_width);
                memset(ui_fore, 0xff, ui_bar_length * ui_bar_width);
        }

        // Trim down the last pane to the right width
        ui_panes[ui_num_panes - 1].width =
                ui_bar_width - ui_panes[ui_num_panes - 1].barpos;

        // Draw labels
        char *label_buf = malloc(ui_bar_width * label_len);
        if (!label_buf)
                epanic("allocating label buffer");
        memset(label_buf, ' ', ui_bar_width * label_len);
        int bar;
        for (bar = 0; bar < ui_num_bars; ++bar) {
                char *out = &label_buf[ui_bars[bar].start];
                int len;
                if (bar == 0) {
                        strcpy(buf, "avg");
                        len = 3;
                } else
                        len = snprintf(buf, sizeof buf, "%d", ui_bars[bar].cpu);
                if (label_len == 1 || bar == 0)
                        memcpy(out, buf, len);
                else
                        for (i = 0; i < len; i++)
                                out[i * ui_bar_width] = buf[i];
        }
        for (i = 0; i < ui_num_panes; ++i) {
                putp(tiparm(cursor_address, LINES - ui_panes[i].start, 0));

                int row;
                for (row = 0; row < label_len; ++row) {
                        if (row > 0)
                                putchar('\n');
                        fwrite(&label_buf[row*ui_bar_width + ui_panes[i].barpos],
                               1, ui_panes[i].width, stdout);
                }
        }
        free(label_buf);

}

void
ui_show_load(float load[3])
{
        char buf[1024];
        snprintf(buf, sizeof buf, "%0.2f %0.2f %0.2f",
                 load[0], load[1], load[2]);
        putp(tiparm(cursor_address, 0, COLS - strlen(buf) - 8));
        putp(exit_attribute_mode);
        putp(tiparm(set_a_foreground, COLOR_WHITE));
        printf("  load: ");
        putp(exit_attribute_mode);
        printf(buf);
}

void
ui_compute_bars(struct cpustats *delta)
{
        if (!ui_ascii) {
                // ui_display and ui_fore are only used in Unicode mode
                memset(ui_display, 0, ui_bar_length * ui_bar_width);
                memset(ui_fore, 0xff, ui_bar_length * ui_bar_width);
        }
        memset(ui_back, 0xff, ui_bar_length * ui_bar_width);

        int i, bar;
        for (bar = 0; bar < ui_num_bars; bar++) {
                int barpos = ui_bars[bar].start;
                struct cpustat *cpu = ui_bars[bar].cpu == -1 ? &delta->avg :
                        &delta->cpus[ui_bars[bar].cpu];

                // Calculate cut-offs between segments.  We divide
                // each display cell into `subcells' steps so we can
                // use integer math.
                enum { subcells = 256 };
                // Values in delta are from 0 to `scale'.  For per-CPU
                // bars this is just the real time, but for the
                // average bar, it's multiplied by the number of
                // online CPU's.
                int scale = delta->real;
                if (ui_bars[bar].cpu == -1)
                        scale *= delta->online;
                // To simplify the code, we include one additional
                // cutoff fixed at the very top of the bar so we can
                // treat the empty region above the bar as a segment.
                int cutoff[NSTATS + 1];
                unsigned long long cumm = 0;
                for (i = 0; i < NSTATS; i++) {
                        cumm += *(unsigned long long*)
                                ((char*)cpu + ui_stats[i].offset);
                        cutoff[i] = cumm * ui_bar_length * subcells / scale;
                }
                cutoff[NSTATS] = ui_bar_length * subcells;

                // Construct bar cells
                int len, stat;
                for (len = stat = 0; len < ui_bar_length && stat < NSTATS; len++) {
                        int lo = len * subcells, hi = (len + 1) * subcells;
                        if (cutoff[stat] >= hi) {
                                // Cell is entirely covered
                                UIXY(ui_back, barpos, len) =
                                        ui_stats[stat].color;
                                continue;
                        }

                        // Find the two segments the cover this cell
                        // the most
                        int topStat[2] = {0, 0};
                        int topVal[2] = {-1, -1};
                        int val = lo;
                        for (; stat < NSTATS + 1; stat++) {
                                val = MIN(cutoff[stat], hi) - val;
                                if (val > topVal[0]) {
                                        topStat[1] = topStat[0];
                                        topVal[1] = topVal[0];
                                        topStat[0] = stat;
                                        topVal[0] = val;
                                } else if (val > topVal[1]) {
                                        topStat[1] = stat;
                                        topVal[1] = val;
                                }
                                if (cutoff[stat] >= hi)
                                        break;
                        }
                        if (topVal[0] == -1 || topVal[1] == -1)
                                panic("bug: topVal={%d,%d}",
                                      topVal[0], topVal[1]);

                        if (ui_ascii) {
                                // We only care about the biggest
                                // cover
                                UIXY(ui_back, barpos, len) =
                                        ui_stats[topStat[0]].color;
                                continue;
                        }

                        // Order the segments by stat so we put the
                        // earlier stat on the bottom
                        if (topStat[0] > topStat[1]) {
                                SWAP(topStat[0], topStat[1]);
                                SWAP(topVal[0], topVal[1]);
                        }

                        // Re-scale and choose a split
                        int cell = topVal[0] * NCHARS / (topVal[0] + topVal[1]);

                        // Fill the cell
                        if (cell == NCHARS - 1) {
                                // We leave this as a space, which
                                // means the color roles are reversed
                                UIXY(ui_back, barpos, len) =
                                        ui_stats[topStat[0]].color;
                        } else {
                                UIXY(ui_display, barpos, len) = cell;
                                UIXY(ui_fore, barpos, len) =
                                        ui_stats[topStat[0]].color;
                                UIXY(ui_back, barpos, len) =
                                        ui_stats[topStat[1]].color;
                        }
                }

                // Copy across bar length
                for (i = 1; i < ui_bars[bar].width; ++i) {
                        memcpy(&UIXY(ui_display, barpos+i, 0),
                               &UIXY(ui_display, barpos, 0), ui_bar_length);
                        memcpy(&UIXY(ui_fore, barpos+i, 0),
                               &UIXY(ui_fore, barpos, 0), ui_bar_length);
                        memcpy(&UIXY(ui_back, barpos+i, 0),
                               &UIXY(ui_back, barpos, 0), ui_bar_length);
                }
        }
}

static void
ui_show_pane(struct ui_pane *pane)
{
        int row, col;
        int lastBack = -1, lastFore = -1;
        for (row = 0; row < ui_bar_length; row++) {
                putp(tiparm(cursor_address, LINES - pane->start - row - 1, 0));

                // What's the width of this row?  Beyond this, we can
                // just clear the line.
                int endCol = 0;
                for (col = pane->barpos; col < pane->barpos + pane->width;
                     col++) {
                        if (UIXY(ui_back, col, row) != 0xff ||
                            UIXY(ui_display, col, row) != 0)
                                endCol = col + 1;
                }

                for (col = pane->barpos; col < endCol; col++) {
                        int cell = UIXY(ui_display, col, row);
                        int back = UIXY(ui_back, col, row);
                        int fore = UIXY(ui_fore, col, row);

                        // If it's a space, we don't care what the
                        // foreground color is.
                        if (ui_chars[cell][0] == ' ' && lastFore != -1)
                                fore = lastFore;

                        // Set attributes
                        if (lastBack != back || lastFore != fore) {
                                if (back == 0xff || fore == 0xff) {
                                        putp(exit_attribute_mode);
                                        lastBack = lastFore = 0xff;
                                }
                                if (lastBack != back) {
                                        putp(tiparm(set_a_background, back));
                                        lastBack = back;
                                }
                                if (lastFore != fore) {
                                        putp(tiparm(set_a_foreground, fore));
                                        lastFore = fore;
                                }
                        }

                        fputs(ui_chars[cell], stdout);
                }

                // Clear to the end of the line
                if (endCol < pane->barpos + pane->width) {
                        if (lastBack != 0xff || lastFore != 0xff) {
                                putp(exit_attribute_mode);
                                lastBack = lastFore = 0xff;
                        }
                        putp(clr_eol);
                }
        }
}

void
ui_show_bars(void)
{
        int pane;
        for (pane = 0; pane < ui_num_panes; ++pane)
                ui_show_pane(&ui_panes[pane]);
}

/******************************************************************
 * Main
 */

static sig_atomic_t need_exit;

void
on_sigint(int sig)
{
        need_exit = 1;
}

int
main(int argc, char **argv)
{
        bool force_ascii = false;
        int delay = 500;

        int opt;
        while ((opt = getopt(argc, argv, "ad:h")) != -1) {
                switch (opt) {
                case 'a':
                        force_ascii = true;
                        break;
                case 'd':
                {
                        char *end;
                        float val = strtof(optarg, &end);
                        if (*end) {
                                fprintf(stderr, "Delay argument (-d) requires "
                                        "a number\n");
                                exit(2);
                        }
                        delay = 1000 * val;
                        break;
                }
                default:
                        fprintf(stderr, "Usage: %s [-a] [-d delay]\n", argv[0]);
                        if (opt == 'h') {
                                fprintf(stderr,
                                        "\n"
                                        "Display CPU usage as a bar chart.\n"
                                        "\n"
                                        "Options:\n"
                                        "  -a       Use ASCII-only bars (instead of Unicode)\n"
                                        "  -d SECS  Specify delay between updates (decimals accepted)\n"
                                        "\n"
                                        "If your bars look funky, use -a or specify LANG=C.\n"
                                        "\n"
                                        "For kernels prior to 2.6.37, using a small delay on a large system can\n"
                                        "induce significant system time overhead.\n");
                                exit(0);
                        }
                        exit(2);
                }
        }
        if (optind < argc) {
                fprintf(stderr, "Unexpected arguments\n");
                exit(2);
        }

        struct sigaction sa = {
                .sa_handler = on_sigint
        };
        sigaction(SIGINT, &sa, NULL);

        cpustats_init();
        term_init();
        ui_init(force_ascii);

        struct cpustats *before = cpustats_alloc(),
                *after = cpustats_alloc(),
                *delta = cpustats_alloc(),
                *prevLayout = cpustats_alloc();

        cpustats_read(before);
        cpustats_subtract(prevLayout, before, before);
        ui_layout(prevLayout);
        fflush(stdout);
        while (!need_exit) {
                // Sleep or take input
                struct pollfd pollfd = {
                        .fd = 0,
                        .events = POLLIN
                };
                if (poll(&pollfd, 1, delay) < 0 && errno != EINTR)
                        epanic("poll failed");
                if (pollfd.revents & POLLIN) {
                        char ch = 0;
                        if (read(0, &ch, 1) < 0)
                                epanic("read failed");
                        if (ch == 'q')
                                break;
                }

                // Get new statistics
                cpustats_read(after);
                cpustats_subtract(delta, after, before);

                // Recompute the layout if necessary
                if (term_check_resize() || !cpustats_sets_equal(delta, prevLayout))
                        ui_layout(delta);

                // Show the load average
                float loadavg[3];
                cpustats_loadavg(loadavg);
                ui_show_load(loadavg);

                if (delta->real) {
                        ui_compute_bars(delta);
                        ui_show_bars();
                }

                // Done updating UI
                fflush(stdout);

                SWAP(before, after);
                SWAP(delta, prevLayout);
        }

        return 0;
}
