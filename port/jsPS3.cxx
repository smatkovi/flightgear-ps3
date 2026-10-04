// PLIB joystick driver for the PS3 controller, including the Sixaxis tilt axes.
// Joystick 0 is the first controller; the state comes from ps3pad, which the
// main loop polls once per frame.

#include "js.h"
#include "../port/ps3pad.h"

struct os_specific_s { int unused ; } ;

void jsJoystick::open ()
{
  error = ( id != 0 ) ;
}

void jsJoystick::close () {}

jsJoystick::jsJoystick ( int ident )
{
  id = ident ;
  os = NULL ;
  error = ( ident != 0 ) ;
  strcpy ( name, "PS3 Sixaxis" ) ;
  num_axes = error ? 0 : PS3PAD_AXES ;
  num_buttons = error ? 0 : PS3PAD_BUTTONS ;

  for ( int i = 0 ; i < _JS_MAX_AXES ; i++ )
  {
    dead_band [ i ] = 0.0f ;
    saturate  [ i ] = 1.0f ;
    center    [ i ] = 0.0f ;
    max       [ i ] = 1.0f ;
    min       [ i ] = -1.0f ;
  }
  ps3pad_init () ;
}

void jsJoystick::rawRead ( int *buttons, float *axes )
{
  const ps3pad_state *st = ps3pad_get () ;

  if ( ps3pad_muted () ) {         /* the START menu is open */
    if ( buttons != NULL ) *buttons = 0 ;
    if ( axes != NULL ) memset ( axes, 0, sizeof(float) * num_axes ) ;
    return ;
  }
  if ( buttons != NULL ) *buttons = error ? 0 : (int) st->buttons ;
  if ( axes != NULL && ! error )
    memcpy ( axes, st->axis, sizeof(float) * PS3PAD_AXES ) ;
}

void jsInit () {}
