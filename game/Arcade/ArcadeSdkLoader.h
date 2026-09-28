/*****************************************************************************
The Dark Mod GPL Source Code

This file is part of the The Dark Mod Source Code, originally based
on the Doom 3 GPL Source Code as published in 2011.

The Dark Mod Source Code is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version. For details, see LICENSE.TXT.

Project: The Dark Mod (http://www.thedarkmod.com/)

******************************************************************************/
#ifndef __ARCADE_SDK_LOADER_H__
#define __ARCADE_SDK_LOADER_H__

#include "arcade_sdk.h"

/**
 * Runtime binding to the arcade SDK shared library.
 *
 * The SDK is loaded with dlopen/LoadLibrary from the executable's directory so
 * that a normal player build keeps working when the library is absent: the
 * integration simply stays disabled. Function pointers mirror arcade_sdk.h.
 */
class ArcadeSdkLoader {
public:
	ArcadeSdkLoader();

	// Loads libarcade_sdk.so / arcade_sdk.dll from the executable directory.
	// Returns false (with a reason in error) if the library or a symbol is missing.
	bool Load();
	// Unloads the library. Only call after a successful arcade_shutdown (see README).
	void Unload();
	bool IsLoaded() const { return handle != 0; }
	const char *GetError() const { return error.c_str(); }
	const char *GetPath() const { return path.c_str(); }

	uint32_t ( *abi_version )( void );
	const char *( *version )( void );
	const char *( *last_error )( void );
	ArcadeStatus ( *init )( const uint8_t *, size_t );
	ArcadeStatus ( *shutdown )( void );
	ArcadeStatus ( *poll_request )( uint8_t *, size_t, size_t * );
	ArcadeStatus ( *respond )( uint64_t, const uint8_t *, size_t );
	ArcadeStatus ( *fail )( uint64_t, const char * );
	ArcadeStatus ( *report )( const uint8_t *, size_t );
	uint32_t ( *metric_handle )( const char * );
	ArcadeStatus ( *push_f32_metric )( uint32_t, float, double );
	ArcadeStatus ( *log )( int32_t, const char *, const char * );

private:
	uintptr_t handle;
	idStr error;
	idStr path;
};

#endif // __ARCADE_SDK_LOADER_H__
