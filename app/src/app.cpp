// Porpoise Retro Rewind - application state, worker jobs and controller UI.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "elevate.h"
#include "gfx.h"
#include "http_sce.h"
#include "pad.h"

#include "rr_patch.h"
#include "rr_update.h"
#include <cerrno>
#include <cstdarg>
#include <fcntl.h>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

extern "C" int sceKernelUsleep(unsigned);

namespace
{
constexpr const char *kRoot = "/data/porpoise/rewind";
constexpr const char *kBase = "/data/porpoise/rewind/base";
constexpr const char *kPack = "/data/porpoise/rewind/pack";
constexpr const char *kBuilder = "/data/porpoise/rewind/builder";
constexpr const char *kOut = "/data/porpoise/games";
constexpr const char *kState = "/data/porpoise/rewind/state";
constexpr const char *kLogDir = "/data/porpoise/rewind/logs";
constexpr const char *kLogFile = "/data/porpoise/rewind/logs/porpoise.log";
constexpr const char *kRegionFile = "/data/porpoise/rewind/state/region.txt";

enum Job { J_NONE, J_REFRESH, J_CHECK, J_INSTALL, J_PATCH, J_VERIFY };

constexpr int kLogLines = 400, kLogLen = 220;
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
char g_log[kLogLines][kLogLen];
int g_log_n = 0;                       // total lines ever written
volatile int g_cancel = 0;
volatile int g_busy = J_NONE;
volatile int g_dirty = 1;

// Shared (g_mu) view of the world.
struct View
{
    char installed[32] = "-", latest[32] = "-", update_note[160] = "not checked yet";
    char base_line[400] = "scanning...", out_line[200] = "-";
    char phase[64] = "", result[300] = "";
    bool result_is_error = false;
    std::uint64_t done = 0, total = 0;
    int pending = -1;
    char region = 0; // 'E','P','J' or 0 = not chosen
    bool base_ok = false;
    bool access_ok = false;
    char access_line[240] = "checking /data access...";
} g_v;

rr_update_state g_us;
bool g_us_valid = false;
rr_http g_http = {sce_http_get, nullptr};

void file_log(const char *line)
{
    static unsigned long written = 0;
    if (written > (1u << 20))
    {
        char old[256];
        std::snprintf(old, sizeof old, "%s.1", kLogFile);
        (void)rename(kLogFile, old);
        written = 0;
    }
    FILE *f = std::fopen(kLogFile, "a");
    if (!f) return;
    written += std::fprintf(f, "%s\n", line);
    std::fclose(f);
}

void add_log(int level, const char *msg)
{
    static const char *tag[] = {"D", "I", "W", "E"};
    char line[kLogLen + 8];
    std::snprintf(line, sizeof line, "[%s] %s", tag[level & 3], msg);
    pthread_mutex_lock(&g_mu);
    std::snprintf(g_log[g_log_n % kLogLines], kLogLen, "%s", line);
    ++g_log_n;
    pthread_mutex_unlock(&g_mu);
    file_log(line);
    g_dirty = 1;
}

void log_cb(void *, int level, const char *msg) { add_log(level, msg); }

void progress_cb(void *, const char *phase, std::uint64_t done, std::uint64_t total)
{
    pthread_mutex_lock(&g_mu);
    std::snprintf(g_v.phase, sizeof g_v.phase, "%s", phase);
    g_v.done = done;
    g_v.total = total;
    pthread_mutex_unlock(&g_mu);
    g_dirty = 1;
}

void set_result(bool err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void set_result(bool err, const char *fmt, ...)
{
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_mu);
    std::snprintf(g_v.result, sizeof g_v.result, "%s", buf);
    g_v.result_is_error = err;
    pthread_mutex_unlock(&g_mu);
    add_log(err ? RR_LOG_ERROR : RR_LOG_INFO, buf);
}

void make_ctx(rr_ctx &c)
{
    rr_ctx_init(&c);
    c.log = log_cb;
    c.progress = progress_cb;
    c.cancel = &g_cancel;
}

rr_patch_cfg make_cfg(char region)
{
    rr_patch_cfg cfg = {kBase, kPack, kBuilder, kOut, region};
    return cfg;
}

char load_region()
{
    char *t = nullptr;
    size_t n = 0;
    char r = 0;
    if (rr_read_text_file(kRegionFile, &t, &n, 16) == 0 && t)
    {
        if (t[0] == 'E' || t[0] == 'P' || t[0] == 'J') r = t[0];
        std::free(t);
    }
    return r;
}

void save_region(char r)
{
    const char s[2] = {r, 0};
    (void)rr_mkdir_p(kState);
    (void)rr_write_text_file_atomic(kRegionFile, s, 1);
}

const char *region_name(char r) { return r == 'E' ? "USA (RMCETO)" : r == 'P' ? "PAL (RMCPTO)" : r == 'J' ? "Japan (RMCJTO)" : "NOT CHOSEN"; }

void human_size(std::uint64_t b, char *out, size_t cap)
{
    if (b >= (1ull << 30)) std::snprintf(out, cap, "%.2f GiB", b / double(1ull << 30));
    else std::snprintf(out, cap, "%.1f MiB", b / double(1ull << 20));
}

// ---- jobs (run on the worker thread) ----
void do_refresh(rr_ctx &c)
{
    char inst[32] = "";
    rr_ctx q;
    make_ctx(q);
    q.log = nullptr;
    const int vr = rr_read_installed_version(&q, kPack, inst);
    rr_patch_status st;
    std::memset(&st, 0, sizeof st);
    rr_patch_cfg cfg = make_cfg(g_v.region ? g_v.region : 'E');
    const int sr = rr_patch_status_get(&q, &cfg, &st);
    (void)c;
    pthread_mutex_lock(&g_mu);
    std::snprintf(g_v.installed, sizeof g_v.installed, "%s", vr == 0 ? inst : "not installed");
    g_v.base_ok = false;
    if (sr == 0 && st.base_found && st.base_region)
    {
        g_v.base_ok = true;
        std::snprintf(g_v.base_line, sizeof g_v.base_line, "found, %s release (%s)", st.base_region == 'E' ? "USA" : st.base_region == 'P' ? "PAL" : "Japan", st.base_id);
    }
    else if (st.base_error[0]) std::snprintf(g_v.base_line, sizeof g_v.base_line, "%s", st.base_error);
    else std::snprintf(g_v.base_line, sizeof g_v.base_line, "%s", q.err[0] ? q.err : "no game image found in base folder");
    if (sr == 0 && st.out_exists)
    {
        char sz[32];
        human_size(st.out_size, sz, sizeof sz);
        std::snprintf(g_v.out_line, sizeof g_v.out_line, "%s present, %s, %s", st.out_path + (std::strrchr(st.out_path, '/') ? std::strrchr(st.out_path, '/') - st.out_path + 1 : 0), sz,
                      st.out_current ? "up to date with current inputs" : "built from different inputs");
    }
    else std::snprintf(g_v.out_line, sizeof g_v.out_line, "%s", "no output yet");
    pthread_mutex_unlock(&g_mu);
}

int do_check(rr_ctx &c)
{
    if (g_us_valid) { rr_update_state_free(&g_us); g_us_valid = false; }
    int r = sce_http_start();
    if (r < 0) return rr_fail(&c, RR_E_NET, "network stack failed to start (0x%08x)", (unsigned)r);
    std::memset(&g_us, 0, sizeof g_us);
    r = rr_update_check_ex(&c, &g_http, kPack, &g_us, 1);
    if (r) return r;
    g_us_valid = true;
    pthread_mutex_lock(&g_mu);
    std::snprintf(g_v.installed, sizeof g_v.installed, "%s", g_us.installed);
    std::snprintf(g_v.latest, sizeof g_v.latest, "%s", g_us.latest);
    g_v.pending = g_us.up_to_date ? 0 : (int)g_us.n_versions;
    if (g_us.up_to_date) std::snprintf(g_v.update_note, sizeof g_v.update_note, "up to date");
    else std::snprintf(g_v.update_note, sizeof g_v.update_note, "update to %s available (%d zip file(s), latest only)", g_us.latest, (int)g_us.n_pending);
    pthread_mutex_unlock(&g_mu);
    return 0;
}

int do_install(rr_ctx &c)
{
    int r = 0;
    if (!g_us_valid || g_us.up_to_date)
    {
        r = do_check(c);
        if (r) return r;
    }
    if (g_us.up_to_date) { set_result(false, "Retro Rewind %s is already the latest version - nothing downloaded.", g_us.installed); return 0; }
    r = rr_update_apply(&c, &g_http, kPack, &g_us);
    char v[32] = "";
    rr_ctx q;
    make_ctx(q);
    (void)rr_read_installed_version(&q, kPack, v);
    pthread_mutex_lock(&g_mu);
    std::snprintf(g_v.installed, sizeof g_v.installed, "%s", v[0] ? v : "?");
    pthread_mutex_unlock(&g_mu);
    if (r == 0)
    {
        pthread_mutex_lock(&g_mu);
        g_v.pending = 0;
        std::snprintf(g_v.update_note, sizeof g_v.update_note, "up to date");
        pthread_mutex_unlock(&g_mu);
        rr_update_state_free(&g_us);
        g_us_valid = false;
    }
    return r;
}

void *worker(void *arg)
{
    const int job = (int)(long)arg;
    rr_ctx c;
    make_ctx(c);
    int r = 0;
    const char region = g_v.region;
    switch (job)
    {
    case J_REFRESH: do_refresh(c); break;
    case J_CHECK:
        r = do_check(c);
        if (!r) set_result(false, "Installed %s, latest %s: %s.", g_v.installed, g_v.latest, g_v.update_note);
        break;
    case J_INSTALL:
        r = do_install(c);
        if (!r && !g_v.result_is_error && g_v.pending == 0 && std::strncmp(g_v.result, "Retro Rewind", 12) != 0)
            set_result(false, "Update complete. Installed version is now %s.", g_v.installed);
        break;
    case J_PATCH:
    {
        rr_patch_cfg cfg = make_cfg(region);
        rr_patch_result res;
        std::memset(&res, 0, sizeof res);
        r = rr_patch_run(&c, &cfg, 0, &res);
        if (!r)
        {
            char sz[32];
            human_size(res.out_size, sz, sizeof sz);
            if (res.skipped_up_to_date) set_result(false, "Output already built from the current inputs and verified - nothing to do. %s (%s)", res.out_path, sz);
            else set_result(false, "Patched and verified: %s (%s, %llu files).", res.out_path, sz, (unsigned long long)res.files);
        }
        break;
    }
    case J_VERIFY:
    {
        rr_patch_cfg cfg = make_cfg(region);
        r = rr_patch_verify_output(&c, &cfg);
        if (!r) set_result(false, "Output verified: disc structure, signatures and all hash tables are valid.");
        break;
    }
    }
    if (r)
    {
        if (r == RR_E_CANCEL) set_result(true, "Cancelled. Nothing was changed: the previous valid output / pack is untouched.");
        else set_result(true, "FAILED: %s", c.err[0] ? c.err : rr_strerror(r));
    }
    if (job != J_REFRESH) do_refresh(c);
    pthread_mutex_lock(&g_mu);
    g_v.phase[0] = 0;
    g_v.done = g_v.total = 0;
    pthread_mutex_unlock(&g_mu);
    g_busy = J_NONE;
    g_dirty = 1;
    return nullptr;
}

void start_job(Job j)
{
    if (g_busy != J_NONE) return;
    g_cancel = 0;
    g_busy = j;
    pthread_mutex_lock(&g_mu);
    if (j != J_REFRESH) { g_v.result[0] = 0; g_v.result_is_error = false; }
    pthread_mutex_unlock(&g_mu);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    if (pthread_create(&t, &attr, worker, (void *)(long)j) != 0)
    {
        g_busy = J_NONE;
        set_result(true, "could not start worker thread");
    }
    pthread_attr_destroy(&attr);
    g_dirty = 1;
}

// ---- UI ----
using gfx::rgb;
const std::uint32_t C_BG = rgb(14, 17, 26), C_PANEL = rgb(26, 31, 46), C_TXT = rgb(228, 232, 240), C_DIM = rgb(140, 150, 170),
                    C_ACC = rgb(80, 200, 255), C_SEL = rgb(40, 70, 110), C_OK = rgb(110, 220, 140), C_ERR = rgb(255, 110, 110),
                    C_WARN = rgb(255, 200, 90);

enum Item { I_CHECK, I_INSTALL, I_REGION, I_PATCH, I_VERIFY, I_LOG, I_COUNT };
int g_sel = I_CHECK;
bool g_log_view = false;
int g_log_scroll = 0; // lines from newest

const char *item_label(int i)
{
    switch (i)
    {
    case I_CHECK: return "Check for Retro Rewind updates";
    case I_INSTALL: return "Install updates";
    case I_REGION: return "Region";
    case I_PATCH: return "Patch game";
    case I_VERIFY: return "Verify output";
    default: return "View diagnostic log";
    }
}

void wrap_text(int x, int y, const char *s, int scale, std::uint32_t col, int max_chars, int max_lines)
{
    const int lh = 20 * scale + 4;
    char line[256];
    int lines = 0;
    size_t n = std::strlen(s), i = 0;
    while (i < n && lines < max_lines)
    {
        size_t take = n - i < (size_t)max_chars ? n - i : (size_t)max_chars;
        if (i + take < n)
        {
            size_t k = take;
            while (k > 0 && s[i + k] != ' ') --k;
            if (k > 0) take = k;
        }
        if (take >= sizeof line) take = sizeof line - 1;
        std::memcpy(line, s + i, take);
        line[take] = 0;
        gfx::text(x, y + lines * lh, line, scale, col);
        i += take;
        while (i < n && s[i] == ' ') ++i;
        ++lines;
    }
}

void draw_log_view()
{
    gfx::fill(0, 0, gfx::W, gfx::H, C_BG);
    gfx::text(60, 30, "Diagnostic log", 2, C_ACC);
    gfx::text(60, 80, "Up/Down scroll   Circle back", 1, C_DIM);
    gfx::text(60, 100, kLogFile, 1, C_DIM);
    pthread_mutex_lock(&g_mu);
    const int total = g_log_n < kLogLines ? g_log_n : kLogLines;
    const int rows = 40;
    if (g_log_scroll > total - rows) g_log_scroll = total > rows ? total - rows : 0;
    if (g_log_scroll < 0) g_log_scroll = 0;
    for (int r = 0; r < rows; ++r)
    {
        const int idx = g_log_n - 1 - g_log_scroll - (rows - 1 - r);
        if (idx < 0 || idx < g_log_n - kLogLines) continue;
        const char *l = g_log[idx % kLogLines];
        const std::uint32_t col = l[1] == 'E' ? C_ERR : l[1] == 'W' ? C_WARN : C_TXT;
        gfx::text(60, 140 + r * 22, l, 1, col, 180);
    }
    pthread_mutex_unlock(&g_mu);
}

void draw_main()
{
    View v;
    char lastlog[3][kLogLen];
    pthread_mutex_lock(&g_mu);
    v = g_v;
    for (int i = 0; i < 3; ++i)
    {
        const int idx = g_log_n - 3 + i;
        std::snprintf(lastlog[i], kLogLen, "%s", idx >= 0 && idx >= g_log_n - kLogLines ? g_log[idx % kLogLines] : "");
    }
    pthread_mutex_unlock(&g_mu);
    const int busy = g_busy;

    gfx::fill(0, 0, gfx::W, gfx::H, C_BG);
    gfx::text(60, 28, "MKWII Retro Rewind Patcher", 3, C_ACC);
    gfx::text(60, 100, "Mario Kart Wii - Retro Rewind updater and patcher, running fully on this console", 1, C_DIM);
    gfx::text(1500, 40, "PPSA97531", 2, C_DIM);

    // menu
    gfx::fill(60, 150, 700, 520, C_PANEL);
    for (int i = 0; i < I_COUNT; ++i)
    {
        const int y = 170 + i * 76;
        if (i == g_sel) { gfx::fill(70, y - 8, 680, 64, C_SEL); gfx::frame(70, y - 8, 680, 64, 2, C_ACC); }
        char buf[128];
        if (i == I_REGION) std::snprintf(buf, sizeof buf, "< %s >", region_name(v.region));
        else std::snprintf(buf, sizeof buf, "%s", item_label(i));
        const bool dis = busy && i != I_LOG;
        if (i == I_REGION)
        {
            gfx::text(90, y - 2, "Region", 1, C_DIM);
            gfx::text(90, y + 18, buf, 2, dis ? C_DIM : (v.region ? C_TXT : C_WARN));
        }
        else gfx::text(90, y + 8, buf, 2, dis ? C_DIM : C_TXT, 30);
    }
    gfx::text(70, 640, busy ? "Circle: cancel the running job" : "Cross: select   Left/Right: region", 1, C_DIM);

    // status
    gfx::fill(800, 150, 1060, 520, C_PANEL);
    int y = 170;
    auto row = [&](const char *k, const char *val, std::uint32_t col) {
        gfx::text(830, y, k, 2, C_DIM);
        gfx::text(1130, y, val, 2, col, 36);
        y += 48;
    };
    row("Installed:", v.installed, C_TXT);
    row("Latest:", v.latest, C_TXT);
    row("Updates:", v.update_note, std::strcmp(v.update_note, "up to date") == 0 ? C_OK : C_TXT);
    row("Access:", v.access_ok ? "OK (can use /data)" : "NO ACCESS to /data", v.access_ok ? C_OK : C_ERR);
    if (!v.access_ok) { wrap_text(850, y - 4, v.access_line, 1, C_WARN, 95, 2); y += 52; }
    gfx::text(830, y, "Base game:", 2, C_DIM); y += 40;
    wrap_text(850, y, v.base_line, 1, v.base_ok ? C_OK : C_ERR, 95, 3); y += 84;
    gfx::text(830, y, "Output:", 2, C_DIM); y += 40;
    wrap_text(850, y, v.out_line, 1, C_TXT, 95, 2);

    // progress / result
    gfx::fill(60, 700, 1800, 150, C_PANEL);
    if (busy)
    {
        char buf[160];
        const int pct = v.total ? (int)(v.done * 100 / v.total) : 0;
        if (v.total) std::snprintf(buf, sizeof buf, "%s  %d%%", v.phase[0] ? v.phase : "working", pct);
        else std::snprintf(buf, sizeof buf, "%s ...", v.phase[0] ? v.phase : "working");
        gfx::text(90, 720, buf, 2, C_ACC);
        gfx::frame(90, 780, 1740, 36, 2, C_DIM);
        if (v.total) gfx::fill(94, 784, (int)(1732ull * (v.done > v.total ? v.total : v.done) / v.total), 28, C_ACC);
        else gfx::fill(94 + (int)((g_log_n * 37) % 1500), 784, 230, 28, C_ACC);
    }
    else if (v.result[0]) wrap_text(90, 720, v.result, 2, v.result_is_error ? C_ERR : C_OK, 85, 3);
    else gfx::text(90, 720, "Ready. Check for updates, install them, choose a region, then patch.", 2, C_DIM);

    // log tail
    gfx::text(60, 880, "Log", 1, C_DIM);
    for (int i = 0; i < 3; ++i)
    {
        const char *l = lastlog[i];
        gfx::text(60, 906 + i * 24, l, 1, l[1] == 'E' ? C_ERR : l[1] == 'W' ? C_WARN : C_DIM, 180);
    }
    gfx::text(60, 1010, "Square: refresh status / retry access    Options: view log    Safe by design: your base image is only read, and a failed patch never replaces a valid output.", 1, C_DIM, 190);
}

void activate(int item)
{
    switch (item)
    {
    case I_CHECK: start_job(J_CHECK); break;
    case I_INSTALL: start_job(J_INSTALL); break;
    case I_PATCH:
        if (!g_v.region) { set_result(true, "Choose a region first (Left/Right on the Region line). The patch is never started without it."); break; }
        start_job(J_PATCH);
        break;
    case I_VERIFY:
        if (!g_v.region) { set_result(true, "Choose the region of the output you want to verify first."); break; }
        start_job(J_VERIFY);
        break;
    case I_LOG: g_log_view = true; g_log_scroll = 0; break;
    default: break;
    }
}

void change_region(int dir)
{
    if (g_busy != J_NONE) return;
    static const char order[] = {'E', 'P', 'J'};
    int cur = -1;
    for (int i = 0; i < 3; ++i) if (order[i] == g_v.region) cur = i;
    cur = cur < 0 ? (dir > 0 ? 0 : 2) : (cur + dir + 3) % 3;
    pthread_mutex_lock(&g_mu);
    g_v.region = order[cur];
    pthread_mutex_unlock(&g_mu);
    save_region(order[cur]);
    add_log(RR_LOG_INFO, region_name(order[cur]));
    start_job(J_REFRESH);
}

// Startup diagnostics: which storage folders can this sandboxed title see, list and write?
void probe_dirs()
{
    char m[260];
    for (const char *d : {"/data", "/data/porpoise", kRoot, kBase, kPack, kBuilder, kOut, "/download0"})
    {
        const int kind = rr_path_kind(d);
        int err = 0, n = 0;
        rr_dir *h = rr_opendir(d);
        if (!h) err = errno;
        else
        {
            while (rr_readdir(h)) ++n;
            rr_closedir(h);
        }
        if (h) std::snprintf(m, sizeof m, "probe %s: kind=%d list ok (%d entries)", d, kind, n);
        else std::snprintf(m, sizeof m, "probe %s: kind=%d list FAILED errno=%d (%s)", d, kind, err, std::strerror(err));
        add_log(h ? RR_LOG_INFO : RR_LOG_WARN, m);
        if (h && n == 0 && kind == 2) // an empty listing is suspicious: record the raw kernel data
        {
            char raw[300];
            rr_dir_debug(d, raw, sizeof raw);
            add_log(RR_LOG_WARN, raw);
        }
    }
    for (const char *d : {kRoot, kOut})
    {
        char t[200];
        std::snprintf(t, sizeof t, "%s/.write-test", d);
        const int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        std::snprintf(m, sizeof m, "probe write %s: %s", d, fd >= 0 ? "ok" : std::strerror(errno));
        add_log(fd >= 0 ? RR_LOG_INFO : RR_LOG_WARN, m);
        if (fd >= 0) { close(fd); (void)unlink(t); }
    }
}

void set_access(elevate::Probe p)
{
    if (p.ok)
    {
        // The folders the app writes to must be writable too, not just /data/porpoise itself.
        for (const char *d : {kPack, kOut, kState, kLogDir})
        {
            char m[240];
            if (rr_path_kind(d) == 2 && !elevate::writable(d, m, sizeof m))
            {
                p.ok = false;
                std::snprintf(p.summary, sizeof p.summary, "%s", m);
                break;
            }
        }
    }
    pthread_mutex_lock(&g_mu);
    g_v.access_ok = p.ok;
    std::snprintf(g_v.access_line, sizeof g_v.access_line, "%s", p.ok ? "OK - all working folders are writable" : p.summary);
    pthread_mutex_unlock(&g_mu);
    add_log(p.ok ? RR_LOG_INFO : RR_LOG_WARN, p.summary);
    g_dirty = 1;
}

// Runs on the main thread only, before any worker thread exists.
void first_launch_access()
{
    elevate::Probe p = elevate::probe();
    if (p.ok) { set_access(p); return; }
    add_log(RR_LOG_WARN, p.summary);
    char detail[200];
    elevate::request(detail, sizeof detail);
    add_log(RR_LOG_INFO, detail);
    pthread_mutex_lock(&g_mu);
    std::snprintf(g_v.access_line, sizeof g_v.access_line, "requesting jailbreak access (waiting for OnionHEN / daemon)...");
    pthread_mutex_unlock(&g_mu);
    // Wait up to ~40 s for the daemon to act, redrawing so the user sees what is happening.
    for (int i = 0; i < 80; ++i)
    {
        g_dirty = 1;
        draw_main();
        gfx::present();
        for (int k = 0; k < 30; ++k) gfx::wait_vblank(); // ~0.5 s
        p = elevate::probe();
        if (p.ok) { add_log(RR_LOG_INFO, "jailbreak access granted"); set_access(p); return; }
    }
    set_access(p);
    add_log(RR_LOG_ERROR, "No /data access. Add PPSA97531 to exact_title_ids under [app_jailbreak] in /data/OnionHEN/config.ini, then relaunch.");
}

} // namespace

void app_run()
{
    const char *why = "";
    const bool video = gfx::init(&why);
    if (video) first_launch_access();
    for (const char *d : {kRoot, kBase, kPack, kBuilder, kState, kLogDir, kOut}) (void)rr_mkdir_p(d);
    if (video) set_access(elevate::probe());
    add_log(RR_LOG_INFO, "MKWII Retro Rewind Patcher PPSA97531 started");
    {
        char m[120];
        std::snprintf(m, sizeof m, "process ids: uid=%d euid=%d (a jailbroken app normally shows 0)", (int)getuid(), (int)geteuid());
        add_log(RR_LOG_INFO, m);
    }
    probe_dirs();
    if (!video)
    {
        char m[128];
        std::snprintf(m, sizeof m, "MKWII Retro Rewind Patcher: %s", why);
        gfx::notify(m);
        add_log(RR_LOG_ERROR, m);
        for (;;) sceKernelUsleep(1000000);
    }
    const bool have_pad = pad::init();
    if (!have_pad) add_log(RR_LOG_WARN, "no controller could be opened");
    g_v.region = load_region();
    gfx::notify("MKWII Retro Rewind Patcher ready");
    start_job(J_REFRESH);

    unsigned frame = 0;
    for (;;)
    {
        const unsigned p = pad::poll();
        if (g_log_view)
        {
            if (p & pad::circle) { g_log_view = false; g_dirty = 1; }
            if (p & pad::up) { g_log_scroll += 1; g_dirty = 1; }
            if (p & pad::down) { g_log_scroll -= 1; g_dirty = 1; }
            if (p & pad::l1) { g_log_scroll += 20; g_dirty = 1; }
            if (p & pad::r1) { g_log_scroll -= 20; g_dirty = 1; }
        }
        else if (p)
        {
            g_dirty = 1;
            if (p & pad::up) g_sel = (g_sel + I_COUNT - 1) % I_COUNT;
            if (p & pad::down) g_sel = (g_sel + 1) % I_COUNT;
            if (g_sel == I_REGION && (p & pad::left)) change_region(-1);
            if (g_sel == I_REGION && (p & pad::right)) change_region(+1);
            if (p & pad::cross) { if (g_sel == I_REGION) change_region(+1); else activate(g_sel); }
            if (p & pad::circle) { if (g_busy != J_NONE) { g_cancel = 1; add_log(RR_LOG_WARN, "cancel requested"); } }
            if (p & pad::square)
            {
                if (!g_v.access_ok && g_busy == J_NONE)
                {
                    char d[200];
                    elevate::request(d, sizeof d);
                    add_log(RR_LOG_INFO, d);
                    set_access(elevate::probe());
                }
                start_job(J_REFRESH);
            }
            if (p & pad::options) { g_log_view = true; g_log_scroll = 0; }
        }
        if (g_busy != J_NONE && (++frame % 6) == 0) g_dirty = 1; // animate progress
        if (g_dirty)
        {
            g_dirty = 0;
            if (g_log_view) draw_log_view(); else draw_main();
            gfx::present();
        }
        gfx::wait_vblank();
    }
}
