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
#ifndef __ARCADE_INTEGRATION_H__
#define __ARCADE_INTEGRATION_H__

#include "ArcadeSdkLoader.h"
#include "ArcadeProto.h"

#include <map>
#include <string>
#include <vector>

/**
 * Arcade game SDK integration (see ThirdParty/arcade_sdk/README_SDK.md).
 *
 * Exposes The Dark Mod as an agent research environment: the SDK serves a
 * loopback endpoint through which external tooling starts "challenges", calls
 * RPCs into the game, and receives reports (events, metrics, outcomes).
 *
 * Enabled with "+set arcade_enable 1" on the command line and the SDK shared
 * library next to the executable. Everything runs on the main thread, driven by
 * Frame() once per engine frame.
 *
 * Challenges all run on the starting map of the currently installed fan mission
 * (fs_currentfm); StartChallenge reloads that map for a clean state.
 */
class CArcadeIntegration {
public:
	CArcadeIntegration();

	// idGameLocal::Init / Shutdown
	void Init();
	void Shutdown();

	// Called once per engine frame after the event loop has run.
	// insideMapLoad: called from the loading-screen pump; only polls to keep the
	// SDK liveness tick alive and defers request handling to the next real frame.
	void Frame( bool insideMapLoad );

	// idGameLocal::InitFromNewMap / MapShutdown
	void OnMapStarted();
	void OnMapShutdown();

	bool IsActive() const { return active; }

private:
	enum State {
		STATE_IDLE,
		STATE_LOADING,		// StartChallenge accepted, waiting for the map to (re)load
		STATE_RUNNING,
	};

	enum Outcome {
		OUTCOME_UNSPECIFIED = 0,
		OUTCOME_SUCCESS = 1,
		OUTCOME_FAILURE = 2,
		OUTCOME_TIMEOUT = 3,
		OUTCOME_ABORTED = 4,
	};

	struct VarValue {
		enum Kind { NONE, ENUM, INT, FLOAT, BOOL } kind;
		std::string s;
		int64_t i;
		double d;
		bool b;
		VarValue() : kind( NONE ), i( 0 ), d( 0.0 ), b( false ) {}
		std::string ToString() const;
	};
	typedef std::map<std::string, VarValue> VarMap;

	struct Attempt {
		std::string challengeId;
		std::string runId;
		uint64_t seed;
		VarMap vars;
		uint64_t pendingRequestId;	// StartChallenge request awaiting the map load
		int mapGenerationAtRequest;
		int loadIssuedMs;			// Sys_Milliseconds when the map command was queued
		int startGameTime;			// gameLocal.time when the attempt started
		int limitSeconds;
		idVec3 lastOrigin;
		float distanceUnits;
		int maxAlertSeen;
		Attempt() : seed( 0 ), pendingRequestId( 0 ), mapGenerationAtRequest( 0 ), loadIssuedMs( 0 ),
			startGameTime( 0 ), limitSeconds( 0 ), lastOrigin( vec3_origin ), distanceUnits( 0.0f ), maxAlertSeen( 0 ) {}
	};

	// --- setup
	void BuildInitRequest( ArcadeProto::Writer &out );
	void ParseMissionLocations();
	void ResolveMetricHandles();

	// --- request handling
	void PollRequests( bool insideMapLoad );
	void Dispatch( const uint8_t *data, size_t len );
	void HandleStartChallenge( uint64_t reqId, ArcadeProto::Reader req );
	void HandleStopChallenge( uint64_t reqId, ArcadeProto::Reader req );
	void HandlePing( uint64_t reqId );
	void HandleTeleportPlayer( uint64_t reqId, ArcadeProto::Reader req );
	void HandleGetPlayerState( uint64_t reqId );
	void HandleListLocations( uint64_t reqId );
	void HandleListAi( uint64_t reqId );
	void HandleExecConsoleCommand( uint64_t reqId, ArcadeProto::Reader req );
	void Respond( uint64_t reqId, const ArcadeProto::Writer &msg );
	void Fail( uint64_t reqId, const char *fmt, ... ) id_attribute( ( format( printf, 3, 4 ) ) );

	// --- challenge state machine
	void AdvanceLoading();
	void StartRunning();
	void Judge();
	void CompleteAttempt( Outcome outcome, double score, const char *detail );
	std::string ResolveInstruction( const std::string &challengeId, const VarMap &vars ) const;
	const char *ChallengeInstructionTemplate( const std::string &challengeId ) const;
	int AlertThresholdFromName( const std::string &name ) const;

	// --- observation
	void Observe();
	void ResetObservers();
	void PushMetrics();
	int MaxAiAlertIndex( idAI **culprit = nullptr ) const;
	int PlayerLoot( idPlayer *player ) const;
	const char *PlayerLocationName( idPlayer *player ) const;

	// --- reports
	double GameTimeS() const;
	void ReportChallengeStarted();
	void ReportEvent( int oneofField, const ArcadeProto::Writer &body );
	void Log( int level, const char *fmt, ... ) id_attribute( ( format( printf, 3, 4 ) ) );

	static void WriteVec3( ArcadeProto::Writer &w, int field, const idVec3 &v );
	static bool ReadVec3( ArcadeProto::Reader r, idVec3 &out );

private:
	ArcadeSdkLoader sdk;
	bool active;
	bool inFrame;

	State state;
	Attempt attempt;
	int mapGeneration;			// bumped on every InitFromNewMap
	int observedGeneration;

	// mission data gathered at Init
	idStr startingMap;
	idStrList locationNames;
	idList<idVec3> locationOrigins;

	// observer memory (per map)
	int lastLoot;
	int lastHealth;
	bool wasDead;
	bool missionCompleteReported;
	idStr lastLocation;
	std::map<std::string, int> lastAiAlert;
	int lastMetricsPushMs;

	// metric handles
	uint32_t mHealth, mLoot, mLightgem, mMaxAlert, mDistance;

	std::vector<uint8_t> pollBuf;
	std::vector<std::vector<uint8_t>> deferredRequests;
};

extern CArcadeIntegration arcadeIntegration;

#endif // __ARCADE_INTEGRATION_H__
