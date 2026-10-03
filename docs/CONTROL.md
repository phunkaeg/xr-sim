# Control command reference

Commands are line-oriented. A batch written in one `command.txt` update is
committed atomically at the next `xrWaitFrame`.

## Writing command.txt

Write each batch with one write: either write `command.txt.tmp` and rename it
over `command.txt`, or truncate `command.txt` and write the batch in a single
call. The simulator notices a batch by the file's last-write time, and marks
that time seen only after it has read at least one line. A batch it cannot open
yet (another process holds the file) or finds empty mid-rewrite is retried on
the next poll, and `xrsim.log` records the retry.

Wait for `cmdSeq` in `state.json` to advance before writing the next batch. A
batch that is replaced before it is read is never applied, and a rewrite within
the same file-system clock tick (about 16 ms) can keep the old write time.

## Head and hands

```text
head pos <x> <y> <z>
head rot <yaw> <pitch> <roll>
head pose <x> <y> <z> <yaw> <pitch> <roll>
head move <dx> <dy> <dz>
head movelocal <forward> <right> <up>
head turn <yaw> <pitch> <roll>
head height <metres>
head valid on|off
head to <x> <y> <z> <yaw> <pitch> <roll> <ms>
head orbit <degrees-per-second> <ms>

hand l|r grip|aim pos <x> <y> <z>
hand l|r grip|aim rot <yaw> <pitch> <roll>
hand l|r grip|aim pose <x> <y> <z> <yaw> <pitch> <roll>
hand l|r follow on|off
hand l|r offset <forward> <right> <up>
hand l|r aimtrim <pitch> <yaw>
hand l|r point <yaw> <pitch>
hand l|r valid on|off
hands reset
```

Positions are metres. Angles are degrees.

## Inputs

```text
btn a|b|x|y|menu down|up|press [ms]
click l|r down|up|press [ms]
thumbrest l|r on|off
trigger l|r <0..1>
trigger l|r pull [ms]
grip l|r <0..1>
grip l|r squeeze [ms]
stick l|r <x> <y>
stick l|r center
input clear
profile touch|simple
```

## Timing and session behavior

```text
pace free [hz]
pace step
pace turbo
step <frame-count>
step on|off
refresh <hz>
idle on <ms>|off|max <ms>
state ready|synchronized|visible|focused|stopping|exiting|lost|idle
focus lose [ms]
focus regain
focus policy vdxr|vdxr-layers|permissive
focus frames <count>
focus norender on|off
focus throttle <ms>
```

## Optics, capture, and hazards

```text
ipd <millimetres>
worldscale <factor>
fov quest3
fov profile
fov <horizontal-half-angle> <vertical-half-angle> [inner-angle]
fov eye l|r <left> <right> <up> <down>
recenter

capture next [count]
capture every <n>|off
capture size <width> <height>
capture tag <name>
shot [name]
compose oncapture|always

hazard nosystem on|off
hazard waitfail|beginfail|endfail <count>
hazard swapchainfail|attachfail on|off
hazard clear
instanceloss
```

`reset` restores the selected boot-profile rig, 90 Hz free pacing, inputs,
hazards, focus model, and capture settings. `fov profile` restores only the
profile FOV; `fov quest3` explicitly selects the built-in measured Quest 3
shape. `status` adds a marker to the log.
