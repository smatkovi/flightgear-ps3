# PS3 port: write the flight controls and the joystick axes to the log every
# two seconds, to check the controller bindings without a property browser.
# Set /sim/ps3/debug-log to false to silence it.

ps3val = func(p) {
    var x = getprop(p);
    if (x == nil) { return 0; }
    return x;
}

ps3dump = func {
    if (getprop("/sim/ps3/debug-log")) {
        print(sprintf("ps3dbg: thr=%.2f ail=%.2f elev=%.2f rud=%.2f rpm=%.0f n1=%.0f kias=%.0f alt=%.0f",
                      ps3val("/controls/engines/engine/throttle"), ps3val("/controls/flight/aileron"),
                      ps3val("/controls/flight/elevator"), ps3val("/controls/flight/rudder"),
                      ps3val("/engines/engine/rpm"), ps3val("/engines/engine/n1"), ps3val("/velocities/airspeed-kt"),
                      ps3val("/position/altitude-ft")));
    }
    settimer(ps3dump, 2);
}

ps3start = func {
    if (getprop("/sim/ps3/debug-log") == nil) { setprop("/sim/ps3/debug-log", 1); }
    ps3dump();
}

settimer(ps3start, 5);
