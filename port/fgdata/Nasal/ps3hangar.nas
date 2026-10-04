# PS3 port: when the aircraft loaded with errors (the port reads them from the
# log once the scenery is there), say so on screen. The hangar lists the same
# note next time. gui.popupTip needs gui.nas to be initialised, which happens
# on the first frame of simulation time, so wait for that (in real time).

var ps3_warning_tip = func {
    var msg = getprop("/sim/ps3/aircraft-warning");
    if (msg == nil or msg == "") { return; }
    if (gui.screenHProp == nil) { settimer(ps3_warning_tip, 1, 1); return; }
    # popupTip() of 0.9.10 takes the tip down after gui.DELAY seconds,
    # whatever time it is given
    var delay = gui.DELAY;
    gui.DELAY = 15;
    gui.popupTip(msg);
    gui.DELAY = delay;
}

setlistener("/sim/ps3/aircraft-warning", func { settimer(ps3_warning_tip, 1, 1); });
