# Netplay

Two players, one match, over the internet or a LAN. The design follows Dolphin's
netplay: the two games exchange only what the players do, never game state, and
rely on lockstep mode (docs/lockstep-mode.md) to play the same fight from the same
inputs.

| Step | State |
|---|---|
| 1. Lockstep mode: the same inputs and seed play the same fight on any frame rate | done |
| 2. Desync check: a per-step fingerprint of the fight (`RECOMP_TEST_SYNC`) | done, test runs only |
| 3. Match setup: host/join, compatibility check, agreed match, start barrier | done |
| 4. Lockstep play: exchange inputs every step, input delay, live desync check | next |
| 5. Rollback, spectating, a relay for players behind routers | later |

## Playing (step 3)

In the launcher, **Online Match**. One player hosts (a port, 7777 by default; it may
need opening in the router), the other joins with `host-address:port`. When both are
connected the games check each other and the launcher starts the game, which goes
straight into the match.

Until step 4 the inputs are not exchanged: each game's own pad drives player 1 and
player 2 is the CPU, so the two games show the same fight only as long as both
players do the same. Step 3 is the plumbing that step 4 plays over.

## The session (src/hooks/netplay.c)

One TCP connection, text lines:

1. both: `HELLO <protocol> <version> <program> <data>`. The fingerprints are hashes of
   the running program and of `game/default.xbe`; any difference ends the session
   with the reason, because a different build or dump plays a different fight.
2. host: `MATCH <seed> <venue> <fighter 0> <fighter 1>`; joiner: `READY`; host: `GO`.
3. Both games set lockstep mode with that seed, and the front-end commands that
   start the match from the main menu (`RECOMP_FE_INJECT`, src/hooks/fe_inject.c):
   `Game.StartBattleMode`, `SetMatchType(0)`, `SetMatchRules(0)`,
   `ControllerSetup(0,0,f0,0)`, `ControllerSetup(1,-1,f1,1)`, `ChooseVenue`,
   `StartGame`. No menu is driven by pad presses, so nothing depends on timing.
4. In the game, at the first fight step (after the crowd's first-step work), each
   sends `AT 1` and waits for the other's (up to two minutes), so the fight starts
   on both together whatever their loading times.

Player 2's slot is set up as the CPU: a human slot 2 needs a second pad plugged in,
and match set-up crashed without one (`sub_001BBC10` on slot 2's record). Step 4
replaces each fighter's pad word every step from the exchanged inputs (the write the
step-timed test input already makes), which drives a CPU slot too.

## Scripted sessions

`RECOMP_NETPLAY_HOST=<port>` or `RECOMP_NETPLAY_JOIN=<host:port>` run the same
session headlessly at start-up (`netplay_startup`, after the launcher), and
`RECOMP_NETPLAY_SEED` fixes the host's seed. The harness route `netplay` boots with
no menu presses and ends at the match summary:

```
python scripts/scenario_suite.py run netplay --fixture F --no-audio --step-input default \
    --test-env RECOMP_TEST_SYNC=1 --test-env RECOMP_NETPLAY_HOST=7777 --output A
python scripts/scenario_suite.py run netplay --fixture F --no-audio --step-input default \
    --test-env RECOMP_TEST_SYNC=1 --test-env RECOMP_NETPLAY_JOIN=127.0.0.1:7777 --output B
python scripts/sync_diff.py A/game.log.err B/game.log.err
```

Measured (2026-10-29, two games on one PC): both agreed the seed, started the match
from the main menu, met at the first step (the joiner arrived 3.4 s after the host,
which waited), and with the same step-timed input played the same fight for all
12,187 steps to the same result. The games ended 47 ms apart.

## Known gaps

- Untested between two machines and over the internet.
- No input exchange (step 4), so no real two-player match yet.
- Only One on One at the default venue with fixed fighters; choosing them is for the
  dialog later.
- A rare wild indirect call in the controller library at boot (`0x27EC6E`, once in 69
  runs) and the intermittent start-up stall: the USB emulation, not netplay.
