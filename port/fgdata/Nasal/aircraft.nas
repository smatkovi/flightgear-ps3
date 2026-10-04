# These classes provide basic functions for use in aircraft specific
# Nasal context. Note that even if a class is called "door" or "light"
# this doesn't mean that it can't be used for other purposes.
#
# Class instances don't have to be assigned to variables. They do also
# work if they remain anonymous. It's even a good idea to keep them
# anonymous if you don't need further access to their members. On the
# other hand, you can assign the class and apply setters at the same time:
#
#   aircraft.light.new("sim/model/foo/beacon");    # anonymous
#   strobe = aircraft.light.new("sim/model/foo/strobe").cont().switch(1);
#
#
# Classes do create properties, but they don't usually overwrite the contents
# of an existing property. This makes it possible to preset them in
# a *-set.xml file or on the command line. For example:
#
#   $ fgfs --aircraft=bo105 --prop:/controls/doors/door[0]/position-norm=1
#
#
# Wherever a property argument can be given, this can either be a path,
# or a node (i.e. property node hash). In return, the property node can
# always be accessed directly as member "node", and turned into a path
# string with node.getPath():
#
#   beacon = aircraft.light.new("sim/model/foo/beacon");
#   print(beacon.node.getPath());
#
#   strobe_node = props.globals.getNode("sim/model/foo/strobe", 1);
#   strobe = aircraft.light.new(strobe_node, 0.05, 1.0);
#
#
# The classes implement only commonly used features, but are easy to
# extend, as all class members are accessible from outside. For example:
#
#   # add custom property to door node:
#   frontdoor.node.getNode("name", 1).setValue("front door");
#
#   # add method to class instance (or base class -> aircraft.door.print)
#   frontdoor.print = func { print(me.position.getValue()) };
#
#


# helper functions
# ==============================================================================

# creates (if necessary) and returns a property node from arg[0],
# which can be a property node already, or a property path
#
makeNode = func {
	if (isa(arg[0], props.Node)) {
		return arg[0];
	} else {
		return props.globals.getNode(arg[0], 1);
	}
}


# returns arg[1]-th optional argument of vector arg[0] or default value arg[2]
#
optarg = func {
	if (size(arg[0]) > arg[1] and arg[0][arg[1]] != nil) {
		arg[0][arg[1]];
	} else {
		arg[2];
	}
}



# door
# ==============================================================================
# class for objects moving at constant speed, with the ability to
# reverse moving direction at any point. Appropriate for doors, canopies, etc.
#
# SYNOPSIS:
#	door.new(<property>, <swingtime> [, <startpos>]);
#
#	property   ... door node: property path or node
#	swingtime  ... time in seconds for full movement (0 -> 1)
#	startpos   ... initial position      (default: 0)
#
# PROPERTIES:
#	./position-norm   (double)     (default: <startpos>)
#	./enabled         (bool)       (default: 1)
#
# EXAMPLE:
#	canopy = aircraft.door.new("sim/model/foo/canopy", 5);
#	canopy.open();
#
door = {
	new : func {
		m = { parents : [door] };
		m.node = makeNode(arg[0]);
		m.swingtime = arg[1];
		m.positionN = m.node.getNode("position-norm", 1);
		m.enabledN = m.node.getNode("enabled", 1);
		if (m.enabledN.getValue() == nil) {
			m.enabledN.setBoolValue(1);
		}
		pos = optarg(arg, 2, 0);
		if (m.positionN.getValue() == nil) {
			m.positionN.setDoubleValue(pos);
		}
		m.target = pos < 0.5;
		return m;
	},
	# door.enable(bool)    ->  set ./enabled
	enable  : func { me.enabledN.setBoolValue(arg[0]); me },

	# door.setpos(double)  ->  set ./position-norm without movement
	setpos  : func { me.positionN.setValue(arg[0]); me.target = arg[0] < 0.5; me },

	# double door.getpos() ->  return current position as double
	getpos  : func { me.positionN.getValue() },

	# door.close()         ->  move to closed state
	close   : func { me.move(me.target = 0) },

	# door.open()          ->  move to open state
	open    : func { me.move(me.target = 1) },

	# door.toggle()        ->  move to opposite end position
	toggle  : func { me.move(me.target) },

	# door.stop()          ->  stop movement
	stop    : func { interpolate(me.positionN) },

	# door.move(double)    ->  move to arbitrary position
	move    : func {
		time = abs(me.getpos() - arg[0]) * me.swingtime;
		interpolate(me.positionN, arg[0], time);
		me.target = !me.target;
	},
};



# light
# ==============================================================================
# class for generation of pulsing values. Appropriate for controlling
# beacons, strobes, etc.
#
# SYNOPSIS:
#	light.new(<property> [, <ontime> [, <offtime> [, <switch>]]]);
#	light.new(<property>, <pattern> [, <switch>]);             (FlightGear 1.9)
#	light.new(<property>, <stretch>, <pattern> [, <switch>]);  (FlightGear 1.9)
#
#	property   ... light node: property path or node
#	ontime     ... time that the light is on when blinking  (default: 0.5 [s])
#	offtime    ... time that the light is off when blinking (default: <ontime>)
#	pattern    ... vector of alternating on and off times [s]
#	stretch    ... factor for all times of <pattern>
#	switch     ... property path or node to use as switch   (default: ./enabled)
#                      instead of ./enabled
#
# PROPERTIES:
#	./state           (bool)   (default: 0)
#	./enabled         (bool)   (default: 0) except if <switch> given)
#
# EXAMPLES:
#	aircraft.light.new("sim/model/foo/beacon", 0.4);    # anonymous light
#	strobe = aircraft.light.new("sim/model/foo/strobe", 0.05, 1.0,
#	                "controls/lighting/strobe");
#	strobe.switch(1);
#
light = {
	new : func {
		var m = { parents : [light] };
		var sw = nil;
		m.node = makeNode(arg[0]);
		# PS3 port: the FlightGear 1.9 forms, used by aircraft from the 1.x archive
		if (size(arg) > 2 and typeof(arg[2]) == "vector") {
			m.pattern = [];
			for (var i = 0; i < size(arg[2]); i += 1) {
				append(m.pattern, arg[2][i] * arg[1]);
			}
			sw = optarg(arg, 3, nil);
		} elsif (size(arg) > 1 and typeof(arg[1]) == "vector") {
			m.pattern = arg[1];
			sw = optarg(arg, 2, nil);
		} else {
			var on = optarg(arg, 1, 0.5);
			m.pattern = [on, optarg(arg, 2, on)];
			sw = optarg(arg, 3, nil);
		}
		if (size(m.pattern) == 0) {
			m.pattern = [0.5];
		}
		if (sw != nil) {
			m.switchN = makeNode(sw);
		} else {
			m.switchN = m.node.getNode("enabled", 1);
		}
		if (m.switchN.getValue() == nil) {
			m.switchN.setBoolValue(0);
		}
		m.stateN = m.node.getNode("state", 1);
		if (m.stateN.getValue() == nil) {
			m.stateN.setBoolValue(0);
		}
		m.interval = 0.5;  # check interval for non blinking (off/on) lights;
		                   # 0.5 is performance friendly, but makes lights
		                   # react a bit slow to switch events
		m.continuous = 0;
		m.index = 0;
		m.loopid = 0;
		m._loop_(0);
		return m;
	},
	# light.switch(bool)   ->  set light switch (also affects other lights
	#                          that use the same switch)
	switch  : func { me.switchN.setBoolValue(arg[0]); me },

	# light.toggle()       ->  toggle light switch
	toggle  : func { me.switchN.setBoolValue(!me.switchN.getValue()); me },

	# light.cont()         ->  continuous light
	cont    : func { me.continuous = 1; me },

	# light.blink()        ->  blinking light  (default)
	blink   : func { me.continuous = 0; me },

	# light.del()          ->  stop the light for good
	del     : func { me.loopid += 1; me },

	_loop_  : func(id) {
		if (id != me.loopid) {
			return;
		}
		var delay = me.interval;
		if (!me.switchN.getValue()) {
			me.stateN.setBoolValue(0);
			me.index = 0;
		} elsif (me.continuous) {
			me.stateN.setBoolValue(1);
		} else {
			# even entries of the pattern are on times, odd ones off times
			me.stateN.setBoolValue(me.index == 2 * int(me.index / 2));
			delay = me.pattern[me.index];
			me.index += 1;
			if (me.index >= size(me.pattern)) {
				me.index = 0;
			}
		}
		settimer(func { me._loop_(id) }, delay);
	},
};



# PS3 port: more classes of FlightGear 1.9 that aircraft from the 1.x archive
# use, as far as 0.9.10 can provide them.
# ==============================================================================

# lowpass
# ==============================================================================
# exponential low-pass filter
#
#	var lp = aircraft.lowpass.new(<coefficient>);   # time constant [s]
#	lp.filter(<value>)   ->  push a new value, returns the filtered value
#	lp.get()             ->  filtered value
#	lp.set(<value>)      ->  set the filtered value
#
lowpass = {
	new : func(coeff) {
		var m = { parents : [lowpass] };
		m.coeff = coeff >= 0 ? coeff : 0;
		m.value = nil;
		return m;
	},
	filter : func(v) {
		if (me.value == nil) {
			me.value = v;
		} else {
			var dt = getprop("/sim/time/delta-sec");
			if (dt == nil) {
				dt = 0;
			}
			var c = (me.coeff + dt) > 0 ? dt / (me.coeff + dt) : 1;
			me.value = v * c + me.value * (1 - c);
		}
		return me.value;
	},
	get : func { me.value },
	set : func(v) { me.value = v },
};



# timer
# ==============================================================================
# adds up the simulation time in a property while it runs
#
#	var t = aircraft.timer.new(<property> [, <resolution> [, <save>]]);
#	t.start();  t.stop();  t.reset();
#
timer = {
	new : func {
		var m = { parents : [timer] };
		m.node = makeNode(arg[0]);
		m.interval = optarg(arg, 1, 1);
		if (m.node.getValue() == nil) {
			m.node.setDoubleValue(0);
		}
		m.systimeN = props.globals.getNode("/sim/time/elapsed-sec", 1);
		m.last = 0;
		m.loopid = 0;
		m.running = 0;
		return m;
	},
	start : func {
		if (!me.running) {
			me.last = me.systimeN.getValue();
			me.running = 1;
			me.loopid += 1;
			me._loop_(me.loopid);
		}
		me;
	},
	stop : func {
		if (me.running) {
			me._apply_();
			me.loopid += 1;
		}
		me.running = 0;
		me;
	},
	reset : func {
		me.node.setDoubleValue(0);
		me.last = me.systimeN.getValue();
		me;
	},
	_apply_ : func {
		var now = me.systimeN.getValue();
		me.node.setDoubleValue(me.node.getValue() + now - me.last);
		me.last = now;
	},
	_loop_ : func(id) {
		if (id != me.loopid) {
			return;
		}
		me._apply_();
		if (me.interval != nil and me.interval > 0) {
			settimer(func { me._loop_(id) }, me.interval);
		}
	},
};



# livery, livery_update, data
# ==============================================================================
# 0.9.10 can neither swap model textures at run time nor keep properties
# between sessions, so these accept the calls and do nothing: the aircraft
# keeps its default livery.
#
_nodialog = { open : func {}, close : func {}, toggle : func {} };

livery = {
	dialog : _nodialog,
	init : func {},
	select : func {},
	next : func {},
	previous : func {},
};

livery_update = {
	new : func { return { parents : [livery_update] }; },
	update : func {},
	stop : func {},
};

data = {
	catalog : [],
	add : func {},
	remove : func {},
	load : func {},
	save : func {},
};

