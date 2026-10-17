# ALIGNMENT - designing a sector

The campaign is 8 hand-made sectors, played in order, each ending in a boss arena. A
sector is a text map: `assets/levels/sectorNN.txt` (format: the docstring at the top of
`tools/gen_levels.py`). This page is what a designer needs to know.

## The mech

* Cells are 16x16 px. The screen shows 10 x 8.5 cells; the camera follows the mech both
  ways (the map is 16 rows tall: about two screens).
* Runs 1.75 px/frame. **A** jumps about 2 cells high; a running jump clears a 2-cell gap
  (3 with room above). **Up+A** (or SELECT) flips gravity: the mech falls to the ceiling,
  ~4.5 px/frame. A flip works any time on the ground, and once in the air until it lands
  again. A flip is refused inside a phase block the new gravity would make solid.
* **B** fires the arm cannon forward (3 shots on screen, ~1 shot / 9 frames, 1 damage;
  the power shot does 2). Shots stop at walls. **Up+B** fires straight overhead (with
  gravity up, **Down+B**): a ceiling crawler or a drone above is a target.
* Falling onto an enemy along gravity (from above, or flipping onto a ceiling crawler)
  stomps it: instant kill (3 damage) and a bounce, with the air flip given back.
* Hull: 3 hits (blinks after a hit). Plasma, lasers (when on), crushers and a slamming
  boss are instant death. Three lives, a checkpoint `C` halfway, 25 cells = extra life.

## Terrain and things

| char | what |
| --- | --- |
| `#` | wall |
| `~` | plasma: death on touch |
| `P` / `G` | phase blocks: purple solid while gravity is DOWN, green while gravity is UP |
| `L` | laser gate: on 72 frames, off 56 (all lasers share the rhythm) |
| `o` | energy cell (score; 12-25 a sector) |
| `p s b r` | power shot, shield (soaks a hit), gravity bomb (next flip wrecks every enemy near), repair (+1 hull) |
| `= - \| O * %` | background: girder, pipe, vertical pipe, window, light, grate (never solid) |
| `c` / `k` | crawler on the floor / hanging from the ceiling: patrols up to 4 cells each way, 2 hp |
| `v` | hover drone: bobs from its cell down to the first solid cell, 1 hp |
| `X` | crusher: slams from its cell to the floor (invulnerable; timing obstacle) |
| `< >` / `{ }` | turret on floor / ceiling firing left / right: a bolt along its row every ~2 s, 3 hp |
| `S C E` | start (needs floor), checkpoint, exit gate column |
| `d D B` | arena entry door (open, seals behind you), exit door (opens when the boss dies), boss's top-left cell |

## Hard limits (gen_levels.py and the bot enforce them)

* Width at most 128 columns, exactly 16 rows. At most 24 enemies.
* **Never more than 4 enemies inside any 10-column window** (the screen draws 4).
* The arena: a closed room, entry door `d` column reaching the arena floor, exit door `D`
  column, ceiling row and floor row solid across, at least 8 (best 12) columns inside, no
  enemies inside. The cell left of the entry door on the floor row is the respawn spot
  (empty, floor below). Boss start `B`: SENTINEL / TWIN on the floor at the right
  (`B` = (exit door x - 3, floor row - 2)); PISTON hanging at the ceiling in the middle
  (`B` = (middle, ceiling row + 1)); HIVE floating in the middle (ceiling row + 3).
  Use ceiling row 2 and floor row 13 unless you have a reason.
* `build/msgen sbot` must finish every sector: the physics bot plays the real game code
  with real buttons, never taking a hit. Its path must stay under 1536 moves (`msgen path
  s N | wc -c`): about 100 s of play. If the bot can't do it, a human will hate it.

## Tools

    python3 tools/gen_levels.py                 # check + compile the maps
    make build/msgen && build/msgen sbot        # the bot plays every sector
    build/msgen show s 3                        # the map as the game reads it
    build/msgen replay s 3 $(build/msgen path s 3) | tail   # the bot's run
    make && python3 tools/level_map.py s 3      # build/maps/sector3.png: the real tiles

Look at the PNG. Then look at it again as a player would see it 10 cells at a time.

## What makes a sector good

1. **A signature idea**, introduced where it can't hurt you, then developed, then turned
   on its head near the end (introduce, develop, twist, conclude). Earlier ideas come back
   mixed in.
2. **Use all 16 rows.** Upper and lower routes, rooms taller than a screen, shafts you
   "fall" up by flipping, ceilings that are floors. A sector is a maze of rooms, not a
   corridor.
3. **Gravity is the star.** The best moments are only possible by flipping: crossing a
   plasma floor on the ceiling, flipping onto a ceiling crawler, dropping past a turret's
   row, threading phase blocks.
4. **Rhythm.** A fight, then a breather with cells, then a traversal puzzle, then a
   set-piece. Never three hard things in a row without a breather.
5. **Rewards for curiosity.** Power-ups and cells in alcoves you only reach with a clever
   flip; cells as breadcrumbs that hint where to go.
6. **Fair.** No blind drops into plasma (the camera shows ~4 rows past the mech); new
   hazards first appear somewhere safe; the checkpoint sits before the hardest stretch;
   a repair `r` or shield `s` before the boss.
7. **Readable and handsome.** Decoration sells the place (windows to space, pipe runs,
   lights, grates, girders) but stays sparse and never looks like a platform. Leave big
   solid masses hollow-looking with rooms, not 6-row slabs of wall.
