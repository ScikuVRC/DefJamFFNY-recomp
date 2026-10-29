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
| 4. Lockstep play: exchange inputs every step, input delay, live desync check | done; untested with a real pad and between machines |
| 5. Rollback, spectating, a relay for players behind routers | later |

## Playing

In the launcher, **Online Match**. One player hosts (a port, 7777 by default; it may
need opening in the router), the other joins with `host-address:port`. When both are
connected the games check each other and the launcher starts the game, which goes
straight into the match. The host is player 1, the joiner player 2; each plays with
their first pad (or keyboard). The host chooses the venue (Foundation, Terrordome,
Hunt's Point, The Pit) and, under "Fighters (advanced)", the fighters by the front
end's numbers (56 and 55 by default; their names are not mapped yet).

Start is held back during a netplay fight (pausing would stop one game only). The
menus after the match are each player's own; the session ends with the match.

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

## Inputs (step 4)

Lockstep with an input delay, as in Dolphin. At every fight step each game:

1. reads its player's input: the first six words of slot 0's pad-table entry
   (`0x3B8F48`; buttons, the change mask and both sticks, which is everything
   `sub_001BAA00` reads), which the local pad fills on both machines;
2. sends it for `delay` steps ahead (`IN <step> <6 words>`; `RECOMP_NETPLAY_DELAY`,
   default 3 steps, 50 ms) and keeps it for that step itself;
3. waits until it has the other player's input for this step (up to 10 s, then the
   match goes on locally), and puts both players' inputs in both fighters' entries
   just before each reads its own (the write the step-timed test input makes; it
   drives player 2's CPU slot as well).

The first `delay` steps have no inputs on either side. Every 60 steps both games send
`SYNC <step> <hash>` of the fighters' state, position and health, and each compares
the other's with its own; a difference is logged as `[NETPLAY] DESYNC`.

In a test run with step-timed input (`--step-input`), the script's buttons for slot 0
are the local player's input instead of the pad.

## Scripted sessions

`RECOMP_NETPLAY_HOST=<port>` or `RECOMP_NETPLAY_JOIN=<host:port>` run the same
session headlessly at start-up (`netplay_startup`, after the launcher), and
`RECOMP_NETPLAY_SEED` fixes the host's seed, `RECOMP_NETPLAY_VENUE` (a ChooseVenue
index) and `RECOMP_NETPLAY_FIGHTERS` (`f0,f1`) the host's match. The harness route `netplay` boots with
no menu presses and ends at the match summary:

```
python scripts/scenario_suite.py run netplay --fixture F --no-audio --step-input default \
    --test-env RECOMP_TEST_SYNC=1 --test-env RECOMP_NETPLAY_HOST=7777 --output A
python scripts/scenario_suite.py run netplay --fixture F --no-audio --step-input default \
    --test-env RECOMP_TEST_SYNC=1 --test-env RECOMP_NETPLAY_JOIN=127.0.0.1:7777 --output B
python scripts/sync_diff.py A/game.log.err B/game.log.err
```

Measured (2026-10-29, two games on one PC): both agreed the seed, started the match
from the main menu and met at the first step (the second to load waited 1.4 to 3.4 s
for the first). With different step-timed inputs (host: right and attacks; joiner:
left and button 0x20) both games played the same 8-minute fight, all 28,684 steps, to
the same result; player 2 followed the joiner's input on both, not the CPU's. The
desync check agreed at all 48 points, no step waited 50 ms for an input, and the
games ended 31 ms apart.

With the live pad instead (the emulated USB pad, the path a real pad takes; harness
routes `netplay-pad` and `netplay-pad-left`, presses timed by the host's clock), the
two games played the same 12-minute fight, all 43,280 steps, to the same result, with
the desync check agreeing at all 73 points and no input stall: the local input read
from the pad-table entry is the pad's, not an echo. A session the host set to the
Terrordome (`RECOMP_NETPLAY_VENUE=6`) started there on both and stayed in sync.

## Known gaps

- Untested between two machines, over the internet, and with a physical pad (the
  emulated USB pad it was tested with is the same path).
- Pausing: in a netplay fight Start is taken out of what the game's XInputGetState
  (`sub_0027E075`) returns, from the first step until the match is decided, and
  logged as `Start held back`. Unproven: in the matches the session starts, a Start
  press on the live pad (harness route `netplay-pause`, and the same match without
  netplay) opened no pause menu at all, so the filter never had anything to hold
  back. The steps that did stop in an earlier probe were a first-time tip, which
  lockstep mode turns off.
- Player 2's slot is still the CPU's in the game's own set-up (its name on screen,
  its AI still running under the input).
- TCP: a lost packet stalls both games until it is resent. UDP and rollback later.
- Only One on One at the default venue with fixed fighters; choosing them is for the
  dialog later.
- The intermittent start-up stall (stuck at "press start": the USB host controller's
  first deferred routine ran seconds late and the driver gave up on the pad) is fixed
  in the toolkit: the timer thread, pinned to the guest core, went unscheduled for
  3.1 s in about one start in fifteen; it now only prefers that core. 0 stalls in 65
  starts against 4 in 55. The fix is commit `6cc6238` on the local toolkit branch
  `defjam/timer-ideal-core` in `tools/xboxrecomp`, not published: the parent's gitlink
  still points at the published toolkit until that commit is on a fork the project
  can fetch.
- A rare wild indirect call in the controller library at boot (`0x27EC6E`, once in 69
  runs), skipped without harm.
