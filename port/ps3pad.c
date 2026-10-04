/* PS3 controller state shared by the joystick driver and the window layer. */
#include <string.h>
#include <io/pad.h>
#include "ps3pad.h"

/* The Sixaxis accelerometers read 512 when level and move about 113 counts per g. */
#define ACC_LEVEL   512.0f
#define ACC_RANGE   87.0f      /* sin(50 deg) * 113: full deflection at 50 degrees of tilt */
#define ACC_SMOOTH  0.25f      /* low-pass per frame against sensor noise */

static ps3pad_state st;
static int inited, configured;
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
    if (!configured) {
        ioPadSetPressMode(0, 1);
        ioPadSetSensorMode(0, 1);
        configured = 1;
    }
    st.connected = 1;
    /* ioPadGetData only returns data when something changed */
    if (ioPadGetData(0, &d) != 0 || d.len <= 0) return;

    st.axis[PS3PAD_LX] = stick(d.ANA_L_H);
    st.axis[PS3PAD_LY] = stick(d.ANA_L_V);
    st.axis[PS3PAD_RX] = stick(d.ANA_R_H);
    st.axis[PS3PAD_RY] = stick(d.ANA_R_V);
    if (!acc_valid) {
        acc_x = d.SENSOR_X;
        acc_z = d.SENSOR_Z;
        acc_valid = 1;
    } else {
        acc_x += ACC_SMOOTH * ((float)d.SENSOR_X - acc_x);
        acc_z += ACC_SMOOTH * ((float)d.SENSOR_Z - acc_z);
    }
    if (d.BTN_SELECT && !select_was_down) {     /* recentre the tilt */
        zero_x = acc_x;
        zero_z = acc_z;
    }
    select_was_down = d.BTN_SELECT;
    st.axis[PS3PAD_TILT_ROLL] = tilt(acc_x, zero_x);
    st.axis[PS3PAD_TILT_PITCH] = tilt(acc_z, zero_z);
    st.axis[PS3PAD_L2] = d.PRE_L2 / 255.0f;
    st.axis[PS3PAD_R2] = d.PRE_R2 / 255.0f;
    st.axis[PS3PAD_GYRO] = ((float)d.SENSOR_G - 512.0f) / 512.0f;

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
