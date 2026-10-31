// Porpoise Retro Rewind - first-launch privilege request and /data access probe.
//
// A PPSA title starts sandboxed. The jailbreak environment can lift that for an allowlisted title when the title
// asks for it from inside its own sandbox:
//   * OnionHEN "app jailbreak": the title appears in [app_jailbreak] exact_title_ids / title_id_prefixes of
//     /data/OnionHEN/config.ini and drops a marker file (download0/onionhen_jailbreak, etaHEN compatible name
//     download0/etahen_jailbreak) in its writable /download0.
//   * PS5-Lapy-JB-Daemon: marker /download0/elevate_proc containing {"PID":<pid>}, after seteuid(geteuid()) made
//     in the only thread of the process.
// All markers live in the app's own /download0 and are harmless when nothing is listening.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "elevate.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace elevate
{
const char *errname(int e)
{
    switch (e)
    {
    case 0: return "ok";
    case EPERM: return "EPERM (operation not permitted)";
    case ENOENT: return "ENOENT (path does not exist / not visible)";
    case EACCES: return "EACCES (permission denied)";
    case EEXIST: return "EEXIST";
    case ENOTDIR: return "ENOTDIR";
    case EROFS: return "EROFS (read-only)";
    case ENOSPC: return "ENOSPC (disk full)";
    case EIO: return "EIO";
    default: return "errno";
    }
}

namespace
{
int mkdir_step(const char *p)
{
    if (mkdir(p, 0777) == 0) return 0;
    const int e = errno;
    struct stat st;
    if (e == EEXIST && stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    errno = e;
    return -1;
}

bool write_marker(const char *final_path, const char *body, size_t n)
{
    char tmp[96];
    std::snprintf(tmp, sizeof tmp, "/download0/.porpoise_tmp.%ld", (long)getpid());
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    const bool ok = write(fd, body, n) == (long)n;
    close(fd);
    if (!ok || rename(tmp, final_path) != 0)
    {
        unlink(tmp);
        return false;
    }
    return true;
}
} // namespace

Probe probe()
{
    Probe r{};
    struct stat st;
    char buf[sizeof r.summary];
    if (stat("/data", &st) != 0)
    {
        const int e = errno;
        std::snprintf(r.summary, sizeof r.summary, "/data is not reachable from this app: %s", errname(e));
        return r;
    }
    if (mkdir_step("/data/porpoise") != 0)
    {
        const int e = errno;
        std::snprintf(r.summary, sizeof r.summary, "cannot create /data/porpoise: %s", errname(e));
        return r;
    }
    std::snprintf(buf, sizeof buf, "/data/porpoise/.write_test.%ld", (long)getpid());
    const int fd = open(buf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        const int e = errno;
        std::snprintf(r.summary, sizeof r.summary, "cannot write inside /data/porpoise: %s", errname(e));
        return r;
    }
    close(fd);
    unlink(buf);
    r.ok = true;
    std::snprintf(r.summary, sizeof r.summary, "/data/porpoise is readable and writable");
    return r;
}

bool writable(const char *dir, char *msg, unsigned cap)
{
    char t[300];
    std::snprintf(t, sizeof t, "%s/.porpoise_write_test.%ld", dir, (long)getpid());
    const int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        const int e = errno;
        std::snprintf(msg, cap, "%s is not writable by the app: %s. Folders made over FTP belong to root: set them to 777 (recursive) or delete them and let the app recreate them.", dir, errname(e));
        return false;
    }
    close(fd);
    unlink(t);
    return true;
}

bool request(char *detail, unsigned cap)
{
    // Lapy: the native same-UID call that gives this process a private credential; must precede any thread.
    const bool seteuid_ok = seteuid(geteuid()) == 0;
    char body[64];
    const int n = std::snprintf(body, sizeof body, "{\"PID\":%ld}\n", (long)getpid());
    bool any = false;
    // Only publish the Lapy request when the credential step worked, as that daemon requires.
    if (seteuid_ok && write_marker("/download0/elevate_proc", body, (size_t)n)) any = true;
    // OnionHEN / etaHEN style marker files.
    if (write_marker("/download0/onionhen_jailbreak", body, (size_t)n)) any = true;
    if (write_marker("/download0/etahen_jailbreak", body, (size_t)n)) any = true;
    std::snprintf(detail, cap, any ? "jailbreak request published (seteuid %s)" : "could not publish jailbreak request: /download0 not writable (%s)",
                  any ? (seteuid_ok ? "ok" : "failed") : errname(errno));
    return any;
}
} // namespace elevate
