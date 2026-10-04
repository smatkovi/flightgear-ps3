/* PS3 controller state shared by the joystick driver and the window layer. */
#include <stdio.h>
#include <string.h>
#include <io/pad.h>
#include "ps3pad.h"

/* The Sixaxis accelerometers read 512 when level and move about 113 counts per g. */
#define ACC_LEVEL   512.0f
#define ACC_RANGE   87.0f      /* sin(50 deg) * 113: full deflection at 50 degrees of tilt */
#define ACC_SMOOTH  0.25f      /* low-pass per frame against sensor noise */

static ps3pad_state st;
static int inited, configured;
static unsigned long polls, with_data, with_sensors;
static int rc_port, rc_press, rc_sensor, last_len;
static unsigned raw_x, raw_y, raw_z, raw_g;
/* Tilt is measured from the attitude the controller had when Select was last
   pressed, so it can be held at whatever angle is comfortable. */
static float acc_x = ACC_LEVEL, acc_z = ACC_LEVEL, zero_x = ACC_LEVEL, zero_z = ACC_LEVEL;
static int acc_valid, select_was_down;

void ps3pad_init(void)
{
    if (inited) return;
    ioPadInit(7);
    inited = 1;
}

static float stick(unsigned v) { return ((float)v - 127.5f) / 127.5f; }

static float tilt(float v, float zero)
{
    float f = (v - zero) / ACC_RANGE;
    return f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
}

void ps3pad_poll(void)
{
    padInfo info;
    padData d;

    ps3pad_init();
    if (ioPadGetInfo(&info) != 0 || !info.status[0]) {
        st.connected = 0;
        configured = 0;
        return;
    }
    /* Pressure and motion sensor data have to be switched on for the port.
       Done with the port setting and the older per-mode calls, and checked
       again every two seconds: the console may not take it right after the
       controller is connected. (RPCS3 delivers sensor data regardless.) */
    if (!configured || (++polls % 120 == 0 && ioPadInfoSensorMode(0) != 1)) {
        rc_press = ioPadSetPressMode(0, 1);
        rc_sensor = ioPadSetSensorMode(0, 1);
        rc_port = ioPadSetPortSetting(0, PAD_SETTINGS_PRESS_ON | PAD_SETTINGS_SENSOR_ON);
        configured = 1;
    }
    st.connected = 1;
    /* ioPadGetData only returns data when something changed */
    memset(&d, 0, sizeof d);
    if (ioPadGetData(0, &d) != 0 || d.len <= 0) return;
    with_data++;
    last_len = d.len;

    st.axis[PS3PAD_LX] = stick(d.ANA_L_H);
    st.axis[PS3PAD_LY] = stick(d.ANA_L_V);
    st.axis[PS3PAD_RX] = stick(d.ANA_R_H);
    st.axis[PS3PAD_RY] = stick(d.ANA_R_V);
    if (d.len >= PAD_BUTTON_OFFSET_SENSOR_G + 1) {      /* the sensor values are there */
        with_sensors++;
        raw_x = d.SENSOR_X;
        raw_y = d.SENSOR_Y;
        raw_z = d.SENSOR_Z;
        raw_g = d.SENSOR_G;
        if (!acc_valid) {
            acc_x = d.SENSOR_X;
            acc_z = d.SENSOR_Z;
            acc_valid = 1;
        } else {
            acc_x += ACC_SMOOTH * ((float)d.SENSOR_X - acc_x);
            acc_z += ACC_SMOOTH * ((float)d.SENSOR_Z - acc_z);
        }
        st.axis[PS3PAD_GYRO] = ((float)d.SENSOR_G - 512.0f) / 512.0f;
    }
    if (d.BTN_SELECT && !select_was_down) {     /* recentre the tilt */
        zero_x = acc_x;
        zero_z = acc_z;
    }
    select_was_down = d.BTN_SELECT;
    if (acc_valid) {
        st.axis[PS3PAD_TILT_ROLL] = tilt(acc_x, zero_x);
        st.axis[PS3PAD_TILT_PITCH] = tilt(acc_z, zero_z);
    }
    if (d.len >= PAD_BUTTON_OFFSET_PRESS_R2 + 1) {
        st.axis[PS3PAD_L2] = d.PRE_L2 / 255.0f;
        st.axis[PS3PAD_R2] = d.PRE_R2 / 255.0f;
    }

    st.buttons = (d.BTN_CROSS << PS3PAD_CROSS) | (d.BTN_CIRCLE << PS3PAD_CIRCLE)
               | (d.BTN_SQUARE << PS3PAD_SQUARE) | (d.BTN_TRIANGLE << PS3PAD_TRIANGLE)
               | (d.BTN_L1 << PS3PAD_BL1) | (d.BTN_R1 << PS3PAD_BR1)
               | (d.BTN_L2 << PS3PAD_BL2) | (d.BTN_R2 << PS3PAD_BR2)
               | (d.BTN_SELECT << PS3PAD_SELECT) | (d.BTN_START << PS3PAD_START)
               | (d.BTN_L3 << PS3PAD_L3) | (d.BTN_R3 << PS3PAD_R3)
               | (d.BTN_UP << PS3PAD_UP) | (d.BTN_DOWN << PS3PAD_DOWN)
               | (d.BTN_LEFT << PS3PAD_LEFT) | (d.BTN_RIGHT << PS3PAD_RIGHT);
}

const ps3pad_state *ps3pad_get(void) { return &st; }

/* Rumble: large motor 0..1, small motor on/off. The large motor does not
   turn below about a quarter of its speed, so weak values start there. */
void ps3pad_rumble(float large, int small)
{
    static int last_large = -1, last_small = -1;
    padActParam act;
    int l = large < 0.05f ? 0 : 64 + (int)((large > 1.0f ? 1.0f : large) * 191.0f);
    small = small != 0;
    if (!st.connected || (l == last_large && small == last_small)) return;
    memset(&act, 0, sizeof act);
    act.small_motor = small;
    act.large_motor = l;
    if (ioPadSetActDirect(0, &act) == 0) {
        last_large = l;
        last_small = small;
    }
}

void ps3pad_report(char *buf, int n)
{
    snprintf(buf, n, "pad: polls %lu, with data %lu, with sensors %lu, len %d; "
             "press/sensor/port setting %d/%d/%d, sensor mode %d; sensors x %u y %u z %u g %u; tilt %.2f %.2f",
             polls, with_data, with_sensors, last_len, rc_press, rc_sensor, rc_port,
             st.connected ? ioPadInfoSensorMode(0) : -1, raw_x, raw_y, raw_z, raw_g,
             st.axis[PS3PAD_TILT_ROLL], st.axis[PS3PAD_TILT_PITCH]);
}
