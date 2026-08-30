# Control command reference

Commands are line-oriented. A batch written in one `command.txt` update is
committed atomically at the next `xrWaitFrame`.

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

`reset` restores the default Quest-shaped rig, 90 Hz free pacing, inputs,
hazards, focus model, and capture settings. `status` adds a marker to the log.
