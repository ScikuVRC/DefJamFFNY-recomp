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
#include "recomp/gen/recomp_types.h"
#include "test_observations.h"

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
/* The lock exists from the first session on; before it there is no worker thread, and
 * the launcher reads the state every frame, so the readers skip it until then. */
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
static int s_want_venue = DEFAULT_VENUE, s_want_fighter[2] = {DEFAULT_F0, DEFAULT_F1};   /* the host's choice */
static char s_fp_program[24], s_fp_data[24];

static void set_state(NetplayState state, const char *format, ...)
{
    va_list args;
    if (s_inited) lock();
    s_state = state;
    va_start(args, format);
    vsnprintf(s_status, sizeof s_status, format, args);
    va_end(args);
    if (s_inited) unlock();
    fprintf(stderr, "[NETPLAY] %s\n", s_status);
    fflush(stderr);
}

NetplayState netplay_state(void)
{
    NetplayState state;
    if (s_inited) lock();
    state = s_state;
    if (s_inited) unlock();
    return state;
}

void netplay_status(char *out, int size)
{
    if (s_inited) lock();
    snprintf(out, (size_t)size, "%s", s_status);
    if (s_inited) unlock();
}

int netplay_match(NetplayMatch *out)
{
    int ok;
    if (s_inited) lock();
    ok = s_state == NETPLAY_READY;
    if (ok) *out = s_match;
    if (s_inited) unlock();
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
        lock();
        m.venue = s_want_venue;
        m.fighter[0] = s_want_fighter[0];
        m.fighter[1] = s_want_fighter[1];
        unlock();
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

/* ---- Inputs and the desync check (the game thread and the worker share these) ----- */
#define RING            512         /* steps of inputs kept; far more than any delay */
#define ENTRY_WORDS     6           /* what sub_001BAA00 reads of a pad-table entry */
#define SYNC_EVERY      60          /* steps between desync checks */
static uint32_t s_inputs[2][RING][ENTRY_WORDS];
static long s_input_step[2][RING];  /* which step each ring entry holds, per slot */
static uint32_t s_sync_peer[RING / SYNC_EVERY + 1], s_sync_mine[RING / SYNC_EVERY + 1];
static long s_sync_peer_step[RING / SYNC_EVERY + 1], s_sync_mine_step[RING / SYNC_EVERY + 1];

/* After the agreement the worker only listens: AT, IN and SYNC from the other game. */
static void listen_in_game(void)
{
    char line[256];
    for (;;) {
        int got = recv_line(line, sizeof line, 500);
        long step;
        uint32_t w[ENTRY_WORDS], hash;
        if (got < 0) {
            fprintf(stderr, "[NETPLAY] the other player's game disconnected\n");
            fflush(stderr);
            lock(); s_peer_at = -2; unlock();
            return;
        }
        if (got == 0) continue;
        if (sscanf(line, "IN %ld %x %x %x %x %x %x", &step, &w[0], &w[1], &w[2], &w[3], &w[4], &w[5]) == 7
            && step > 0) {
            int remote = 1 - s_match.local_slot;
            lock();
            memcpy(s_inputs[remote][step % RING], w, sizeof w);
            s_input_step[remote][step % RING] = step;
            unlock();
        } else if (sscanf(line, "SYNC %ld %x", &step, &hash) == 2 && step > 0) {
            int i = (int)((step / SYNC_EVERY) % (RING / SYNC_EVERY + 1));
            lock();
            s_sync_peer[i] = hash;
            s_sync_peer_step[i] = step;
            unlock();
        } else if (sscanf(line, "AT %ld", &step) == 1) {
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

void netplay_choose(int venue, int fighter0, int fighter1)
{
    if (s_inited) lock();
    s_want_venue = venue > 0 ? venue : DEFAULT_VENUE;
    s_want_fighter[0] = fighter0 > 0 ? fighter0 : DEFAULT_F0;
    s_want_fighter[1] = fighter1 > 0 ? fighter1 : DEFAULT_F1;
    if (s_inited) unlock();
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
        const char *venue = getenv("RECOMP_NETPLAY_VENUE"), *fighters = getenv("RECOMP_NETPLAY_FIGHTERS");
        int f0 = 0, f1 = 0;
        if (fighters) sscanf(fighters, "%d,%d", &f0, &f1);
        netplay_choose(venue ? atoi(venue) : 0, f0, f1);
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

/* ---- In game: the start barrier, inputs, the desync check ------------------------ */
#define ACTORS          0x003B92E0u /* fighter records, 0x12E8 apart */
#define STRIDE          0x12E8u
#define PAD_TABLE       0x003B8F48u /* pad-table entries, 0x20 apart, one per slot */
#define STALL_MS        10000       /* how long to wait for an input before giving up */

static long s_step;
static int s_delay = -1;
static uint32_t s_current[2][ENTRY_WORDS];  /* this step's input per slot */
static long s_current_step;
static int s_desynced;

static void end_session(const char *why)
{
    fprintf(stderr, "[NETPLAY] %s; the match goes on locally\n", why);
    fflush(stderr);
    s_session_live = 0;
}

/* Wait (with the game paused) until `done` says so; 0 when the other game is gone. */
static int wait_for_peer(int (*done)(void), int limit_ms, int *waited)
{
    *waited = 0;
    for (;;) {
        long peer;
        if (done()) return 1;
        lock(); peer = s_peer_at; unlock();
        if (peer == -2 || *waited >= limit_ms) return 0;
        sleep_ms(1);
        (*waited)++;
    }
}

static int peer_at_first_step(void)
{
    long peer;
    lock(); peer = s_peer_at; unlock();
    return peer >= 1;
}

static int remote_input_here(void)
{
    int remote = 1 - s_match.local_slot, here;
    lock();
    here = s_input_step[remote][s_step % RING] == s_step;
    unlock();
    return here;
}

/* The local player's input: the pad-table entry of slot 0, which the local pad fills
 * on both machines (the match binds pad 0 to slot 0). In a test run with step-timed
 * input, the script's buttons for slot 0 instead. */
static void local_input(long step, uint32_t *out)
{
    uint32_t i, buttons;
    for (i = 0; i < ENTRY_WORDS; i++)
        out[i] = MEM32(PAD_TABLE + i * 4);
    if (defjam_test_scripted_input(0, step, &buttons)) {
        memset(out, 0, ENTRY_WORDS * sizeof *out);
        out[0] = buttons;
    }
}

/* A hash of what the fight is: each fighter's state, position and health. */
static uint32_t fight_hash(void)
{
    static const uint32_t fields[][2] = {{0x30, 4}, {0x44, 12}, {0x54, 4}, {0x350, 8}};
    uint32_t h = 2166136261u, slot, f, i;
    for (slot = 0; slot < 2; slot++)
        for (f = 0; f < sizeof fields / sizeof fields[0]; f++)
            for (i = 0; i < fields[f][1]; i += 4)
                h = (h ^ MEM32(ACTORS + slot * STRIDE + fields[f][0] + i)) * 16777619u;
    return h;
}

static void desync_check(void)
{
    int i = (int)((s_step / SYNC_EVERY) % (RING / SYNC_EVERY + 1));
    int prev = (int)(((s_step / SYNC_EVERY) + RING / SYNC_EVERY) % (RING / SYNC_EVERY + 1));
    uint32_t mine = fight_hash();
    long peer_step;
    uint32_t peer;
    send_line("SYNC %ld %08x", s_step, mine);
    s_sync_mine[i] = mine;
    s_sync_mine_step[i] = s_step;
    /* The other game's hash for the previous check has had a whole interval to arrive. */
    lock();
    peer_step = s_sync_peer_step[prev];
    peer = s_sync_peer[prev];
    unlock();
    if (s_desynced || peer_step != s_step - SYNC_EVERY || s_sync_mine_step[prev] != peer_step) return;
    if (peer != s_sync_mine[prev]) {
        s_desynced = 1;
        fprintf(stderr, "[NETPLAY] DESYNC: the two games differ at step %ld (%08x here, %08x there)\n",
                peer_step, s_sync_mine[prev], peer);
    } else if (peer_step % 600 == 0) {
        fprintf(stderr, "[NETPLAY] in sync at step %ld\n", peer_step);
    }
    fflush(stderr);
}

void netplay_match_start(void)
{
    s_step = 0;
    s_current_step = 0;
    s_desynced = 0;
}

void netplay_step(void)
{
    int local = s_match.local_slot, remote = 1 - local, waited;
    uint32_t mine[ENTRY_WORDS];
    if (!s_session_live) return;
    if (s_delay < 0) {
        const char *d = getenv("RECOMP_NETPLAY_DELAY");
        s_delay = d && *d ? atoi(d) : 3;
        if (s_delay < 1) s_delay = 1;
        if (s_delay > 30) s_delay = 30;
    }
    s_step++;
    if (s_step == 1) {
        /* Both games are at the first step of the same match; whichever got here
         * first waits for the other, so the fight starts on both together. */
        send_line("AT 1");
        fprintf(stderr, "[NETPLAY] at the first step; waiting for the other player\n");
        fflush(stderr);
        if (!wait_for_peer(peer_at_first_step, BARRIER_MS, &waited)) {
            end_session(s_peer_at == -2 ? "the other player disconnected" : "the other player did not arrive");
            return;
        }
        fprintf(stderr, "[NETPLAY] both players at the first step after %d ms: fight (input delay %d steps)\n",
                waited, s_delay);
        fflush(stderr);
    }

    /* This step's local input is played `delay` steps from now, on both games. */
    local_input(s_step, mine);
    lock();
    memcpy(s_inputs[local][(s_step + s_delay) % RING], mine, sizeof mine);
    s_input_step[local][(s_step + s_delay) % RING] = s_step + s_delay;
    unlock();
    send_line("IN %ld %08x %08x %08x %08x %08x %08x", s_step + s_delay,
              mine[0], mine[1], mine[2], mine[3], mine[4], mine[5]);

    /* The first `delay` steps have no inputs on either side: both play them empty. */
    if (s_step <= s_delay) {
        memset(s_current, 0, sizeof s_current);
    } else {
        if (!wait_for_peer(remote_input_here, STALL_MS, &waited)) {
            end_session(s_peer_at == -2 ? "the other player disconnected" : "the other player stopped sending");
            return;
        }
        if (waited > 50)
            fprintf(stderr, "[NETPLAY] waited %d ms for the other player's input at step %ld\n", waited, s_step);
        lock();
        memcpy(s_current[local], s_inputs[local][s_step % RING], sizeof s_current[local]);
        memcpy(s_current[remote], s_inputs[remote][s_step % RING], sizeof s_current[remote]);
        unlock();
    }
    s_current_step = s_step;
    if (s_step % SYNC_EVERY == 0)
        desync_check();
}

/* The game's XInputGetState (sub_0027E075, docs/research/m4b-controller-ports.md) has
 * just filled an XINPUT_STATE. Start opens the pause menu, which stops the fight
 * steps on this game only; the other then waits for inputs that never come. So while
 * a netplay fight runs (from the first step until the match is decided, phase 2 at
 * 0x3B8F30) Start is taken out of the buttons word (+4, bit 0x10). */
#define G_PHASE         0x003B8F30u
#define XINPUT_START    0x0010u

void netplay_filter_pad(uint32_t state)
{
    static int was_down;
    int down;
    if (!s_session_live || !s_current_step || !state || MEM32(G_PHASE) >= 2u) return;
    down = (MEM16(state + 4) & XINPUT_START) != 0;
    if (down && !was_down) {
        fprintf(stderr, "[NETPLAY] Start held back at step %ld: pausing is off in a netplay fight\n", s_step);
        fflush(stderr);
    }
    was_down = down;
    MEM16(state + 4) = (uint16_t)(MEM16(state + 4) & ~XINPUT_START);
}

/* sub_001BAA00(record): a fighter is about to read its pad-table entry. Both games put
 * the same exchanged input there for both fighters. */
void netplay_input(uint32_t actor)
{
    uint32_t slot, i;
    if (!s_session_live || !s_current_step) return;
    if (actor < ACTORS || actor >= ACTORS + 2 * STRIDE || (actor - ACTORS) % STRIDE) return;
    slot = (actor - ACTORS) / STRIDE;
    for (i = 0; i < ENTRY_WORDS; i++)
        MEM32(PAD_TABLE + slot * 0x20u + i * 4) = s_current[slot][i];
}
