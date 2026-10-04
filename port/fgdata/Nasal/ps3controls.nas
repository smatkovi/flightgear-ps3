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
