/*
 * sites.h - the list of things to health-check
 * ============================================
 *
 * This is the only file you need to touch to add or remove a site.
 * Edit the list, save, re-upload. Nothing else in main.ino changes.
 *
 * Format, one line per site:
 *
 *     SITE("label in Telegram", "https://full.url/including/path")
 *
 * Rules of thumb:
 *   - Keep the label short; it is what shows up in the alert and in /status.
 *   - Include the scheme. https:// and http:// both work.
 *   - A trailing "/" is fine. So is a deeper path if you want to check a
 *     specific endpoint, e.g. "https://git.example.org/api/v1/version".
 *   - Comment a line out with // to stop checking it without deleting it.
 *
 * What counts as UP:
 *   Any HTTP status below 500. That means 200, 301, 401 and 403 all count
 *   as alive, because the server answered. 502/503/504, a refused
 *   connection, a DNS failure or a TLS error all count as DOWN.
 *
 *   If a site normally sits behind auth and returns 401, that is still UP.
 *   If you want to catch "it answers but serves garbage", point the URL at
 *   a health endpoint that only returns 200 when the app is really healthy.
 *
 * Timing:
 *   Every site is checked once per cycle (PING_INTERVAL, 60s by default).
 *   Each check can take up to SITE_TIMEOUT_MS (8s) before it gives up, so
 *   keep the list shorter than about 6 entries or a cycle full of timeouts
 *   will run past the next cycle. Adjust the timing in main.ino if needed.
 */

#ifndef SITES_H
#define SITES_H

// ---------------------------------------------------------------------------
// >>> YOUR SITES GO HERE <<<
// ---------------------------------------------------------------------------

SITE("dllmch.org", "https://dllmch.org/")
SITE("sy19.org",   "https://sy19.org/")

// --- examples, delete or replace with your real subdomains ---
// SITE("git.dllmch.org",   "https://git.dllmch.org/")
// SITE("cloud.dllmch.org", "https://cloud.dllmch.org/")
// SITE("media.sy19.org",   "https://media.sy19.org/")

// ---------------------------------------------------------------------------

#endif  // SITES_H
