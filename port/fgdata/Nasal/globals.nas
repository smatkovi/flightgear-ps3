##
# Returns true if the first object is an instance of the second
# (class) object.  Example: isa(someObject, props.Node)
#
isa = func {
    obj = arg[0]; class = arg[1];
    if(!contains(obj, "parents")) { return 0; }
    foreach(c; obj.parents) {
        if(c == class)     { return 1; }
        elsif(isa(obj, c)) { return 1; }
    }
    return 0;
}

##
# Invokes a FlightGear command specified by the first argument.  The
# second argument specifies the property tree to be passed to the
# command as its argument.  It may be either a props.Node object or a
# string, in which case it specifies a path in the global property
# tree.
#
fgcommand = func {
    if(isa(arg[1], props.Node)) { arg[1] = arg[1]._g }
    _fgcommand(arg[0], arg[1]);
}

##
# Returns the SGPropertyNode argument to the currently executing
# function. Wrapper for the internal _cmdarg function that retrieves
# the ghost handlet to the argument and wraps it in a
# props.Node object.
#
cmdarg = func { props.wrapNode(_cmdarg()) }

##
# Utility.  Does what it you think it does.
#
abs = func { if(arg[0] < 0) { -arg[0] } else { arg[0] } }

##
# Convenience wrapper for the _interpolate function.  Takes a
# single string or props.Node object in arg[0] indicating a target
# property, and a variable-length list of time/value pairs.  Example:
#
#  interpolate("/animations/radar/angle",
#              180, 1, 360, 1, 0, 0,
#              180, 1, 360, 1, 0, 0,
#              180, 1, 360, 1, 0, 0,
#              180, 1, 360, 1, 0, 0,
#              180, 1, 360, 1, 0, 0,
#              180, 1, 360, 1, 0, 0,
#              180, 1, 360, 1, 0, 0,
#              180, 1, 360, 1, 0, 0);
#
# This will swing the "radar dish" smoothly through 8 revolutions over
# 16 seconds.  Note the use of zero-time interpolation between 360 and
# 0 to wrap the interpolated value properly.
#
interpolate = func {
    if(isa(arg[0], props.Node)) { arg[0] = arg[0]._g; }
    elsif(typeof(arg[0]) != "scalar") { return; }
    _interpolate(arg[0], size(arg) == 1 ? [] : subvec(arg, 1));
}


##
# Convenience wrapper for the _setlistener function. Takes a
# single string or props.Node object in arg[0] indicating the
# listened to property, a function in arg[1], and an optional
# bool in arg[2], which triggers the function initially if true.
#
# PS3 port: functions with parameters, as aircraft written for
# FlightGear 1.9 use them (setlistener(p, func(n) { ... })), are
# called as there: with the changed node, the listened-to node (both
# props.Node), the mode (0) and whether a child changed (0).
# Functions without parameters are called without arguments, as
# before (some look at arg, see gui.fpsDisplay).
#
setlistener = func {
    var node = arg[0];
    var fn = arg[1];
    if(isa(node, props.Node)) { node = node._g; }
    elsif(typeof(node) != "scalar") { return; }
    if(_numparams(fn) > 0) {
        var f = fn;
        var listened = typeof(node) == "scalar" ? props.globals.getNode(node, 1)
                                                : props.wrapNode(node);
        fn = func { f(cmdarg(), listened, 0, 0) };
    }
    _setlistener(node, fn, size(arg) > 2 ? arg[2] : 0);
}


##
# Returns true if the symbol name is defined in the caller, or the
# caller's lexical namespace.  (i.e. defined("varname") tells you if
# you can use varname in an expression without a undefined symbol
# error.
#
defined = func(sym) {
    var fn = 1;
    while((var frame = caller(fn)) != nil) {
        if(contains(frame[0], sym)) { return 1; }
        fn += 1;
    }
    return 0;
}


##
# Print log messages in appropriate --log-level.
# Usage: printlog("warn", "...");
# The underscore hash prevents helper functions/variables from
# needlessly polluting the global namespace.
#
__ = {};
__.dbg_types = { none:0, bulk:1, debug:2, info:3, warn:4, alert:5 };
__.log_level = __.dbg_types[getprop("/sim/logging/priority")];
printlog = func(level, args...) {
    if(__.dbg_types[level] >= __.log_level) { call(print, args); }
}


