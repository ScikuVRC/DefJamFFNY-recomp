/*
 * Netplay sessions (docs/netplay.md).
 *
 * Two players agree on a match over one TCP connection, then both games start it in
 * lockstep mode (lockstep.c), where the same inputs play the same fight. This file
 * is the agreement and the start:
 *
 *   1. both send   HELLO <protocol> <version> <program> <game data>
 *      and refuse a peer whose protocol, version or fingerprints differ: a
 *      different build or a different dump would play a different fight;
 *   2. the host sends MATCH <seed> <venue> <fighter 0> <fighter 1>, the joiner
 *      answers READY and the host GO; the launcher (or a scripted run) starts the
 *      game, which goes straight from the main menu into that match through the
 *      front end's own commands (fe_inject.c);
 *   3. at the first fight step each game sends AT 1 and waits for the other's,
 *      so neither starts while the other is still loading.
 *
 * Inputs are not exchanged yet (the next step): until then the local pad drives
 * slot 0 on both machines and slot 1 is the CPU.
 * Messages are text lines. The worker thread owns the socket; the game and the
 * launcher read the state under a lock.
 */
#define _CRT_SECURE_NO_WARNINGS
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netplay.h"

#ifndef DEFJAM_VERSION
#define DEFJAM_VERSION "dev"
#endif
#define PROTOCOL        1
#define BARRIER_MS      120000      /* how long a game waits for the other at the first step */

/* The match both games start: One on One at the default venue, the harness's fighters. */
#define DEFAULT_VENUE   1
#define DEFAULT_F0      56
#define DEFAULT_F1      55

/* ---- Platform: a lock, a thread, sleeping, sockets ------------------------------ */
#if defined(_WIN32)
static CRITICAL_SECTION s_lock;
static void lock_init(void) { InitializeCriticalSection(&s_lock); }
static void lock(void) { EnterCriticalSection(&s_lock); }
static void unlock(void) { LeaveCriticalSection(&s_lock); }
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static DWORD WINAPI worker_entry(LPVOID arg);
static int start_thread(void) { return CreateThread(NULL, 0, worker_entry, NULL, 0, NULL) != NULL; }
static int net_init(void) { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }
#else
static pthread_mutex_t s_lock_m = PTHREAD_MUTEX_INITIALIZER;
static void lock_init(void) {}
static void lock(void) { pthread_mutex_lock(&s_lock_m); }
static void unlock(void) { pthread_mutex_unlock(&s_lock_m); }
static void sleep_ms(int ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
static void *worker_entry(void *arg);
static int start_thread(void) { pthread_t t; return pthread_create(&t, NULL, worker_entry, NULL) == 0; }
static int net_init(void) { return 1; }
#endif

/* ---- Session state (under the lock) ------------------------------------------- */
static int s_inited;
static volatile int s_cancel;
static NetplayState s_state = NETPLAY_IDLE;
static char s_status[160];
static NetplayMatch s_match;
static int s_hosting;
static uint16_t s_port;
static char s_address[128];
static sock_t s_sock = SOCK_BAD;
static long s_peer_at = -1;         /* the highest step the other game has reached */
static char s_fp_program[24], s_fp_data[24];

static void set_state(NetplayState state, const char *format, ...)
{
    va_list args;
    lock();
    s_state = state;
    va_start(args, format);
    vsnprintf(s_status, sizeof s_status, format, args);
    va_end(args);
    unlock();
    fprintf(stderr, "[NETPLAY] %s\n", s_status);
    fflush(stderr);
}

NetplayState netplay_state(void)
{
    NetplayState state;
    lock();
    state = s_state;
    unlock();
    return state;
}

void netplay_status(char *out, int size)
{
    lock();
    snprintf(out, (size_t)size, "%s", s_status);
    unlock();
}

int netplay_match(NetplayMatch *out)
{
    int ok;
    lock();
    ok = s_state == NETPLAY_READY;
    if (ok) *out = s_match;
    unlock();
    return ok;
}

/* ---- Fingerprints: this program and the game data -------------------------------- */
static int file_fingerprint(const char *path, char *out, size_t size)
{
    unsigned char buf[65536];
    uint64_t h = 1469598103934665603ull;
    size_t n, i;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(out, size, "none"); return 0; }
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        for (i = 0; i < n; i++)
            h = (h ^ buf[i]) * 1099511628211ull;
    fclose(f);
    snprintf(out, size, "%016llx", (unsigned long long)h);
    return 1;
}

static void fingerprints(void)
{
    char exe[1024] = "";
#if defined(_WIN32)
    GetModuleFileNameA(NULL, exe, sizeof exe);
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) exe[n] = 0;
#endif
    file_fingerprint(exe, s_fp_program, sizeof s_fp_program);
    file_fingerprint("game/default.xbe", s_fp_data, sizeof s_fp_data);
}

/* ---- Lines over the socket ---------------------------------------------------- */
static char s_in[1024];
static size_t s_in_len;

static int send_line(const char *format, ...)
{
    char line[256];
    va_list args;
    int n;
    va_start(args, format);
    n = vsnprintf(line, sizeof line - 1, format, args);
    va_end(args);
    if (n < 0 || n > (int)sizeof line - 2) return 0;
    line[n++] = '\n';
    return send(s_sock, line, n, 0) == n;
}

/* The next line from the peer into `out`: 1, 0 when none came within `wait_ms`,
 * -1 when the connection is gone. */
static int recv_line(char *out, size_t size, int wait_ms)
{
    for (;;) {
        char *nl = memchr(s_in, '\n', s_in_len);
        fd_set read_set;
        struct timeval tv;
        int n;
        if (nl) {
            size_t len = (size_t)(nl - s_in);
            if (len >= size) len = size - 1;
            memcpy(out, s_in, len);
            out[len] = 0;
            if (len && out[len - 1] == '\r') out[len - 1] = 0;
            s_in_len -= (size_t)(nl - s_in) + 1;
            memmove(s_in, nl + 1, s_in_len);
            return 1;
        }
        if (s_in_len == sizeof s_in) return -1;     /* a line too long: not our peer */
        FD_ZERO(&read_set);
        FD_SET(s_sock, &read_set);
        tv.tv_sec = wait_ms / 1000;
        tv.tv_usec = (wait_ms % 1000) * 1000;
        n = select((int)s_sock + 1, &read_set, NULL, NULL, &tv);
        if (n < 0) return -1;
        if (n == 0) return 0;
        n = recv(s_sock, s_in + s_in_len, (int)(sizeof s_in - s_in_len), 0);
        if (n <= 0) return -1;
        s_in_len += (size_t)n;
    }
}

/* ---- Connecting ----------------------------------------------------------------- */
static sock_t listen_and_accept(uint16_t port)
{
    struct sockaddr_in addr;
    sock_t server = socket(AF_INET, SOCK_STREAM, 0), peer = SOCK_BAD;
    int yes = 1;
    if (server == SOCK_BAD) return SOCK_BAD;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(server, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(server, 1) != 0) {
        sock_close(server);
        set_state(NETPLAY_FAILED, "Could not listen on port %u (is another program using it?)", port);
        return SOCK_BAD;
    }
    set_state(NETPLAY_LISTENING, "Hosting on port %u: waiting for the other player", port);
    while (!s_cancel) {
        fd_set read_set;
        struct timeval tv = {0, 200000};
        FD_ZERO(&read_set);
        FD_SET(server, &read_set);
        if (select((int)server + 1, &read_set, NULL, NULL, &tv) > 0) {
            peer = accept(server, NULL, NULL);
            break;
        }
    }
    sock_close(server);
    return peer;
}

static sock_t connect_to(const char *address)
{
    char host[128];
    const char *colon = strrchr(address, ':');
    const char *port = colon ? colon + 1 : NULL;
    char port_text[8];
    struct addrinfo hints, *found = NULL, *a;
    sock_t sock = SOCK_BAD;
    size_t len = colon ? (size_t)(colon - address) : strlen(address);
    if (len >= sizeof host) len = sizeof host - 1;
    memcpy(host, address, len);
    host[len] = 0;
    if (!port || !*port) {
        snprintf(port_text, sizeof port_text, "%d", NETPLAY_DEFAULT_PORT);
        port = port_text;
    }
    set_state(NETPLAY_CONNECTING, "Connecting to %s:%s", host, port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &found) != 0) {
        set_state(NETPLAY_FAILED, "Could not find %s", host);
        return SOCK_BAD;
    }
    /* The host may still be starting: try for a while. */
    while (!s_cancel && sock == SOCK_BAD) {
        for (a = found; a && sock == SOCK_BAD; a = a->ai_next) {
            sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (sock != SOCK_BAD && connect(sock, a->ai_addr, (int)a->ai_addrlen) != 0) {
                sock_close(sock);
                sock = SOCK_BAD;
            }
        }
        if (sock == SOCK_BAD) sleep_ms(500);
    }
    freeaddrinfo(found);
    return sock;
}

/* ---- The session ---------------------------------------------------------------- */
static int handshake(void)
{
    char line[256], version[64], program[24], data[24];
    int protocol, got;
    set_state(NETPLAY_HANDSHAKE, "Connected: checking that both games match");
    if (!send_line("HELLO %d %s %s %s", PROTOCOL, DEFJAM_VERSION, s_fp_program, s_fp_data)) return 0;
    while ((got = recv_line(line, sizeof line, 200)) == 0)
        if (s_cancel) return 0;
    if (got < 0) { set_state(NETPLAY_FAILED, "The other player left"); return 0; }
    if (sscanf(line, "HELLO %d %63s %23s %23s", &protocol, version, program, data) != 4) {
        set_state(NETPLAY_FAILED, "The other side is not Def Jam netplay");
        return 0;
    }
    if (protocol != PROTOCOL || strcmp(version, DEFJAM_VERSION)) {
        set_state(NETPLAY_FAILED, "Different versions: this game is %s, the other is %s", DEFJAM_VERSION, version);
        return 0;
    }
    if (strcmp(program, s_fp_program)) {
        set_state(NETPLAY_FAILED, "The two games are different builds of %s; both need the same one", version);
        return 0;
    }
    if (strcmp(data, s_fp_data)) {
        set_state(NETPLAY_FAILED, "The game data differs (default.xbe); both need the same dump");
        return 0;
    }

    if (s_hosting) {
        NetplayMatch m;
        const char *fixed = getenv("RECOMP_NETPLAY_SEED");
        memset(&m, 0, sizeof m);
#if defined(_WIN32)
        m.seed = fixed && *fixed ? (uint32_t)strtoul(fixed, NULL, 0) : (uint32_t)GetTickCount() ^ (uint32_t)GetCurrentProcessId() * 2654435761u;
#else
        m.seed = fixed && *fixed ? (uint32_t)strtoul(fixed, NULL, 0) : (uint32_t)time(NULL) ^ (uint32_t)getpid() * 2654435761u;
#endif
        m.venue = DEFAULT_VENUE;
        m.fighter[0] = DEFAULT_F0;
        m.fighter[1] = DEFAULT_F1;
        m.local_slot = 0;
        if (!send_line("MATCH %u %d %d %d", m.seed, m.venue, m.fighter[0], m.fighter[1])) return 0;
        while ((got = recv_line(line, sizeof line, 200)) == 0)
            if (s_cancel) return 0;
        if (got < 0 || strcmp(line, "READY")) { set_state(NETPLAY_FAILED, "The other player left"); return 0; }
        if (!send_line("GO")) return 0;
        lock(); s_match = m; unlock();
    } else {
        NetplayMatch m;
        memset(&m, 0, sizeof m);
        while ((got = recv_line(line, sizeof line, 200)) == 0)
            if (s_cancel) return 0;
        if (got < 0 || sscanf(line, "MATCH %u %d %d %d", &m.seed, &m.venue, &m.fighter[0], &m.fighter[1]) != 4) {
            set_state(NETPLAY_FAILED, "The host left before agreeing a match");
            return 0;
        }
        m.local_slot = 1;
        if (!send_line("READY")) return 0;
        while ((got = recv_line(line, sizeof line, 200)) == 0)
            if (s_cancel) return 0;
        if (got < 0 || strcmp(line, "GO")) { set_state(NETPLAY_FAILED, "The host left"); return 0; }
        lock(); s_match = m; unlock();
    }
    set_state(NETPLAY_READY, "Match agreed (seed %u): starting", s_match.seed);
    return 1;
}

/* After the agreement the worker only listens: AT <step> from the other game. */
static void listen_in_game(void)
{
    char line[256];
    for (;;) {
        int got = recv_line(line, sizeof line, 500);
        long step;
        if (got < 0) {
            fprintf(stderr, "[NETPLAY] the other player's game disconnected\n");
            fflush(stderr);
            lock(); s_peer_at = -2; unlock();
            return;
        }
        if (got > 0 && sscanf(line, "AT %ld", &step) == 1) {
            lock();
            if (step > s_peer_at) s_peer_at = step;
            unlock();
        }
    }
}

#if defined(_WIN32)
static DWORD WINAPI worker_entry(LPVOID arg)
#else
static void *worker_entry(void *arg)
#endif
{
    (void)arg;
    s_sock = s_hosting ? listen_and_accept(s_port) : connect_to(s_address);
    if (s_sock == SOCK_BAD) {
        if (netplay_state() != NETPLAY_FAILED)
            set_state(NETPLAY_FAILED, s_cancel ? "Cancelled" : "Could not connect");
        return 0;
    }
    {
        int yes = 1;
        setsockopt(s_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof yes);
    }
    if (!handshake()) {
        if (netplay_state() != NETPLAY_FAILED)
            set_state(NETPLAY_FAILED, s_cancel ? "Cancelled" : "The connection was lost");
        sock_close(s_sock);
        s_sock = SOCK_BAD;
        return 0;
    }
    listen_in_game();
    return 0;
}

static int start_session(void)
{
    if (!s_inited) {
        lock_init();
        if (!net_init()) return 0;
        s_inited = 1;
    }
    if (netplay_state() != NETPLAY_IDLE && netplay_state() != NETPLAY_FAILED) return 0;
    fingerprints();
    s_cancel = 0;
    s_in_len = 0;
    s_peer_at = -1;
    if (!start_thread()) {
        set_state(NETPLAY_FAILED, "Could not start the network thread");
        return 0;
    }
    return 1;
}

int netplay_host(uint16_t port)
{
    s_hosting = 1;
    s_port = port ? port : NETPLAY_DEFAULT_PORT;
    return start_session();
}

int netplay_join(const char *address)
{
    s_hosting = 0;
    snprintf(s_address, sizeof s_address, "%s", address);
    return start_session();
}

void netplay_cancel(void)
{
    s_cancel = 1;
}

/* ---- Starting the game ----------------------------------------------------------- */
static void put_env(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static int s_session_live;          /* this process plays a netplay match */

int netplay_startup(void)
{
    const char *host = getenv("RECOMP_NETPLAY_HOST"), *join = getenv("RECOMP_NETPLAY_JOIN");
    NetplayMatch m;
    char text[512];
    if (s_inited && netplay_state() == NETPLAY_IDLE) return 1;
    if (!s_inited || netplay_state() == NETPLAY_FAILED) {
        if (host && *host) netplay_host((uint16_t)atoi(host));
        else if (join && *join) netplay_join(join);
        else return 1;                              /* no netplay asked for */
        while (netplay_state() != NETPLAY_READY && netplay_state() != NETPLAY_FAILED)
            sleep_ms(50);
    }
    if (!netplay_match(&m)) return 0;
    snprintf(text, sizeof text, "%u", m.seed);
    put_env("RECOMP_LOCKSTEP", "1");
    put_env("RECOMP_LOCKSTEP_SEED", text);
    /* Straight from the main menu into the match, with the Battle menus' own
     * commands, the same on both machines so both build the same match. Slot 1 is
     * set up as the CPU: a human slot 1 needs a second pad plugged in, and match
     * set-up crashed without one (sub_001BBC10 on slot 1's record). The fighters'
     * pad words are replaced every step from the exchanged inputs instead (the same
     * write the step-timed test input makes), which drives a CPU slot as well. */
    snprintf(text, sizeof text,
             "getscreeninfo(intmain/mainmenu#1=>Game.StartBattleMode();Game.SetMatchType(0);"
             "Game.SetMatchRules(0);Controller.ControllerBind(0);Controller.ControllerSetup(0,0,%d,0);"
             "Controller.ControllerSetup(1,-1,%d,1);Game.ChooseVenue(%d);Game.StartGame()",
             m.fighter[0], m.fighter[1], m.venue);
    put_env("RECOMP_FE_INJECT", text);
    s_session_live = 1;
    fprintf(stderr, "[NETPLAY] starting the match as player %d (seed %u, venue %d, fighters %d and %d)\n",
            m.local_slot + 1, m.seed, m.venue, m.fighter[0], m.fighter[1]);
    fflush(stderr);
    return 1;
}

/* ---- In game: the start barrier ---------------------------------------------------- */
static long s_step;

void netplay_match_start(void)
{
    s_step = 0;
}

void netplay_step(void)
{
    int waited = 0;
    if (!s_session_live) return;
    s_step++;
    if (s_step != 1) return;
    /* Both games are at the first step of the same match; whichever got here first
     * waits for the other, so the fight starts on both together. */
    send_line("AT %ld", s_step);
    fprintf(stderr, "[NETPLAY] at the first step; waiting for the other player\n");
    fflush(stderr);
    for (;;) {
        long peer;
        lock(); peer = s_peer_at; unlock();
        if (peer >= s_step) break;
        if (peer == -2 || waited >= BARRIER_MS) {
            fprintf(stderr, "[NETPLAY] the other player %s; playing on alone\n",
                    peer == -2 ? "disconnected" : "did not arrive");
            fflush(stderr);
            s_session_live = 0;
            return;
        }
        sleep_ms(1);
        waited++;
    }
    fprintf(stderr, "[NETPLAY] both players at the first step after %d ms: fight\n", waited);
    fflush(stderr);
}
