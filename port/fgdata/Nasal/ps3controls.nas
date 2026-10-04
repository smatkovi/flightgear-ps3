# PS3 port: response curve for the right stick (Input/Joysticks/Sony/
# ps3-sixaxis.xml). Squared near the centre, for fine control, but no steeper
# than linear beyond half deflection: plain squared got too strong towards
# full deflection, which now gives 75 %. factor scales (and signs) the result.

var stick = func(prop, factor) {
    var v = cmdarg().getNode("setting").getValue();
    var a = v < 0 ? -v : v;
    var out = a <= 0.5 ? a * a : a - 0.25;
    setprop(prop, (v < 0 ? -out : out) * factor);
}

# Throttle on R2 (dir 1) / L2 (dir -1), called every frame while the trigger
# is held: the harder it is pressed, the faster the throttle moves (from
# about 0.1 to 1.6 of its travel per second); the pressure comes from the
# port in /input/ps3/pressure-r2 and -l2.
var throttle = func(dir) {
    var p = getprop(dir > 0 ? "/input/ps3/pressure-r2" : "/input/ps3/pressure-l2");
    if (p == nil) { p = 1; }
    var rate = 0.002 + 0.025 * p;
    controls.incThrottle(dir * rate, dir * 1.0);
}
