// Porpoise Retro Rewind - first-launch privilege request and /data access probe.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace elevate
{
struct Probe
{
    bool ok;               // /data/porpoise can be created and written
    char summary[220];     // human readable: what worked / what failed, with errno names
};
Probe probe();             // no side effects beyond creating /data/porpoise and a tiny test file
// Asks the resident jailbreak daemon (OnionHEN app jailbreak, etaHEN-style marker, Lapy daemon) to elevate
// this process. MUST be called from the only thread, before any other thread is created.
// Returns true if at least one request marker was published (not an acknowledgement).
bool request(char *detail, unsigned cap);
const char *errname(int e);
// Can this app create a file inside dir? On failure msg explains (errno name and what to do).
bool writable(const char *dir, char *msg, unsigned cap);
} // namespace elevate
