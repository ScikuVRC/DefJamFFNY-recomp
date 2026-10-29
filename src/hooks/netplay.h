#ifndef DEFJAM_NETPLAY_H
#define DEFJAM_NETPLAY_H
/* Netplay sessions: two players agree on a match over TCP, both games start it in
 * lockstep mode (lockstep.h), and the fight begins on both at once
 * (src/hooks/netplay.c, docs/netplay.md). */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETPLAY_DEFAULT_PORT 7777

typedef enum {
    NETPLAY_IDLE,
    NETPLAY_LISTENING,      /* host: waiting for the other player */
    NETPLAY_CONNECTING,     /* join: reaching the host */
    NETPLAY_HANDSHAKE,      /* connected; comparing builds, agreeing the match */
    NETPLAY_READY,          /* match agreed; start the game */
    NETPLAY_FAILED          /* see netplay_status() */
} NetplayState;

typedef struct {
    uint32_t seed;          /* RECOMP_LOCKSTEP_SEED on both */
    int venue;              /* Game.ChooseVenue */
    int fighter[2];         /* Controller.ControllerSetup per slot */
    int local_slot;         /* 0 on the host, 1 on the joiner */
} NetplayMatch;

/* The host's choice of venue (Game.ChooseVenue index) and fighters (the front end's
 * fighter numbers); 0 keeps the default. Sent to the joiner with the match. */
void netplay_choose(int venue, int fighter0, int fighter1);

/* Start a session in the background; poll netplay_state() for progress. */
int netplay_host(uint16_t port);
int netplay_join(const char *address);     /* "host[:port]" */
void netplay_cancel(void);

NetplayState netplay_state(void);
/* A line for the player: what the session is doing, or why it failed. */
void netplay_status(char *out, int size);
/* The agreed match, once the state is NETPLAY_READY. */
int netplay_match(NetplayMatch *out);

/* Called after the launcher, before the game reads its environment. Runs a session
 * from RECOMP_NETPLAY_HOST=<port> / RECOMP_NETPLAY_JOIN=<host:port> when one is set
 * (blocking, for scripted runs), and, once a session is ready, sets lockstep mode,
 * its seed and the front-end commands that start the agreed match. Returns 0 when a
 * session was asked for and failed. */
int netplay_startup(void);

/* From the game: a match is being set up / a fight step is about to run. At the
 * first step each game waits until the other has reached it too; at every step each
 * sends its local input for `delay` steps ahead (RECOMP_NETPLAY_DELAY, default 3)
 * and waits for the other's input for this one. */
void netplay_match_start(void);
void netplay_step(void);
/* sub_001BAA00(record): put the exchanged input in the fighter's pad-table entry. */
void netplay_input(uint32_t actor);
/* After the game's XInputGetState filled `state`: no Start (pause) in a netplay fight. */
void netplay_filter_pad(uint32_t state);

#ifdef __cplusplus
}
#endif
#endif
