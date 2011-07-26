// -*- c-file-style: "bsd" -*-

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
                epanic("out of memory");
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

static int cpustatsFile, cpustatsLoadFile;
static char *cpustatsBuf;
static int maxCpus, cpustatsBufSize;

void
cpustats_init(void)
{
        if ((cpustatsFile = open("/proc/stat", O_RDONLY)) < 0)
                epanic("failed to open /proc/stat");
        if ((cpustatsLoadFile = open("/proc/loadavg", O_RDONLY)) < 0)
                epanic("failed to open /proc/loadavg");

        // Find the maximum number of CPU's we'll need
        char *poss = read_all("/sys/devices/system/cpu/possible");
        maxCpus = cpuset_max(poss) + 1;
        free(poss);

        // Allocate a big buffer to read /proc/stat in to
        cpustatsBufSize = maxCpus * 256;
        if (!(cpustatsBuf = malloc(cpustatsBufSize)))
                epanic("out of memory");
}

void
cpustats_loadavg(float load[3])
{
        if ((lseek(cpustatsLoadFile, 0, SEEK_SET)) < 0)
                epanic("failed to seek /proc/loadavg");
        if ((readn_str(cpustatsLoadFile, cpustatsBuf, cpustatsBufSize)) < 0)
                epanic("failed to read /proc/loadavg");
        if (sscanf(cpustatsBuf, "%f %f %f",
                   &load[0], &load[1], &load[2]) != 3)
                epanic("failed to parse /proc/loadavg");
}

struct cpustats*
cpustats_alloc(void)
{
        struct cpustats *res = malloc(sizeof *res);
        if (!res)
                epanic("out of memory");
        memset(res, 0, sizeof *res);
        res->cpus = malloc(maxCpus * sizeof *res->cpus);
        if (!res->cpus)
                epanic("out of memory");
        memset(res->cpus, 0, maxCpus * sizeof *res->cpus);
        return res;
}

void
cpustats_read(struct cpustats *out)
{
        int i;
        for (i = 0; i < maxCpus; i++)
                out->cpus[i].online = false;

        if ((lseek(cpustatsFile, 0, SEEK_SET)) < 0)
                epanic("failed to seek /proc/stat");
        if ((readn_str(cpustatsFile, cpustatsBuf, cpustatsBufSize)) < 0)
                epanic("failed to read /proc/stat");

        out->online = out->max = 0;
        out->real = time_usec() * sysconf(_SC_CLK_TCK) / 1000000;
        char *pos = cpustatsBuf;
        while (strncmp(pos, "cpu", 3) == 0) {
                pos += 3;

                struct cpustat *st;
                int cpu = -1;
                if (*pos == ' ') {
                        // Aggregate line
                        st = &out->avg;
                } else if (isdigit(*pos)) {
                        cpu = strtol(pos, &pos, 10);
                        if (cpu >= maxCpus)
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
        for (i = 0; i < maxCpus; i++) {
                cpustats_subtract1(&out->cpus[i], &a->cpus[i], &b->cpus[i]);
                if (out->cpus[i].online) {
                        out->online++;
                        out->max = i;
                }
        }
}

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

struct stat_info
{
        const char *name;
        int color;
        int offset;
} stat_info[] = {
#define FIELD(name, color) {#name, color, offsetof(struct cpustat, name)}
        FIELD(nice, 2), FIELD(user, 1), FIELD(sys, 4),
        FIELD(iowait, 3), FIELD(irq, 5), FIELD(softirq, 6),
        /* 
         * {"nice", 2, offsetof(struct cpustat, nice)},
         * {"user", 1}, {"sys", 4},
         * {"iowait", 3}, {"irq", 5}, {"softirq", 6},
         */
        {}
#undef FIELD
};
#define NSTATS 6

struct bar_info
{
        int start, len, cpu;
} *bar_info;
int bar_count;

// The layout of ui_display, etc is independent of final display
// layout, hence we avoid the terms "row", "column", "x", and "y".
// Rather, bar display is laid out as
//
//           len
//           012345678
//  barpos 0 |--bar--|
//         1
//         2 |--bar--|
int bar_length, bar_maxpos;
// XXX This won't work with variable-length UTF-8 characters
unsigned char *ui_display, *ui_fore, *ui_back;
#define UIXY(array, barpos, len) (array[barpos*bar_length + len])

void
ui_layout(struct cpustats *cpus)
{
        int i;

        putp(exit_attribute_mode);
        putp(clear_screen);

        // Draw key at the top
        struct stat_info *si;
        for (si = stat_info; si->name; si++) {
                putp(tiparm(set_background, si->color));
                printf("  ");
                putp(exit_attribute_mode);
                printf(" %s ", si->name);
        }

        // Create bar info
        free(bar_info);
        bar_count = cpus->online + 1;
        bar_info = malloc(bar_count * sizeof *bar_info);
        if (!bar_info)
                epanic("out of memory");

        // Create average bar
        bar_info[0].start = 0;
        bar_info[0].len = 3;
        bar_info[0].cpu = -1;

        // Lay out labels
        char buf[16];
        snprintf(buf, sizeof buf, "%d", cpus->max);
        int length = strlen(buf);
        int w = COLS - 3;

        if ((length + 1) * cpus->online < w) {
                // Lay out and draw labels horizontally
                bar_length = LINES - 2;
                putp(tiparm(cursor_address, LINES, 0));
                printf("avg");
                int bar = 1;
                for (i = 0; i <= cpus->max; ++i) {
                        if (cpus->cpus[i].online) {
                                printf(" %*d", length, i);
                                bar_info[bar].start = 4 + i*(length+1);
                                bar_info[bar].len = length;
                                bar_info[bar].cpu = i;
                                bar++;
                        }
                }
        } else {
                // Lay out the labels vertically
                int pad = 0, count = cpus->online;
                bar_length = LINES - 1 - length;
                if (cpus->online * 2 < w) {
                        // We have space for padding
                        pad = 1;
                } else if (cpus->online >= w) {
                        // We don't even have space for all of them
                        bar_count = w - 1;
                }

                int bar = 1;
                for (i = 0; i <= cpus->max; ++i) {
                        if (cpus->cpus[i].online) {
                                bar_info[bar].start = 4 + (bar-1)*(pad+1);
                                bar_info[bar].len = 1;
                                bar_info[bar].cpu = i;
                                bar++;
                        }
                }

                // Draw vertical labels
                putp(tiparm(cursor_address, LINES - length, 0));
                int row;
                for (row = 0; row < length; ++row) {
                        printf(row == 0 ? "avg" : "   ");
                        for (bar = 1; bar < bar_count; ++bar) {
                                i = snprintf(buf, sizeof buf, "%d", bar_info[bar].cpu);
                                if (pad || bar == 1)
                                        putchar(' ');
                                putchar(row < i ? buf[row] : ' ');
                        }
                        if (row != length - 1)
                                putchar('\n');
                }
        }

        // Allocate bar display buffers
        free(ui_display);
        free(ui_fore);
        free(ui_back);
        bar_maxpos = bar_info[bar_count-1].start + bar_info[bar_count-1].len;
        if (!(ui_display = malloc(bar_length * bar_maxpos)))
                epanic("out of memory");
        if (!(ui_fore = malloc(bar_length * bar_maxpos)))
                epanic("out of memory");
        if (!(ui_back = malloc(bar_length * bar_maxpos)))
                epanic("out of memory");
}

void
ui_show_load(float load[3])
{
        char buf[1024];
        snprintf(buf, sizeof buf, "%0.2f %0.2f %0.2f",
                 load[0], load[1], load[2]);
        putp(tiparm(cursor_address, 0, COLS - strlen(buf) - 8));
        putp(exit_attribute_mode);
        putp(tiparm(set_foreground, 7));
        printf("  load: ");
        putp(exit_attribute_mode);
        printf(buf);
}

void
ui_compute_bars(struct cpustats *delta)
{
        // XXX ui_display and ui_fore are fixed with ASCII-only bars
        memset(ui_display, ' ', bar_length * bar_maxpos);
        memset(ui_fore, 0xff, bar_length * bar_maxpos);
        memset(ui_back, 0xff, bar_length * bar_maxpos);

        int i, bar;
        for (bar = 0; bar < bar_count; bar++) {
                int barpos = bar_info[bar].start;
                struct cpustat *cpu = bar_info[bar].cpu == -1 ? &delta->avg :
                        &delta->cpus[bar_info[bar].cpu];

                // Calculate cut-offs
                enum { subcells = 100 };
                int scale = delta->real * (bar_info[bar].cpu == -1 ? delta->online : 1);
                int cutoff[NSTATS + 1];
                unsigned long long cumm = 0;
                for (i = 0; i < NSTATS; i++) {
                        cumm += *(unsigned long long*)
                                ((char*)cpu + stat_info[i].offset);
                        cutoff[i] = cumm * bar_length * subcells / scale;
                }
                cutoff[NSTATS] = bar_length * subcells;

                // Construct bar cells
                int len, stat;
                for (len = stat = 0; len < bar_length && stat < NSTATS; len++) {
                        int lo = len * subcells, hi = (len + 1) * subcells;
                        if (cutoff[stat] >= hi) {
                                // Cell is covered by this stat
                                UIXY(ui_back, barpos, len) =
                                        stat_info[stat].color;
                                continue;
                        }

                        // Find the biggest cover of this cell
                        // (possibly empty)
                        int biggestStat = stat, biggestVal = 0;
                        for (; stat < NSTATS + 1; stat++) {
                                int val = MIN(cutoff[stat] - lo, subcells);
                                if (val > biggestVal) {
                                        biggestVal = val;
                                        biggestStat = stat;
                                }
                                if (cutoff[stat] >= hi)
                                        break;
                        }
                        if (biggestStat < NSTATS)
                                UIXY(ui_back, barpos, len) =
                                        stat_info[biggestStat].color;
                }

                // Copy across bar length
                for (i = 1; i < bar_info[bar].len; ++i) {
                        memcpy(&UIXY(ui_display, barpos+i, 0),
                               &UIXY(ui_display, barpos, 0), bar_length);
                        memcpy(&UIXY(ui_fore, barpos+i, 0),
                               &UIXY(ui_fore, barpos, 0), bar_length);
                        memcpy(&UIXY(ui_back, barpos+i, 0),
                               &UIXY(ui_back, barpos, 0), bar_length);
                }
        }
}

void
ui_show_bars_dumb(void)
{
        int row, col;
        for (row = 0; row < bar_length; row++) {
                putp(tiparm(cursor_address, bar_length - row, 0));
                for (col = 0; col < bar_maxpos; col++) {
                        int back = UIXY(ui_back, col, row);
                        if (back == 0xff)
                                putp(exit_attribute_mode);
                        else
                                putp(tiparm(set_background, back));
                        putchar(UIXY(ui_display, col, row));
                }
        }
}

void
ui_show_bars(void)
{
        int row, col;
        int lastBack = -1;
        for (row = 0; row < bar_length; row++) {
                putp(tiparm(cursor_address, bar_length - row, 0));
                for (col = 0; col < bar_maxpos;) {
                        int back = UIXY(ui_back, col, row);

                        // Get a run of equivalent attributes
                        int end;
                        bool clearRun = (back == 0xff);
                        for (end = col; end < bar_maxpos; ++end) {
                                clearRun = clearRun &&
                                        UIXY(ui_display, end, row) == ' ';
                                if (UIXY(ui_back, end, row) != back)
                                        break;
                        }

                        // Draw run
                        if (lastBack != back) {
                                if (back == 0xff)
                                        putp(exit_attribute_mode);
                                else
                                        putp(tiparm(set_background, back));
                                lastBack = back;
                        }
                        if (clearRun && end == bar_maxpos) {
                                // Just clear the rest of the row
                                putp(clr_eol);
                                break;
                        }
                        fwrite(&UIXY(ui_display, col, row), 1, end-col, stdout);
                        col = end;
                }
        }
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
main(int argc, char **arv)
{
        struct sigaction sa = {
                .sa_handler = on_sigint
        };
        sigaction(SIGINT, &sa, NULL);

        cpustats_init();

        /* 
         * struct cpustats *st = cpustats_alloc();
         * int i;
         * while (!need_exit) {
         *         cpustats_read(st);
         *         printf("\n%llu\n", st->real);
         *         for (i = 0; i < maxCpus; i++) {
         *                 if (!st->cpus[i].online)
         *                         continue;
         *                 printf("%d %llu %llu %llu %llu %llu %llu\n",
         *                        i, st->cpus[i].user, st->cpus[i].nice,
         *                        st->cpus[i].sys, st->cpus[i].iowait,
         *                        st->cpus[i].irq, st->cpus[i].softirq);
         *         }
         *         sleep(1);
         * }
         */

        term_init();

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
                if (poll(&pollfd, 1, 1000) < 0 && errno != EINTR)
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
