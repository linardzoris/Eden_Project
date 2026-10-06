#pragma once

enum
{
	AF_GODMODE					= (1 << 0),
	AF_NO_CLIP					= (1 << 1),
	AF_UNLIMITEDAMMO			= (1 << 3),
	AF_RUN_BACKWARD				= (1 << 4),
	AF_AUTOPICKUP				= (1 << 5),
	AF_DYNAMIC_MUSIC			= (1 << 7),
	AF_DISABLE_CONDITION_TEST	= (1 << 8),
	AF_IMPORTANT_SAVE			= (1 << 9),
	AF_CROUCH_TOGGLE			= (1 << 10),
	AF_RIGHT_SHOULDER			= (1 << 11),
	AF_DISPLAY_VOICE_ICON		= (1 << 12),
	AF_INFINITEFIRE				= (1 << 13),
	AF_INFINITEDURABILITY		= (1 << 14),
	AF_HIT_SLOWMO				= (1 << 15),
	// Monolith 3D ballistics flags (Eden: bits 16+ are free)
	AF_FIREPOS					= (1 << 16), // bullets originate from the actual weapon muzzle
	AF_FIREPOS_ZOOM				= (1 << 17),
	AF_FIREDIR_THIRD_PERSON		= (1 << 18),
	AF_AIMPOS					= (1 << 19),
	AF_AIMPOS_ZOOM				= (1 << 20),
};

extern Flags32	psActorFlags;
extern BOOL		GodMode	();	
