//========= Copyright � 1996-2002, Valve LLC, All rights reserved. ============
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================

// in_win.c -- windows 95 mouse and joystick code
// 02/21/97 JCB Added extended DirectInput code to support external controllers.

#include "port.h"

#include "hud.h"
#include "cl_util.h"
#include "camera.h"
#include "kbutton.h"
#include "cvardef.h"
#include "usercmd.h"
#include "const.h"
#include "camera.h"
#include "in_defs.h"
#include "../public/keydefs.h"
#include "view.h"
#include "Exports.h"

#include <SDL2/SDL_mouse.h>
#include <SDL2/SDL_gamecontroller.h>

#ifdef _WIN32
#include <process.h>
#endif

#define MOUSE_BUTTON_COUNT 5

// Use IN_SetVisibleMouse to set this
int g_iVisibleMouse = 0;

extern cl_enginefunc_t gEngfuncs;
extern int iMouseInUse;

extern kbutton_t	in_strafe;
extern kbutton_t	in_mlook;
extern kbutton_t	in_speed;
extern kbutton_t	in_jlook;

extern cvar_t	*m_pitch;
extern cvar_t	*m_yaw;
extern cvar_t	*m_forward;
extern cvar_t	*m_side;

extern cvar_t *lookstrafe;
extern cvar_t *lookspring;
extern cvar_t *cl_pitchdown;
extern cvar_t *cl_pitchup;
extern cvar_t *cl_yawspeed;
extern cvar_t *cl_sidespeed;
extern cvar_t *cl_forwardspeed;
extern cvar_t *cl_pitchspeed;
extern cvar_t *cl_movespeedkey;

#ifdef WIN32
static double s_flRawInputUpdateTime = 0.0f;
static bool m_bRawInput = false;
static bool m_bMouseThread = false;
bool g_bIsMouseRelative = false;
#endif

// mouse variables
cvar_t		*m_filter;
cvar_t		*sensitivity;

// Custom mouse acceleration (0 disable, 1 to enable, 2 enable with separate yaw/pitch rescale)
static cvar_t *m_customaccel;
//Formula: mousesensitivity = ( rawmousedelta^m_customaccel_exponent ) * m_customaccel_scale + sensitivity
// If mode is 2, then x and y sensitivity are scaled by m_pitch and m_yaw respectively.
// Custom mouse acceleration value.
static cvar_t *m_customaccel_scale;
//Max mouse move scale factor, 0 for no limit
static cvar_t *m_customaccel_max;
//Mouse move is raised to this power before being scaled by scale factor
static cvar_t *m_customaccel_exponent;

#ifdef WIN32
// if threaded mouse is enabled then the time to sleep between polls
static cvar_t *m_mousethread_sleep;
#endif

int			mouse_buttons;
int			mouse_oldbuttonstate;
POINT		current_pos;
int			old_mouse_x, old_mouse_y, mx_accum, my_accum;
float		mouse_x, mouse_y;

static int	restore_spi;
static int	originalmouseparms[3], newmouseparms[3] = {0, 0, 1};
static int	mouseactive = 0;
int			mouseinitialized;
static int	mouseparmsvalid;
static int	mouseshowtoggle = 1;

// joystick defines and variables
// where should defines be moved?
#define JOY_ABSOLUTE_AXIS	0x00000000		// control like a joystick
#define JOY_RELATIVE_AXIS	0x00000010		// control like a mouse, spinner, trackball
#define	JOY_MAX_AXES		6				// X, Y, Z, R, U, V
#define JOY_AXIS_X			0
#define JOY_AXIS_Y			1
#define JOY_AXIS_Z			2
#define JOY_AXIS_R			3
#define JOY_AXIS_U			4
#define JOY_AXIS_V			5

enum _ControlList
{
	AxisNada = 0,
	AxisForward,
	AxisLook,
	AxisSide,
	AxisTurn
};



DWORD	dwAxisMap[ JOY_MAX_AXES ];
DWORD	dwControlMap[ JOY_MAX_AXES ];
int	pdwRawValue[ JOY_MAX_AXES ];
DWORD		joy_oldbuttonstate, joy_oldpovstate;

int			joy_id;
DWORD		joy_numbuttons;

SDL_GameController *s_pJoystick = NULL;

// none of these cvars are saved over a session
// this means that advanced controller configuration needs to be executed
// each time.  this avoids any problems with getting back to a default usage
// or when changing from one controller to another.  this way at least something
// works.
cvar_t	*in_joystick;
cvar_t	*joy_name;
cvar_t	*joy_advanced;
cvar_t	*joy_advaxisx;
cvar_t	*joy_advaxisy;
cvar_t	*joy_advaxisz;
cvar_t	*joy_advaxisr;
cvar_t	*joy_advaxisu;
cvar_t	*joy_advaxisv;
cvar_t	*joy_forwardthreshold;
cvar_t	*joy_sidethreshold;
cvar_t	*joy_pitchthreshold;
cvar_t	*joy_yawthreshold;
cvar_t	*joy_forwardsensitivity;
cvar_t	*joy_sidesensitivity;
cvar_t	*joy_pitchsensitivity;
cvar_t	*joy_yawsensitivity;
cvar_t	*joy_wwhack1;
cvar_t	*joy_wwhack2;

int			joy_avail, joy_advancedinit, joy_haspov;

#ifdef _WIN32
unsigned int s_hMouseThreadId = 0;
HANDLE	s_hMouseThread = 0;
HANDLE	s_hMouseQuitEvent = 0;
HANDLE	s_hMouseThreadActiveLock = 0;
#endif

/*
===========
Force_CenterView_f
===========
*/
void Force_CenterView_f (void)
{
	vec3_t viewangles;

	if (!iMouseInUse)
	{
		gEngfuncs.GetViewAngles( (float *)viewangles );
	    viewangles[PITCH] = 0;
		gEngfuncs.SetViewAngles( (float *)viewangles );
	}
}

#ifdef _WIN32
LONG g_lMouseThreadActive = 0;
LONG g_lMouseThreadCenterX = 0;
LONG g_lMouseThreadCenterY = 0;
LONG g_lMouseThreadDeltaX = 0;
LONG g_lMouseThreadDeltaY = 0;
LONG g_lMouseThreadSleep = 0;

bool MouseThread_ActiveLock_Enter()
{
	if ( !m_bMouseThread )
		return true;

	return WAIT_OBJECT_0 == WaitForSingleObject( s_hMouseThreadActiveLock, INFINITE );
}

void MouseThread_ActiveLock_Exit()
{
	if ( !m_bMouseThread )
		return;

	SetEvent( s_hMouseThreadActiveLock );
}

unsigned __stdcall MouseThread_Function( void *pArg )
{
	while ( true )
	{
		DWORD sleepVal = (DWORD)InterlockedExchangeAdd( &g_lMouseThreadSleep, 0 );
		if ( 0 > sleepVal )
			sleepVal = 0;
		else if ( 1000 < sleepVal )
			sleepVal = 1000;

		if ( WAIT_OBJECT_0 == WaitForSingleObject( s_hMouseQuitEvent, sleepVal ) )
			break;

		if ( MouseThread_ActiveLock_Enter() )
		{
			if ( InterlockedExchangeAdd( &g_lMouseThreadActive, 0 ) )
			{
				POINT mousePos;
				POINT centerPos;

				centerPos.x = InterlockedExchangeAdd( &g_lMouseThreadCenterX, 0 );
				centerPos.y = InterlockedExchangeAdd( &g_lMouseThreadCenterY, 0 );
				GetCursorPos( &mousePos );

				mousePos.x -= centerPos.x;
				mousePos.y -= centerPos.y;

				if ( mousePos.x || mousePos.y )
					SetCursorPos( centerPos.x, centerPos.y );

				InterlockedExchangeAdd( &g_lMouseThreadDeltaX, mousePos.x );
				InterlockedExchangeAdd( &g_lMouseThreadDeltaY, mousePos.y );
			}

			MouseThread_ActiveLock_Exit();
		}
	}

	return 0;
}

// Updates g_lMouseThreadActive using the global variables mouseactive, g_iVisibleMouse and m_bRawInput.
// Should be called after any of these is changed.
// Has to be interlocked manually by programmer! Use MouseThread_ActiveLock_Enter and MouseThread_ActiveLock_Exit.
void UpdateMouseThreadActive(void)
{
	InterlockedExchange( &g_lMouseThreadActive, mouseactive && !g_iVisibleMouse && !m_bRawInput );
}
#endif

void IN_SetMouseMode( bool bEnable )
{
	static bool bCurrentMouseMode = false;

	if ( bEnable == bCurrentMouseMode )
		return;

	if ( bEnable )
	{
#ifdef _WIN32
		if (mouseparmsvalid)
			restore_spi = SystemParametersInfo( SPI_SETMOUSE, 0, newmouseparms, 0 );

		m_bRawInput = CVAR_GET_FLOAT( "m_rawinput" ) != 0.0f;
		if ( m_bRawInput )
		{
			SDL_SetRelativeMouseMode( SDL_TRUE );
			g_bIsMouseRelative = true;
		}
#else
		SDL_SetRelativeMouseMode( SDL_TRUE );
#endif
		bCurrentMouseMode = true;
	}
	else
	{
#ifdef _WIN32
		if ( g_bIsMouseRelative )
		{
			SDL_SetRelativeMouseMode( SDL_FALSE );
			g_bIsMouseRelative = false;
		}

		if ( restore_spi )
			SystemParametersInfo( SPI_SETMOUSE, 0, originalmouseparms, 0 );
#else
		SDL_SetRelativeMouseMode( SDL_FALSE );
#endif
		bCurrentMouseMode = false;
	}
}

void IN_SetVisibleMouse( bool bVisible )
{
#ifdef _WIN32
	bool bLockEntered = MouseThread_ActiveLock_Enter();
#endif

	g_iVisibleMouse = bVisible;
	IN_SetMouseMode( !bVisible );

#ifdef _WIN32
	UpdateMouseThreadActive();
	if ( bLockEntered )
		MouseThread_ActiveLock_Exit();
#endif
}

void IN_ResetMouse();

/*
===========
IN_ActivateMouse
===========
*/
void DLLEXPORT IN_ActivateMouse (void)
{
	if (mouseinitialized)
	{
#ifdef _WIN32
		bool bLockEntered = MouseThread_ActiveLock_Enter();
#endif
		IN_SetMouseMode( true );
		mouseactive = 1;
#ifdef _WIN32
		UpdateMouseThreadActive();
		if ( bLockEntered )
			MouseThread_ActiveLock_Exit();
#endif
		// now is a good time to reset mouse positon:
		IN_ResetMouse();
	}
}


/*
===========
IN_DeactivateMouse
===========
*/
void DLLEXPORT IN_DeactivateMouse (void)
{
	if (mouseinitialized)
	{
#ifdef _WIN32
		bool bLockEntered = MouseThread_ActiveLock_Enter();
#endif
		IN_SetMouseMode( false );
		mouseactive = 0;
#ifdef _WIN32
		UpdateMouseThreadActive();
		if ( bLockEntered )
			MouseThread_ActiveLock_Exit();
#endif
	}
}

/*
===========
IN_StartupMouse
===========
*/
void IN_StartupMouse (void)
{
	if ( gEngfuncs.CheckParm ("-nomouse", NULL ) ) 
		return; 

	mouseinitialized = 1;
#ifdef _WIN32
	mouseparmsvalid = SystemParametersInfo (SPI_GETMOUSE, 0, originalmouseparms, 0);

	if (mouseparmsvalid)
	{
		if ( gEngfuncs.CheckParm ("-noforcemspd", NULL ) ) 
			newmouseparms[2] = originalmouseparms[2];

		if ( gEngfuncs.CheckParm ("-noforcemaccel", NULL ) ) 
		{
			newmouseparms[0] = originalmouseparms[0];
			newmouseparms[1] = originalmouseparms[1];
		}

		if ( gEngfuncs.CheckParm ("-noforcemparms", NULL ) ) 
		{
			newmouseparms[0] = originalmouseparms[0];
			newmouseparms[1] = originalmouseparms[1];
			newmouseparms[2] = originalmouseparms[2];
		}
	}
#endif
	
	mouse_buttons = MOUSE_BUTTON_COUNT;
}

/*
===========
IN_Shutdown
===========
*/
void IN_Shutdown (void)
{
	IN_DeactivateMouse ();

#ifdef _WIN32
	if ( s_hMouseQuitEvent )
	{
		SetEvent( s_hMouseQuitEvent );
	}

	if ( s_hMouseThread )
	{
		if ( WAIT_OBJECT_0 != WaitForSingleObject( s_hMouseThread, 5000 ) )
			TerminateThread( s_hMouseThread, 0 );

		CloseHandle( s_hMouseThread );
		s_hMouseThread = (HANDLE)0;
	}

	if ( s_hMouseQuitEvent )
	{
		CloseHandle( s_hMouseQuitEvent );
		s_hMouseQuitEvent = (HANDLE)0;
	}

	if( s_hMouseThreadActiveLock )
	{
		CloseHandle( s_hMouseThreadActiveLock );
		s_hMouseThreadActiveLock = (HANDLE)0;
	}
#endif
}

/*
===========
IN_GetMousePos

Ask for mouse position from engine
===========
*/
void IN_GetMousePos( int *mx, int *my )
{
	gEngfuncs.GetMousePosition( mx, my );
}

/*
===========
IN_ResetMouse

FIXME: Call through to engine?
===========
*/
void IN_ResetMouse( void )
{
	// no work to do in SDL
#ifdef _WIN32
	// reset only if mouse is active and not in visible mode:
	if ( mouseactive && !g_iVisibleMouse )
	{
		if ( !m_bRawInput && gEngfuncs.GetWindowCenterX() && gEngfuncs.GetWindowCenterY() )
		{
			bool bLockEntered = MouseThread_ActiveLock_Enter();

			int iCenterX = gEngfuncs.GetWindowCenterX();
			int iCenterY = gEngfuncs.GetWindowCenterY();

			SetCursorPos( iCenterX, iCenterY );
			InterlockedExchange( &g_lMouseThreadCenterX, iCenterX );
			InterlockedExchange( &g_lMouseThreadCenterY, iCenterY );
			InterlockedExchange( &g_lMouseThreadDeltaX, 0 );
			InterlockedExchange( &g_lMouseThreadDeltaY, 0 );

			if ( bLockEntered )
				MouseThread_ActiveLock_Exit();
		}
	}
#endif
}

/*
===========
IN_MouseEvent
===========
*/
void DLLEXPORT IN_MouseEvent (int mstate)
{
	int		i;

	if ( iMouseInUse || g_iVisibleMouse )
		return;

	// perform button actions
	for (i=0 ; i<mouse_buttons ; i++)
	{
		if ( (mstate & (1<<i)) &&
			!(mouse_oldbuttonstate & (1<<i)) )
		{
			gEngfuncs.Key_Event (K_MOUSE1 + i, 1);
		}

		if ( !(mstate & (1<<i)) &&
			(mouse_oldbuttonstate & (1<<i)) )
		{
			gEngfuncs.Key_Event (K_MOUSE1 + i, 0);
		}
	}	
	
	mouse_oldbuttonstate = mstate;
}

//-----------------------------------------------------------------------------
// Purpose: Allows modulation of mouse scaling/senstivity value and application
//  of custom algorithms.
// Input  : *x - 
//			*y - 
//-----------------------------------------------------------------------------
void IN_ScaleMouse( float *x, float *y )
{
	float mx = *x;
	float my = *y;

	// This is the default sensitivity
	float mouse_senstivity = ( gHUD.GetSensitivity() != 0 ) ? gHUD.GetSensitivity() : sensitivity->value;

	// Using special accleration values
	if ( m_customaccel->value != 0 ) 
	{ 
		float raw_mouse_movement_distance = sqrt( mx * mx + my * my );
		float acceleration_scale = m_customaccel_scale->value;
		float accelerated_sensitivity_max = m_customaccel_max->value;
		float accelerated_sensitivity_exponent = m_customaccel_exponent->value;
		float accelerated_sensitivity = ( (float)pow( raw_mouse_movement_distance, accelerated_sensitivity_exponent ) * acceleration_scale + mouse_senstivity );

		if ( accelerated_sensitivity_max > 0.0001f && 
			accelerated_sensitivity > accelerated_sensitivity_max )
		{
			accelerated_sensitivity = accelerated_sensitivity_max;
		}

		*x *= accelerated_sensitivity; 
		*y *= accelerated_sensitivity; 

		// Further re-scale by yaw and pitch magnitude if user requests alternate mode 2
		// This means that they will need to up their value for m_customaccel_scale greatly (>40x) since m_pitch/yaw default
		//  to 0.022
		if ( m_customaccel->value == 2 )
		{ 
			*x *= m_yaw->value; 
			*y *= m_pitch->value; 
		} 
	}
	else
	{ 
		// Just apply the default
		*x *= mouse_senstivity;
		*y *= mouse_senstivity;
	}
}

void IN_GetMouseDelta( int *piOutX, int *piOutY )
{
	bool bActive = mouseactive && !g_iVisibleMouse;
	int iMouseX, iMouseY;

	if ( bActive )
	{
		int iDeltaX, iDeltaY;
#ifdef _WIN32
		if ( !m_bRawInput )
		{
			if ( m_bMouseThread )
			{
				// update mouseThreadSleep:
				InterlockedExchange( &g_lMouseThreadSleep, (LONG)m_mousethread_sleep->value );

				bool bLockEntered = MouseThread_ActiveLock_Enter();

				current_pos.x = InterlockedExchange( &g_lMouseThreadDeltaX, 0 );
				current_pos.y = InterlockedExchange( &g_lMouseThreadDeltaY, 0 );

				if ( bLockEntered )
					MouseThread_ActiveLock_Exit();
			}
			else
			{
				GetCursorPos( &current_pos );
			}
		}
		else
#endif
		{
			SDL_GetRelativeMouseState( &iDeltaX, &iDeltaY );
			current_pos.x = iDeltaX;
			current_pos.y = iDeltaY;
		}
		
#ifdef _WIN32
		if ( !m_bRawInput )
		{
			if ( m_bMouseThread )
			{
				iMouseX = current_pos.x;
				iMouseY = current_pos.y;
			}
			else
			{
				iMouseX = current_pos.x - gEngfuncs.GetWindowCenterX() + mx_accum;
				iMouseY = current_pos.y - gEngfuncs.GetWindowCenterY() + my_accum;
			}
		}
		else
#endif
		{
			iMouseX = iDeltaX + mx_accum;
			iMouseY = iDeltaY + my_accum;
		}

		mx_accum = 0;
		my_accum = 0;

		// reset mouse position if required, so there is room to move:
#ifdef _WIN32
		// do not reset if mousethread would do it:
		if ( m_bRawInput || !m_bMouseThread )
			IN_ResetMouse();
#else
		IN_ResetMouse();
#endif

#ifdef _WIN32
		// update m_bRawInput occasionally: 
		const float currentTime = gEngfuncs.GetClientTime();
		if ((currentTime - s_flRawInputUpdateTime) > 1.0f || s_flRawInputUpdateTime == 0.0f)
		{
			s_flRawInputUpdateTime = currentTime;

			bool bLockEntered = MouseThread_ActiveLock_Enter();

			m_bRawInput = CVAR_GET_FLOAT( "m_rawinput" ) != 0.0f;

			if ( m_bRawInput && !g_bIsMouseRelative )
			{
				SDL_SetRelativeMouseMode( SDL_TRUE );
				g_bIsMouseRelative = true;
			}
			else if (!m_bRawInput && g_bIsMouseRelative)
			{
				SDL_SetRelativeMouseMode( SDL_FALSE );
				g_bIsMouseRelative = false;
			}

			UpdateMouseThreadActive();
			if ( bLockEntered )
				MouseThread_ActiveLock_Exit();
		}
#endif
	}
	else
	{
		iMouseX = iMouseY = 0;
	}

	if ( piOutX )
		*piOutX = iMouseX;

	if ( piOutY )
		*piOutY = iMouseY;
}

/*
===========
IN_MouseMove
===========
*/
void IN_MouseMove( float frametime, usercmd_t *cmd )
{
	int iMouseX, iMouseY;
	vec3_t vecViewAngles;

	gEngfuncs.GetViewAngles( (float *)vecViewAngles );

	if ( in_mlook.state & 1 )
		V_StopPitchDrift();

	//jjb - this disbles normal mouse control if the user is trying to 
	//      move the camera, or if the mouse cursor is visible or if we're in intermission
	if ( !iMouseInUse && !gHUD.m_iIntermission && !g_iVisibleMouse )
	{
		IN_GetMouseDelta( &iMouseX, &iMouseY );

		if ( m_filter && m_filter->value )
		{
			mouse_x = (iMouseX + old_mouse_x) * 0.5;
			mouse_y = (iMouseY + old_mouse_y) * 0.5;
		}
		else
		{
			mouse_x = iMouseX;
			mouse_y = iMouseY;
		}

		old_mouse_x = iMouseX;
		old_mouse_y = iMouseY;

		// Apply custom mouse scaling/acceleration
		IN_ScaleMouse( &mouse_x, &mouse_y );

		// add mouse X/Y movement to cmd
		if ( (in_strafe.state & 1) || (lookstrafe->value && (in_mlook.state & 1)) )
			cmd->sidemove += m_side->value * mouse_x;
		else
			vecViewAngles[YAW] -= m_yaw->value * mouse_x;

		if ( (in_mlook.state & 1) && !(in_strafe.state & 1) )
		{
			vecViewAngles[PITCH] += m_pitch->value * mouse_y;
			if ( vecViewAngles[PITCH] > cl_pitchdown->value )
				vecViewAngles[PITCH] = cl_pitchdown->value;

			if ( vecViewAngles[PITCH] < -cl_pitchup->value )
				vecViewAngles[PITCH] = -cl_pitchup->value;
		}
		else
		{
			if ( (in_strafe.state & 1) && gEngfuncs.IsNoClipping() )
			{
				cmd->upmove -= m_forward->value * mouse_y;
			}
			else
			{
				cmd->forwardmove -= m_forward->value * mouse_y;
			}
		}
	}

	gEngfuncs.SetViewAngles( (float *)vecViewAngles );

/*
//#define TRACE_TEST
#if defined( TRACE_TEST )
	{
		int mx, my;
		void V_Move( int mx, int my );
		IN_GetMousePos( &mx, &my );
		V_Move( mx, my );
	}
#endif
*/
}

/*
===========
IN_Accumulate
===========
*/
void DLLEXPORT IN_Accumulate (void)
{
	//only accumulate mouse if we are not moving the camera with the mouse
	if ( !iMouseInUse && !g_iVisibleMouse)
	{
	    if (mouseactive)
	    {
#ifdef _WIN32
			if ( !m_bRawInput )
			{
				if ( !m_bMouseThread )
				{
					GetCursorPos (&current_pos);
					
					mx_accum += current_pos.x - gEngfuncs.GetWindowCenterX();
					my_accum += current_pos.y - gEngfuncs.GetWindowCenterY();
				}
			}
			else
#endif
			{
				int deltaX, deltaY;
				SDL_GetRelativeMouseState( &deltaX, &deltaY );
				mx_accum += deltaX;
				my_accum += deltaY;	
			}

#ifdef WIN32
			// do not reset if mousethread would do it
			if ( m_bRawInput || !m_bMouseThread )
				IN_ResetMouse();
#else
			IN_ResetMouse();
#endif
		}
	}

}

/*
===================
IN_ClearStates
===================
*/
void DLLEXPORT IN_ClearStates (void)
{
	if ( !mouseactive )
		return;

	mx_accum = 0;
	my_accum = 0;
	mouse_oldbuttonstate = 0;
}

/* 
=============== 
IN_StartupJoystick 
=============== 
*/  
void IN_StartupJoystick (void) 
{ 
	// abort startup if user requests no joystick
	if ( gEngfuncs.CheckParm ("-nojoy", NULL ) ) 
		return; 
 
 	// assume no joystick
	joy_avail = 0; 

	int nJoysticks = SDL_NumJoysticks();
	if ( nJoysticks > 0 )
	{
		for ( int i = 0; i < nJoysticks; i++ )
		{
			if ( SDL_IsGameController( i ) )
			{
				s_pJoystick = SDL_GameControllerOpen( i );
				if ( s_pJoystick )
				{
					//save the joystick's number of buttons and POV status
					joy_numbuttons = SDL_CONTROLLER_BUTTON_MAX;
					joy_haspov = 0;
					
					// old button and POV states default to no buttons pressed
					joy_oldbuttonstate = joy_oldpovstate = 0;
					
					// mark the joystick as available and advanced initialization not completed
					// this is needed as cvars are not available during initialization
					gEngfuncs.Con_Printf ("joystick found\n\n", SDL_GameControllerName(s_pJoystick)); 
					joy_avail = 1; 
					joy_advancedinit = 0;
					break;
				}

			}
		}
	}
	else
	{
		gEngfuncs.Con_DPrintf ("joystick not found -- driver not present\n\n");
	}
	
}


int RawValuePointer (int axis)
{
	switch (axis)
	{
		default:
		case JOY_AXIS_X:
			return SDL_GameControllerGetAxis( s_pJoystick, SDL_CONTROLLER_AXIS_LEFTX );
		case JOY_AXIS_Y:
			return SDL_GameControllerGetAxis( s_pJoystick, SDL_CONTROLLER_AXIS_LEFTY );
		case JOY_AXIS_Z:
			return SDL_GameControllerGetAxis( s_pJoystick, SDL_CONTROLLER_AXIS_RIGHTX );
		case JOY_AXIS_R:
			return SDL_GameControllerGetAxis( s_pJoystick, SDL_CONTROLLER_AXIS_RIGHTY );
		
	}
}

/*
===========
Joy_AdvancedUpdate_f
===========
*/
void Joy_AdvancedUpdate_f (void)
{

	// called once by IN_ReadJoystick and by user whenever an update is needed
	// cvars are now available
	int	i;
	DWORD dwTemp;

	// initialize all the maps
	for (i = 0; i < JOY_MAX_AXES; i++)
	{
		dwAxisMap[i] = AxisNada;
		dwControlMap[i] = JOY_ABSOLUTE_AXIS;
		pdwRawValue[i] = RawValuePointer(i);
	}

	if( joy_advanced->value == 0.0)
	{
		// default joystick initialization
		// 2 axes only with joystick control
		dwAxisMap[JOY_AXIS_X] = AxisTurn;
		// dwControlMap[JOY_AXIS_X] = JOY_ABSOLUTE_AXIS;
		dwAxisMap[JOY_AXIS_Y] = AxisForward;
		// dwControlMap[JOY_AXIS_Y] = JOY_ABSOLUTE_AXIS;
	}
	else
	{
		if ( strcmp ( joy_name->string, "joystick") != 0 )
		{
			// notify user of advanced controller
			gEngfuncs.Con_Printf ("\n%s configured\n\n", joy_name->string);
		}

		// advanced initialization here
		// data supplied by user via joy_axisn cvars
		dwTemp = (DWORD) joy_advaxisx->value;
		dwAxisMap[JOY_AXIS_X] = dwTemp & 0x0000000f;
		dwControlMap[JOY_AXIS_X] = dwTemp & JOY_RELATIVE_AXIS;
		dwTemp = (DWORD) joy_advaxisy->value;
		dwAxisMap[JOY_AXIS_Y] = dwTemp & 0x0000000f;
		dwControlMap[JOY_AXIS_Y] = dwTemp & JOY_RELATIVE_AXIS;
		dwTemp = (DWORD) joy_advaxisz->value;
		dwAxisMap[JOY_AXIS_Z] = dwTemp & 0x0000000f;
		dwControlMap[JOY_AXIS_Z] = dwTemp & JOY_RELATIVE_AXIS;
		dwTemp = (DWORD) joy_advaxisr->value;
		dwAxisMap[JOY_AXIS_R] = dwTemp & 0x0000000f;
		dwControlMap[JOY_AXIS_R] = dwTemp & JOY_RELATIVE_AXIS;
		dwTemp = (DWORD) joy_advaxisu->value;
		dwAxisMap[JOY_AXIS_U] = dwTemp & 0x0000000f;
		dwControlMap[JOY_AXIS_U] = dwTemp & JOY_RELATIVE_AXIS;
		dwTemp = (DWORD) joy_advaxisv->value;
		dwAxisMap[JOY_AXIS_V] = dwTemp & 0x0000000f;
		dwControlMap[JOY_AXIS_V] = dwTemp & JOY_RELATIVE_AXIS;
	}
}


/*
===========
IN_Commands
===========
*/
void IN_Commands (void)
{
	int		i, key_index;

	if (!joy_avail)
	{
		return;
	}

	DWORD	buttonstate, povstate;
	
	// loop through the joystick buttons
	// key a joystick event or auxillary event for higher number buttons for each state change
	buttonstate = 0;
	for ( i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++ )
	{
		if ( SDL_GameControllerGetButton( s_pJoystick, (SDL_GameControllerButton)i ) )
		{
			buttonstate |= 1<<i;
		}
	}
	
	for (i = 0; i < JOY_MAX_AXES; i++)
	{
		pdwRawValue[i] = RawValuePointer(i);
	}

	for (i=0 ; i < (int)joy_numbuttons ; i++)
	{
		if ( (buttonstate & (1<<i)) && !(joy_oldbuttonstate & (1<<i)) )
		{
			key_index = (i < 4) ? K_JOY1 : K_AUX1;
			gEngfuncs.Key_Event (key_index + i, 1);
		}

		if ( !(buttonstate & (1<<i)) && (joy_oldbuttonstate & (1<<i)) )
		{
			key_index = (i < 4) ? K_JOY1 : K_AUX1;
			gEngfuncs.Key_Event (key_index + i, 0);
		}
	}
	joy_oldbuttonstate = buttonstate;

	if (joy_haspov)
	{
		// convert POV information into 4 bits of state information
		// this avoids any potential problems related to moving from one
		// direction to another without going through the center position
		povstate = 0;
		// determine which bits have changed and key an auxillary event for each change
		for (i=0 ; i < 4 ; i++)
		{
			if ( (povstate & (1<<i)) && !(joy_oldpovstate & (1<<i)) )
			{
				gEngfuncs.Key_Event (K_AUX29 + i, 1);
			}

			if ( !(povstate & (1<<i)) && (joy_oldpovstate & (1<<i)) )
			{
				gEngfuncs.Key_Event (K_AUX29 + i, 0);
			}
		}
		joy_oldpovstate = povstate;
	}
}


/* 
=============== 
IN_ReadJoystick
=============== 
*/  
int IN_ReadJoystick (void)
{
	SDL_JoystickUpdate();
	return 1;
}


/*
===========
IN_JoyMove
===========
*/
void IN_JoyMove ( float frametime, usercmd_t *cmd )
{
	float	speed, aspeed;
	float	fAxisValue, fTemp;
	int		i;
	vec3_t viewangles;

	gEngfuncs.GetViewAngles( (float *)viewangles );


	// complete initialization if first time in
	// this is needed as cvars are not available at initialization time
	if( joy_advancedinit != 1 )
	{
		Joy_AdvancedUpdate_f();
		joy_advancedinit = 1;
	}

	// verify joystick is available and that the user wants to use it
	if (!joy_avail || !in_joystick->value)
	{
		return; 
	}
 
	// collect the joystick data, if possible
	if (IN_ReadJoystick () != 1)
	{
		return;
	}

	if (in_speed.state & 1)
		speed = cl_movespeedkey->value;
	else
		speed = 1;

	aspeed = speed * frametime;

	// loop through the axes
	for (i = 0; i < JOY_MAX_AXES; i++)
	{
		// get the floating point zero-centered, potentially-inverted data for the current axis
		fAxisValue = (float)pdwRawValue[i];

		if (joy_wwhack2->value != 0.0)
		{
			if (dwAxisMap[i] == AxisTurn)
			{
				// this is a special formula for the Logitech WingMan Warrior
				// y=ax^b; where a = 300 and b = 1.3
				// also x values are in increments of 800 (so this is factored out)
				// then bounds check result to level out excessively high spin rates
				fTemp = 300.0 * pow( fabs(fAxisValue) / 800.0, 1.3);
				if (fTemp > 14000.0)
					fTemp = 14000.0;
				// restore direction information
				fAxisValue = (fAxisValue > 0.0) ? fTemp : -fTemp;
			}
		}

		// convert range from -32768..32767 to -1..1 
		fAxisValue /= 32768.0;

		switch (dwAxisMap[i])
		{
		case AxisForward:
			if ((joy_advanced->value == 0.0) && (in_jlook.state & 1))
			{
				// user wants forward control to become look control
				if (fabs(fAxisValue) > joy_pitchthreshold->value)
				{		
					// if mouse invert is on, invert the joystick pitch value
					// only absolute control support here (joy_advanced is 0)
					if (m_pitch->value < 0.0)
					{
						viewangles[PITCH] -= (fAxisValue * joy_pitchsensitivity->value) * aspeed * cl_pitchspeed->value;
					}
					else
					{
						viewangles[PITCH] += (fAxisValue * joy_pitchsensitivity->value) * aspeed * cl_pitchspeed->value;
					}
					V_StopPitchDrift();
				}
				else
				{
					// no pitch movement
					// disable pitch return-to-center unless requested by user
					// *** this code can be removed when the lookspring bug is fixed
					// *** the bug always has the lookspring feature on
					if(lookspring->value == 0.0)
					{
						V_StopPitchDrift();
					}
				}
			}
			else
			{
				// user wants forward control to be forward control
				if (fabs(fAxisValue) > joy_forwardthreshold->value)
				{
					cmd->forwardmove += (fAxisValue * joy_forwardsensitivity->value) * speed * cl_forwardspeed->value;
				}
			}
			break;

		case AxisSide:
			if (fabs(fAxisValue) > joy_sidethreshold->value)
			{
				cmd->sidemove += (fAxisValue * joy_sidesensitivity->value) * speed * cl_sidespeed->value;
			}
			break;

		case AxisTurn:
			if ((in_strafe.state & 1) || (lookstrafe->value && (in_jlook.state & 1)))
			{
				// user wants turn control to become side control
				if (fabs(fAxisValue) > joy_sidethreshold->value)
				{
					cmd->sidemove -= (fAxisValue * joy_sidesensitivity->value) * speed * cl_sidespeed->value;
				}
			}
			else
			{
				// user wants turn control to be turn control
				if (fabs(fAxisValue) > joy_yawthreshold->value)
				{
					if(dwControlMap[i] == JOY_ABSOLUTE_AXIS)
					{
						viewangles[YAW] += (fAxisValue * joy_yawsensitivity->value) * aspeed * cl_yawspeed->value;
					}
					else
					{
						viewangles[YAW] += (fAxisValue * joy_yawsensitivity->value) * speed * 180.0;
					}

				}
			}
			break;

		case AxisLook:
			if (in_jlook.state & 1)
			{
				if (fabs(fAxisValue) > joy_pitchthreshold->value)
				{
					// pitch movement detected and pitch movement desired by user
					if(dwControlMap[i] == JOY_ABSOLUTE_AXIS)
					{
						viewangles[PITCH] += (fAxisValue * joy_pitchsensitivity->value) * aspeed * cl_pitchspeed->value;
					}
					else
					{
						viewangles[PITCH] += (fAxisValue * joy_pitchsensitivity->value) * speed * 180.0;
					}
					V_StopPitchDrift();
				}
				else
				{
					// no pitch movement
					// disable pitch return-to-center unless requested by user
					// *** this code can be removed when the lookspring bug is fixed
					// *** the bug always has the lookspring feature on
					if( lookspring->value == 0.0 )
					{
						V_StopPitchDrift();
					}
				}
			}
			break;

		default:
			break;
		}
	}

	// bounds check pitch
	if (viewangles[PITCH] > cl_pitchdown->value)
		viewangles[PITCH] = cl_pitchdown->value;
	if (viewangles[PITCH] < -cl_pitchup->value)
		viewangles[PITCH] = -cl_pitchup->value;

	gEngfuncs.SetViewAngles( (float *)viewangles );
}

/*
===========
IN_Move
===========
*/
void IN_Move ( float frametime, usercmd_t *cmd)
{
	if ( !iMouseInUse && mouseactive )
	{
		IN_MouseMove ( frametime, cmd);
	}

	IN_JoyMove ( frametime, cmd);
}

/*
===========
IN_Init
===========
*/
void IN_Init (void)
{
	m_filter				= gEngfuncs.pfnRegisterVariable ( "m_filter","0", FCVAR_ARCHIVE );
	sensitivity				= gEngfuncs.pfnRegisterVariable ( "sensitivity","3", FCVAR_ARCHIVE ); // user mouse sensitivity setting.

	in_joystick				= gEngfuncs.pfnRegisterVariable ( "joystick","0", FCVAR_ARCHIVE );
	joy_name				= gEngfuncs.pfnRegisterVariable ( "joyname", "joystick", 0 );
	joy_advanced			= gEngfuncs.pfnRegisterVariable ( "joyadvanced", "0", 0 );
	joy_advaxisx			= gEngfuncs.pfnRegisterVariable ( "joyadvaxisx", "0", 0 );
	joy_advaxisy			= gEngfuncs.pfnRegisterVariable ( "joyadvaxisy", "0", 0 );
	joy_advaxisz			= gEngfuncs.pfnRegisterVariable ( "joyadvaxisz", "0", 0 );
	joy_advaxisr			= gEngfuncs.pfnRegisterVariable ( "joyadvaxisr", "0", 0 );
	joy_advaxisu			= gEngfuncs.pfnRegisterVariable ( "joyadvaxisu", "0", 0 );
	joy_advaxisv			= gEngfuncs.pfnRegisterVariable ( "joyadvaxisv", "0", 0 );
	joy_forwardthreshold	= gEngfuncs.pfnRegisterVariable ( "joyforwardthreshold", "0.15", 0 );
	joy_sidethreshold		= gEngfuncs.pfnRegisterVariable ( "joysidethreshold", "0.15", 0 );
	joy_pitchthreshold		= gEngfuncs.pfnRegisterVariable ( "joypitchthreshold", "0.15", 0 );
	joy_yawthreshold		= gEngfuncs.pfnRegisterVariable ( "joyyawthreshold", "0.15", 0 );
	joy_forwardsensitivity	= gEngfuncs.pfnRegisterVariable ( "joyforwardsensitivity", "-1.0", 0 );
	joy_sidesensitivity		= gEngfuncs.pfnRegisterVariable ( "joysidesensitivity", "-1.0", 0 );
	joy_pitchsensitivity	= gEngfuncs.pfnRegisterVariable ( "joypitchsensitivity", "1.0", 0 );
	joy_yawsensitivity		= gEngfuncs.pfnRegisterVariable ( "joyyawsensitivity", "-1.0", 0 );
	joy_wwhack1				= gEngfuncs.pfnRegisterVariable ( "joywwhack1", "0.0", 0 );
	joy_wwhack2				= gEngfuncs.pfnRegisterVariable ( "joywwhack2", "0.0", 0 );

	m_customaccel			= gEngfuncs.pfnRegisterVariable ( "m_customaccel", "0", FCVAR_ARCHIVE );
	m_customaccel_scale		= gEngfuncs.pfnRegisterVariable ( "m_customaccel_scale", "0.04", FCVAR_ARCHIVE );
	m_customaccel_max		= gEngfuncs.pfnRegisterVariable ( "m_customaccel_max", "0", FCVAR_ARCHIVE );
	m_customaccel_exponent	= gEngfuncs.pfnRegisterVariable ( "m_customaccel_exponent", "1", FCVAR_ARCHIVE );

#ifdef _WIN32
	m_bRawInput				= CVAR_GET_FLOAT( "m_rawinput" ) != 0.0f;
	m_bMouseThread			= gEngfuncs.CheckParm ("-mousethread", NULL ) != NULL;
	m_mousethread_sleep			= gEngfuncs.pfnRegisterVariable ( "m_mousethread_sleep", "1", FCVAR_ARCHIVE ); // default to less than 1000 Hz

	m_bMouseThread = m_bMouseThread && nullptr != m_mousethread_sleep;

	if ( m_bMouseThread )
	{
		// init mouseThreadSleep:
		InterlockedExchange( &g_lMouseThreadSleep, (LONG)m_mousethread_sleep->value );
		s_hMouseQuitEvent = CreateEvent( NULL, FALSE, FALSE, NULL );
		s_hMouseThreadActiveLock = CreateEvent( NULL, FALSE, TRUE, NULL );
		if ( s_hMouseQuitEvent && s_hMouseThreadActiveLock )
		{
			s_hMouseThread = (HANDLE)_beginthreadex( NULL, 0, MouseThread_Function, NULL, 0, &s_hMouseThreadId );
		}

		m_bMouseThread = nullptr != s_hMouseThread;
	}
#endif

	gEngfuncs.pfnAddCommand ("force_centerview", Force_CenterView_f);
	gEngfuncs.pfnAddCommand ("joyadvancedupdate", Joy_AdvancedUpdate_f);

	IN_StartupMouse ();
	IN_StartupJoystick ();
}
