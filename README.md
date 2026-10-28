# peeble-apps

A family of small Pebble apps and a watchface, all built around
one idea: big, calm, glanceable information in the LECO 1976
typeface. Author: Jota.

| App | What it does | Version |
| --- | --- | --- |
| [_weather](_weather/) | Min/max temperature and precipitation for the day. Nothing else. | 2.7.20 |
| [_battery](_battery/) | Battery percentage as days remaining, learned from the watch's own drain. | 2.8.11 |
| [_countdown](_countdown/) | A 45-minute countdown timer (adjustable), with a wakeup at zero. | 1.5.3 |
| [_life](_life/) | Magic: The Gathering life counter, 20 to 0. | 1.10.2 |
| [_watchface](_watchface/) | Watchface forked from Lean: huge tilted time, day/night aware, font-size setting. | 1.4.19 |

## Shared code

The whole family shares one header, [`shared/family.h`](shared/family.h):
the tunable parameters (wobble delay/length/amp, idle timeout), the
appear-wobble engine, the day/night engine, the idle exit, and the
common math helpers — each defined once. Apps override a default by
#defining it before the include (e.g. _weather's longer, wider wobble).

## Installing

Download the `.pbw` from the matching
[Release](../../releases), open it with the Pebble app on your
phone, and choose Add/Install.

## Building

Each app builds with the Pebble SDK (4.33):

```
cd _weather   # or any other app
pebble build
```

## Credits

The LECO 1976 typeface and the original watchface design come
from **Lean** by Kieselleger (Axel Kark) —
https://github.com/ProggroP/Lean — used under its MIT license;
see `_watchface/LICENSE`. `_watchface` is a fork of Lean 1.1.0;
the four apps are original.
