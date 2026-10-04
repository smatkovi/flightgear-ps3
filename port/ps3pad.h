/* PS3 controller state shared by the joystick driver and the window layer. */
#ifndef PS3PAD_H
#define PS3PAD_H
#ifdef __cplusplus
extern "C" {
#endif

enum { PS3PAD_LX, PS3PAD_LY, PS3PAD_RX, PS3PAD_RY,
       PS3PAD_TILT_ROLL, PS3PAD_TILT_PITCH,     /* Sixaxis accelerometer, -1..1 at about 50 degrees */
       PS3PAD_L2, PS3PAD_R2,                    /* trigger pressure, 0..1 */
       PS3PAD_GYRO,                             /* yaw rate */
       PS3PAD_AXES };

enum { PS3PAD_CROSS, PS3PAD_CIRCLE, PS3PAD_SQUARE, PS3PAD_TRIANGLE,
       PS3PAD_BL1, PS3PAD_BR1, PS3PAD_BL2, PS3PAD_BR2,
       PS3PAD_SELECT, PS3PAD_START, PS3PAD_L3, PS3PAD_R3,
       PS3PAD_UP, PS3PAD_DOWN, PS3PAD_LEFT, PS3PAD_RIGHT,
       PS3PAD_BUTTONS };

typedef struct {
    int connected;
    float axis[PS3PAD_AXES];
    unsigned buttons;       /* bit n = button n held */
} ps3pad_state;

void ps3pad_init(void);
void ps3pad_poll(void);                 /* once per frame */
const ps3pad_state *ps3pad_get(void);
void ps3pad_report(char *buf, int n);    /* diagnostics: sensor mode and raw values */

#ifdef __cplusplus
}
#endif
#endif
