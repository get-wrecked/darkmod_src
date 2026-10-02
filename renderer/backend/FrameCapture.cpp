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

#include "renderer/tr_local.h"
#include "renderer/backend/FrameBuffer.h"
#include "renderer/backend/FrameBufferManager.h"
#include <mutex>

/*
=============================================================================
Frame capture hook

Lets an external consumer (the arcade SDK integration) receive a downscaled
copy of every presented frame. RB_CaptureFrameForHook() runs in the backend
right before the buffers are swapped: it blits the default framebuffer into a
small private FBO, reads it back as RGBA8 and hands the pixels to the callback
on the backend thread. The FBO is managed by FrameBufferManager, so it survives
vid_restart like every other one.
=============================================================================
*/

static frameCaptureCallback_t captureCallback = nullptr;
static frameCaptureClock_t captureClock = nullptr;
static std::mutex captureHookMutex;
static void *captureUser = nullptr;
static int captureWidth = 0;
static int captureHeight = 0;
static FrameBuffer *captureFbo = nullptr;
static int captureFboWidth = 0;
static int captureFboHeight = 0;
static idList<byte> capturePixels;

void R_SetFrameCaptureHook( int width, int height, frameCaptureClock_t clock, frameCaptureCallback_t callback, void *user ) {
	std::lock_guard<std::mutex> lock( captureHookMutex );
	if ( !callback ) {
		captureCallback = nullptr;
		captureClock = nullptr;
		captureUser = nullptr;
		return;
	}
	captureWidth = width;
	captureHeight = height;
	captureUser = user;
	captureClock = clock;
	captureCallback = callback;
}

void RB_CaptureFrameForHook() {
	std::lock_guard<std::mutex> lock( captureHookMutex );
	frameCaptureCallback_t callback = captureCallback;
	if ( !callback || !captureClock || captureWidth <= 0 || captureHeight <= 0 || !frameBuffers || !frameBuffers->defaultFbo ) {
		return;
	}
	if ( glConfig.vidWidth <= 0 || glConfig.vidHeight <= 0 ) {
		return;
	}

	TRACE_GL_SCOPE( "FrameCapture" )
	const uint64_t observedNs = captureClock( captureUser );

	if ( !captureFbo ) {
		captureFbo = frameBuffers->CreateFromGenerator( "arcadeCapture", []( FrameBuffer *fbo ) {
			fbo->Init( captureFboWidth, captureFboHeight );
			fbo->AddColorRenderBuffer( 0, GL_RGBA8 );
		} );
	}
	if ( captureFboWidth != captureWidth || captureFboHeight != captureHeight ) {
		captureFboWidth = captureWidth;
		captureFboHeight = captureHeight;
		captureFbo->Destroy();		// regenerated with the new size on next Bind
	}

	// downscale the presented image into the capture FBO
	frameBuffers->defaultFbo->BlitToVidSize( captureFbo, GL_COLOR_BUFFER_BIT, GL_LINEAR, 0, 0, glConfig.vidWidth, glConfig.vidHeight );

	// read it back (rows bottom-up, as glReadPixels always delivers them)
	int stride = captureWidth * 4;
	capturePixels.SetNum( stride * captureHeight );
	captureFbo->Bind();
	qglPixelStorei( GL_PACK_ALIGNMENT, 1 );
	qglReadPixels( 0, 0, captureWidth, captureHeight, GL_RGBA, GL_UNSIGNED_BYTE, capturePixels.Ptr() );
	frameBuffers->defaultFbo->Bind();

	callback( capturePixels.Ptr(), captureWidth, captureHeight, stride, observedNs, captureUser );
}
