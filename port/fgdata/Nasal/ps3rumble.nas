# PS3 port: controller rumble. The port hands /input/ps3/rumble-large (0..1)
# and /input/ps3/rumble-small (on/off) to the controller every frame:
# - a jolt on touchdown, the harder the higher the sink rate,
# - a light shake while rolling over the ground, growing with speed,
# - the small motor buzzes while the stall warning sounds,
# - a hard shake after a crash.
# Set /input/ps3/rumble to 0 to switch it off.

var ps3_jolt = 0;
var ps3_last_wow = 1;

var ps3_on_ground = func {
    var gear = props.globals.getNode("/gear");
    if (gear == nil) { return 0; }
    var wheels = gear.getChildren("gear");
    for (var i = 0; i < size(wheels); i += 1) {
        var w = wheels[i].getNode("wow");
        if (w != nil and w.getBoolValue()) { return 1; }
    }
    return 0;
}

var ps3_rumble = func {
    var large = 0;
    var small = 0;
    if (getprop("/input/ps3/rumble") != 0 and !getprop("/sim/freeze/master")) {
        var wow = ps3_on_ground();
        if (wow and !ps3_last_wow) {
            var vs = getprop("/velocities/vertical-speed-fps");
            if (vs == nil) { vs = 0; }
            ps3_jolt = -vs / 8;          # 8 ft/s sink rate: full strength
            if (ps3_jolt < 0.3) { ps3_jolt = 0.3; }
            if (ps3_jolt > 1) { ps3_jolt = 1; }
        }
        ps3_last_wow = wow;
        if (ps3_jolt > 0) {
            large = ps3_jolt;
            ps3_jolt -= 0.2;             # fades in a quarter second
        }
        if (wow) {
            var gs = getprop("/velocities/groundspeed-kt");
            if (gs != nil and gs > 8) {
                var roll = gs / 300;
                if (roll > 0.2) { roll = 0.2; }
                if (roll > large) { large = roll; }
            }
        }
        if (getprop("/sim/alarms/stall-warning")) { small = 1; }
        if (getprop("/sim/crashed")) { large = 1; }
    }
    setprop("/input/ps3/rumble-large", large);
    setprop("/input/ps3/rumble-small", small);
    settimer(ps3_rumble, 0.05, 1);
}

settimer(ps3_rumble, 1, 1);
