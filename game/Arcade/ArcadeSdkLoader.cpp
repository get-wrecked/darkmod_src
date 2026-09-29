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
#include "precompiled.h"
#include "ArcadeSdkLoader.h"

#ifdef _WIN32
	#define ARCADE_SDK_LIBNAME "arcade_sdk.dll"
#else
	#define ARCADE_SDK_LIBNAME "libarcade_sdk.so"
#endif

ArcadeSdkLoader::ArcadeSdkLoader() : handle( 0 ) {
	ClearSymbols();
}

void ArcadeSdkLoader::ClearSymbols() {
	abi_version = nullptr; version = nullptr; last_error = nullptr; init = nullptr; shutdown = nullptr;
	instance_count = nullptr; poll_request = nullptr; respond = nullptr; fail = nullptr; report = nullptr;
	metric_handle = nullptr; push_f32_metric = nullptr; submit_frame = nullptr; poll_input = nullptr; log = nullptr;
}

template<class F>
static bool BindSymbol( uintptr_t handle, const char *name, F &fn, idStr &error ) {
	void *ptr = Sys_DLL_GetProcAddress( handle, name );
	if ( !ptr ) {
		error = va( "symbol '%s' not found in " ARCADE_SDK_LIBNAME " (older SDK release?)", name );
		fn = nullptr;
		return false;
	}
	fn = reinterpret_cast<F>( ptr );
	return true;
}

bool ArcadeSdkLoader::Load() {
	if ( handle ) {
		return true;
	}

	// The library lives next to the executable (that is also where the SDK writes
	// arcade_sdk.log and looks for the arcade-sdk tool to launch the debug app).
	path = Sys_EXEPath();
	path.StripFilename();
	path.AppendPath( ARCADE_SDK_LIBNAME );

	handle = Sys_DLL_Load( path.c_str() );
	if ( !handle ) {
		error = va( "could not load '%s'", path.c_str() );
		return false;
	}

	bool ok = true;
	ok &= BindSymbol( handle, "arcade_abi_version", abi_version, error );
	ok &= BindSymbol( handle, "arcade_version", version, error );
	ok &= BindSymbol( handle, "arcade_last_error", last_error, error );
	ok &= BindSymbol( handle, "arcade_init", init, error );
	ok &= BindSymbol( handle, "arcade_shutdown", shutdown, error );
	ok &= BindSymbol( handle, "arcade_instance_count", instance_count, error );
	ok &= BindSymbol( handle, "arcade_poll_request", poll_request, error );
	ok &= BindSymbol( handle, "arcade_respond", respond, error );
	ok &= BindSymbol( handle, "arcade_fail", fail, error );
	ok &= BindSymbol( handle, "arcade_report", report, error );
	ok &= BindSymbol( handle, "arcade_metric_handle", metric_handle, error );
	ok &= BindSymbol( handle, "arcade_push_f32_metric", push_f32_metric, error );
	ok &= BindSymbol( handle, "arcade_submit_frame", submit_frame, error );
	ok &= BindSymbol( handle, "arcade_poll_input", poll_input, error );
	ok &= BindSymbol( handle, "arcade_log", log, error );
	if ( !ok ) {
		Unload();
		return false;
	}

	if ( abi_version() != ARCADE_SDK_ABI_VERSION ) {
		error = va( "ABI mismatch: library implements %u, game compiled against %u",
					abi_version(), (unsigned)ARCADE_SDK_ABI_VERSION );
		Unload();
		return false;
	}
	return true;
}

void ArcadeSdkLoader::Unload() {
	if ( handle ) {
		Sys_DLL_Unload( handle );
		handle = 0;
	}
	ClearSymbols();
}
