# ET:Legacy — working notes

## Debugging client simulation vs. real server fire

The missile trajectory preview (`CG_PredictRiflenadeTrajectory`, `src/cgame/cg_weapons.c`) is a
client-side re-implementation of the server's `G_RunMissile`/`G_BounceMissile`/`G_MissileImpact`
physics. Any divergence between them is a bug in the client copy, and the only reliable way to find
one is to run both against the *same shot* and diff the numbers. Reasoning about the code alone is
not enough — several of the bugs found this way were one-frame ordering differences invisible in a
side-by-side read.

### Setup

Run a **listen server host** (the shooter) and a **second client** (the target dummy), both from
`cmake-build-debug`:

```sh
# host - the process to attach gdb to. It runs BOTH cgame and qagame, so client prediction and
# server physics are debuggable in one inferior.
./etl.x86_64 --title "ETL Host" +set developer 1 +devmap <map>

# target dummy - stand it inside the missiletarget box
./etl.x86_64 --title "ETL Tester" +connect 127.0.0.1:27960 +set developer 0
```

**Always pass `--title` to both.** Without it every ET window matches a substring search for
"ET Legacy" — including the tester, the terminal running the build, and any editor window with the
project open — and there is no way to tell which PID owns the server. With explicit titles:

```sh
for w in $(xdotool search --name "ETL "); do
  echo "win=$w pid=$(xdotool getwindowpid $w) name=$(xdotool getwindowname $w)"
done
```

`+set developer 1` on the host is **required**: without it `cgame_restart` is refused with
"Cgame restart only allowed in development mode", the rebuilt module never loads, and gdb silently
resolves breakpoints against the new source while the process still runs the old code — every
breakpoint lands at a wrong address and simply never fires. A probe run that produces *no* client
output at all is almost always this, not a logic problem.

`g_debugdamage 1` on the host makes the server print who actually took damage, which is the
ground truth to check a predicted damage figure against.

### Driving the game with xdotool

`xdotool ... --window <id>` reaches the game without stealing focus — no `windowactivate` needed,
and not stealing focus matters because the person at the keyboard is usually holding an aim.

The console is the main hazard. It is a *toggle*, so a blind `key grave` is as likely to open it as
to close it, and every keystroke afterwards goes to the wrong place: `+activate` and `+attack`
typed into the console look exactly like a shot that did not happen.

- Console commands **must** be prefixed with `/` or they are sent as chat. `cgame_restart` typed
  without the slash appears in the chat log as a message and silently does nothing.
- `ctrl+u` clears the console input line — use it before typing, since a previous run may have left
  characters there.
- `cgame_restart` **closes the console** when it runs. That makes it a reliable way to get into a
  known state: issue it, and the console is closed afterwards.
- When in doubt, screenshot: `import -window <id> shot.png` and read it. Cheaper than a wasted run.

Firing a riflenade drops the player back to the plain rifle, so a second shot needs `/weapalt`
first — otherwise `+attack` fires a bullet and no missile is ever created.

### The rebuild/reload cycle

```sh
cmake --build . --target cgame   # from cmake-build-debug; CLIENT_SRC is a GLOB, no CMake edit needed
# then, in the host console:
/cgame_restart
```

`cgame_restart` reloads the module, which **resets every cgame static** — including the
`missiletarget` marker. Recover it with the 9-argument form the command prints on every placement:

```
missiletarget 8486.450 -1383.290 -103.880 -18.000 -18.000 -24.000 18.000 18.000 48.000
```

### gdb harness

Attach detached, let breakpoint `commands` blocks print and `continue`, and log to a file:

```sh
nohup gdb -p <host-pid> -x probe.gdb > probe.stdout 2>&1 &
```

This needs `dangerouslyDisableSandbox: true` from the Bash tool.

**Never `pkill -KILL` gdb.** If it is stopped at a breakpoint in a hot path, the trap instructions
stay behind in a process with no debugger attached and the game dies with SIGSEGV. Wait for the log
to stop growing, then `pkill -INT` followed by `pkill -TERM` so gdb detaches cleanly.

`disable` **with no argument disables every breakpoint** — always name the one you mean
(`disable 2`). A self-throttling breakpoint that disables itself must therefore use its own number.

Useful breakpoints (line numbers drift — regenerate them from source with `grep -n` rather than
hardcoding):

| Where | What it tells you |
|---|---|
| `cg_draw.c`, `CG_DrawMissileCamera` after `queryPoint` is set | the predicted explosion point, damage, distance, LOS |
| `g_missile.c`, `weapon_*_fire` | `R-FIRE`: the real launch origin/direction and `level.time` |
| `g_missile.c:668` (`trap_Trace` in `G_RunMissile`) | one line per server frame: `currentOrigin`, target origin, fraction, `surfaceFlags`, `lastSurfaceFlags` |
| `G_MissileImpact` | what was hit, plane normal, whether it takes damage |
| `G_ExplodeMissile` | the real burst point — the number to diff against |
| the four `G_FreeEntity` paths in `G_RunMissile` | mapcoords / up-not-sky / below-world / SURF_NOIMPACT silent destruction |
| `G_RadiusDamage` after `points` is computed | per-client `dist`, `radius`, `points`, `CanDamage` |
| `G_DamageExt` | what damage actually landed |

Throttle the client-side breakpoint (`if ($n % 60) == 1`) — it fires every rendered frame and will
otherwise produce megabytes of identical lines.

### Reading the result

The prediction line **immediately before `R-FIRE`** is the one to compare; everything logged after
it describes a different aim, and the weapon number changes too once the shot switches the player
back to the rifle. A correct prediction matches the real burst to ~0.02 units — these are the same
arithmetic on the same map data, so anything larger is a real divergence, not float noise.

Diff the vectors rather than eyeballing them. A miss that is an exact multiple of one frame's
displacement (compare against the per-frame delta in the early `R-TRACE` lines, 25 ms apart) is an
ordering bug, not a physics bug.

### Things the server does that are easy to get wrong

- One collision trace per **server frame** (25 ms), not per client frame.
- `MISSILE_PRESTEP_TIME` is `-50`: `trTime` is backdated 50 ms relative to the fire frame, while
  `nextthink` is set from the fire frame. Elapsed-since-`trTime` and time-to-think differ by 50 ms.
- `G_RunMissile`'s sky branch calls `G_RunThink` **before** `r.currentOrigin` is brought forward
  (and the "above the sky limit" sub-path never brings it forward at all), the opposite order from
  the in-world path. A fuse expiring up there detonates a whole frame behind the missile.
- The sky-state branch's `return` sits *outside* its `else`, so the re-entry frame runs no
  collision trace at all.
- `tr.startsolid` becomes a fraction-0 hit that falls through to the normal impact path — it must
  not short-circuit into "keep flying".
- `SnapVector` is applied to `trDelta` at launch and after every bounce, and to `trBase` on bounce.
- `VectorNormalize` vs `VectorNormalizeFast` matters: ~0.2% per step compounds into whole units.
- `WP_KAR98`/`WP_CARBINE` have `splashDamage` 0 — riflenade damage comes from their `weapAlts`.
- `G_ExplodeMissile` raises the blast origin 4 units for `WP_DYNAMITE` before radius damage.
