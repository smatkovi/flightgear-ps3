# FlightGear 0.9.10 for the PlayStation 3

The open-source flight simulator [FlightGear](https://www.flightgear.org/) in its
2006 version 0.9.10 (with SimGear 0.3.10 and PLIB 1.8.4), ported to the PS3 as
a homebrew application. You fly by **tilting the controller**: the Sixaxis /
DualShock 3 motion sensors drive aileron and elevator.

![C172p on runway 29 at Vienna](docs/loww-runway29.png)
![In the air over San Francisco Bay](docs/airborne.png)

*Screenshots from RPCS3.*

## Status

Works:

- the full simulator: JSBSim, YASim and LaRCsim flight models, 3D cockpits and
  instruments, scenery, sky, clouds, HUD, Nasal scripting, autopilot
- scenery for the Vienna area (from FlightGear's World Scenery 2.12) and the
  San Francisco Bay Area (from the base package); the Cessna 172P starts on
  runway 29 at Vienna (LOWW)
- the PS3 controller, including tilt steering
- 720p, about 60 MB of main memory in flight (of ~213 MB)

Not (yet) there:

- sound: the OpenAL layer is a silent stub
- mipmaps: distant textures shimmer
- mouse and keyboard: the menu bar is hidden because it cannot be used
- the UIUC aircraft (their flight model needs more memory than the PS3 has)

## Installing

You need a PS3 with custom firmware or HEN.

1. Download `FlightGear-0.9.10-PS3.pkg` from the
   [Releases](../../releases) page (252 MB, FlightGear data and scenery included).
2. Install it: copy it to a USB stick and use *Package Manager > Install
   Package Files* in the XMB, or put it into `/dev_hdd0/packages` and install
   it from there (webMAN can do both).
3. Start **FlightGear 0.9.10** from the *Game* column. The splash screen
   appears after a few seconds; loading the scenery takes a while longer.

## Controls

| Input | Action |
| --- | --- |
| tilt left / right | aileron (roll) |
| tilt forward / back | elevator (pitch) |
| **SELECT** | take the current controller attitude as neutral |
| right stick | aileron and elevator, like tilting |
| left stick left / right | rudder |
| R2 / L2 | throttle up / down |
| cross | brakes |
| circle (hold) | starter |
| triangle | landing gear (retractable aircraft) |
| square | next view |
| R1 / L1 | flaps down / up |
| d-pad up / down | elevator trim |
| START | pause |

Hold the controller the way that is comfortable and press SELECT once: tilt is
measured from there. If roll or pitch goes the wrong way for you, flip the sign
of `<factor>` for axis 4 (roll) or axis 5 (pitch) in
`USRDIR/fgdata/Input/Joysticks/Sony/ps3-sixaxis.xml`; `<dead-band>` there sets
how calm the tilt steering is.

## Aircraft, airports and scenery

`/dev_hdd0/game/FGFS00910/USRDIR/fgfs.args` holds FlightGear's command line,
one option per line (`--aircraft=`, `--airport-id=`, `--runway=`,
`--timeofday=`, ...). FlightGear 0.9.10 has no menu for any of this.

Aircraft: all of the 0.9.10 base package except the UIUC ones (their flight
model needs more memory than the PS3 has): `737-300`, `A-10`, `bf109`, `bo105`,
`c172`, `c172p`, `c310`, `c310u3a`, `Citation-Bravo`, `f16`, `Hunter`, `j3cub`,
`p51d`, `pa28-161`, `Rascal`, `T38`, `ufo`, `wrightFlyer1903`. Other aircraft of
the 0.9.x / 1.0 era can be copied to `USRDIR/fgdata/Aircraft`; aircraft for
later FlightGear versions mostly do not load.

Scenery: the Vienna tiles come from World Scenery 2.12 (2013). Its terrain
files use the format version FlightGear 0.9.10 reads, and its land class,
runway and light materials all exist in 0.9.10, so the tiles load unchanged;
[tools/ws2_install.py](tools/ws2_install.py) only drops scenery objects whose
models 0.9.10 lacks (power pylons, VOR/DME, markers). More areas can be added
the same way from the
[World Scenery 2.12 archives](https://mirrors.ibiblio.org/flightgear/ftp/Scenery-v2.12/)
(see `fetch_scenery.sh`), as long as their terrain files are version 6.
The airport and navaid databases are cut down to California and Central
Europe ([tools/regional_db.py](tools/regional_db.py)); the full world
databases also fit, at about 35 MB more memory.

## Logs

`/dev_hdd0/game/FGFS00910/USRDIR` also holds `fgfs.log` (FlightGear's output)
and `ps3gl.log` (one line of memory and draw statistics every 600 frames).
`fetch_logs.sh` copies them from a console running webMAN.

## Building

Requirements: Docker, `curl`, `patch`, Python 3 and, for the package, the host
tools of the PS3 toolchain (`make_self_npdrm`, `sfo.py`, `pkg.py`,
`package_finalize` in `$PS3DEV/bin`, default `~/ps3dev/bin`).

```sh
./fetch_sources.sh      # FlightGear, SimGear, PLIB + port/patches -> src/
./dk make -j8           # build/fgfs.run.elf, build/gltest.run.elf
./fetch_data.sh         # FlightGear base data -> fgdata_x/
./fetch_scenery.sh      # Vienna area from World Scenery 2.12
./pkg.sh --full         # FlightGear-0.9.10-PS3.pkg
```

`./dk` runs the build in the Docker image `markstreet/sm64:ps3` (PSL1GHT with
GCC 7.2). `./pkg.sh` without `--full` builds a small package without the data;
`deploy_ps3.sh` uploads the data to a console over FTP instead.

In RPCS3: `./install_rpcs3.sh` puts the game into RPCS3's virtual hard disk,
`./run_rpcs3.sh fgfs` starts it, `./stop_rpcs3.sh` stops it.

## How it works

- **ps3gl** ([ps3gl/](ps3gl/)): the OpenGL 1.x subset that FlightGear and PLIB
  use, on top of the RSX through PSL1GHT's librsx. The fixed-function pipeline
  (transform, lighting, fog, texture environments) is one Cg vertex program and
  a handful of fragment programs; display lists are compiled into vertex
  buffers in video memory. `gltest/` is its self-test.
- **port** ([port/](port/)): the window and OS layer, the controller as a PLIB
  joystick (with the tilt axes), stubs for OpenAL, serial ports, render
  textures and Nasal threads.
- **patches** ([port/patches/](port/patches/)): the changes to FlightGear,
  SimGear and PLIB, kept small. Notable ones: FlightGear runs on the primary
  thread; the UIUC flight model's 118 MB data block is allocated on demand;
  an undefined-behaviour loop in the Morse code generator that GCC 7 turned
  into an endless loop.
- **build**: `-mminimal-toc` (with a TOC split into groups, ld missed r2
  switches on some calls), and no optimisations that exploit undefined
  behaviour (`-fno-strict-aliasing -fno-aggressive-loop-optimizations
  -fno-delete-null-pointer-checks`).

## License

FlightGear is GPL v2, SimGear and PLIB are LGPL v2, the FlightGear data is
GPL v2. The code in this repository is GPL v2 or later, see [LICENSE](LICENSE).
