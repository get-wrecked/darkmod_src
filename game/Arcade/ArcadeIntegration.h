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

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class idAI;
class idPlayer;

/**
 * Arcade game SDK integration (see ThirdParty/arcade_sdk/README_SDK.md).
 *
 * Exposes The Dark Mod as an agent research environment: the SDK serves a
 * loopback endpoint through which external tooling starts "challenges", calls
 * RPCs into the game, and receives reports (events, metrics, outcomes).
 *
 * Challenges run on the two official missions shipped with the arcade build,
 * "A New Job" (map prologue9) and "Tears of St. Lucia" (map saintlucia). Both
 * are packaged as one fan mission folder (see arcade/stage_build.sh) so either
 * map can be loaded by name; StartChallenge reloads the chosen map for a clean
 * world, sets the difficulty, and judges the attempt from the game's own
 * objective system and mission statistics.
 *
 * Enabled with "+set arcade_enable 1" on the command line and the SDK shared
 * library next to the executable. Everything runs on the main thread, driven by
 * Frame() once per engine frame.
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

	// --- static mission knowledge (see ArcadeIntegration.cpp)
	struct MissionInfo {
		const char *id;			// variation value, e.g. "newjob"
		const char *map;		// map name without path/extension
		const char *display;	// "A New Job"
		const char *summary;	// one paragraph of context given to the agent
	};
	struct ObjectiveSpec {
		const char *mission;
		const char *slug;		// variation value, e.g. "steal-rubies"
		int indices[3];			// 1-based objective indices that count (alternatives, 0-terminated)
		const char *summary;	// what the agent is told
	};
	struct LocationSpec {
		const char *mission;
		const char *entity;		// info_location entity name
		const char *display;	// "the church kitchen"
	};
	struct ItemSpec {
		const char *mission;
		const char *slug;		// variation value, e.g. "church-entrance-key"
		const char *invName;	// the item's inv_name (what CInventory::GetItem matches)
		const char *display;	// "the church entrance key"
		const char *hint;		// where it is / who carries it
	};
	struct DoorSpec {
		const char *mission;
		const char *slug;		// variation value, e.g. "vestibule-door"
		const char *entity;		// CBinaryFrobMover or CFrobLock entity name
		const char *display;	// "the vestibule door inside the church"
		const char *hint;		// which key opens it / where it is
	};
	struct StartSpec {
		const char *mission;
		const char *tier;		// shared across missions: "outside", "entrance", "inside", "deep"
		const char *entity;		// info_location whose origin the player is placed at
	};

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

	enum StealthRule {
		STEALTH_ANY,		// no constraint
		STEALTH_UNSEEN,		// fail if any AI searches for or spots the player
		STEALTH_GHOST,		// fail if any AI even becomes suspicious
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
		int mission;				// index into the mission table
		int difficulty;				// 0..2
		int limitSeconds;
		StealthRule stealth;
		bool killsForbidden;		// "kills" variation: any kill fails the attempt
		std::string startTier;		// "mission-start" or a StartSpec tier
		uint64_t pendingRequestId;	// StartChallenge request awaiting the map load
		int mapGenerationAtRequest;
		int loadIssuedMs;			// Sys_Milliseconds when the map command was queued
		int startGameTime;			// gameLocal.time when the attempt started
		idVec3 lastOrigin;
		float distanceUnits;
		int maxAlertSeen;
		std::set<std::string> visited;	// explore: distinct info_location names entered
		Attempt() : seed( 0 ), mission( -1 ), difficulty( 0 ), limitSeconds( 0 ), stealth( STEALTH_ANY ), killsForbidden( false ),
			pendingRequestId( 0 ), mapGenerationAtRequest( 0 ), loadIssuedMs( 0 ), startGameTime( 0 ),
			lastOrigin( vec3_origin ), distanceUnits( 0.0f ), maxAlertSeen( 0 ) {}
	};

	// Snapshot of what the game reports about the current map; see MissionState in vendor.proto.
	struct MissionSnapshot {
		int mission;			// table index or -1
		int difficulty;
		int result;				// EMissionResult
		int lootFound, lootTotal;
		float stealthScore;
		int timesSeen, timesSuspicious, timesSearched;
		int knockouts, kills, damageReceived, pocketsPicked;
		int objectivesTotal, objectivesMandatory, objectivesMandatoryComplete;
	};

	// --- setup
	void BuildInitRequest( ArcadeProto::Writer &out );
	void DiscoverMissions();
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
	void HandleGetMissionState( uint64_t reqId );
	void Respond( uint64_t reqId, const ArcadeProto::Writer &msg );
	void Fail( uint64_t reqId, const char *fmt, ... ) id_attribute( ( format( printf, 3, 4 ) ) );

	// --- challenge state machine
	bool ValidateAttempt( const Attempt &a, std::string &error ) const;
	void AdvanceLoading();
	void StartRunning();
	void Judge();
	void CompleteAttempt( Outcome outcome, double score, const char *detail );
	std::string ResolveInstruction( const Attempt &a ) const;
	const char *ChallengeInstructionTemplate( const std::string &challengeId ) const;
	bool IsMissionChallenge( const std::string &id ) const;	// has a "mission" variation
	int MissionIndexForChallenge( const std::string &id ) const;	// for per-mission ids, else -1
	const ObjectiveSpec *FindObjectiveSpec( int mission, const std::string &slug ) const;
	const LocationSpec *FindLocationSpec( int mission, const std::string &entity ) const;
	const ItemSpec *FindItemSpec( int mission, const std::string &slug ) const;
	const DoorSpec *FindDoorSpec( int mission, const std::string &slug ) const;
	const StartSpec *FindStartSpec( int mission, const std::string &tier ) const;
	bool ChallengeHasSuffix( const std::string &id, const char *suffix ) const;
	void ApplyStartPosition();
	bool DoorUnlocked( const DoorSpec &door, bool &exists ) const;
	void BuildBenchmark( ArcadeProto::Writer &init ) const;
	static bool IsGameplayKey( int tdmKey );
	const char *LocationDisplayName( int mission, const char *entity ) const;
	bool ObjectiveSpecComplete( const ObjectiveSpec &spec, bool &failed, std::string &text ) const;
	float StealthFactor( const MissionSnapshot &s ) const;
	bool StealthViolated( const MissionSnapshot &s, StealthRule rule, const char *&why ) const;

	// --- observation
	void Observe();
	void ResetObservers();
	bool Snapshot( MissionSnapshot &out ) const;
	int CurrentMissionIndex() const;
	int MaxAiAlertIndex( idAI **culprit = nullptr ) const;
	int PlayerLoot( idPlayer *player ) const;
	const char *PlayerLocationName( idPlayer *player ) const;

	// --- frames out, input in (SDK ABI 2)
	static uint64_t CaptureClockThunk( void *user );
	static void AudioCaptureThunk( const float *samples, uint32_t frames, uint64_t firstSampleNs, bool discontinuity, void *user );
	static void FrameCaptureThunk( const unsigned char *rgba, int width, int height, int stride, uint64_t observedNs, void *user );
	void OnFrameCaptured( const unsigned char *rgba, int width, int height, int stride, uint64_t observedNs );	// backend thread
	void PollInput();
	void ApplyInputEvent( ArcadeProto::Reader inputEvent );
	static int KeyCodeToTdmKey( int keyCode );

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

	// missions found in the search path at Init: index into the mission table -> locations parsed from its map
	struct MissionRuntime {
		bool available;
		idStrList locationNames;
		idList<idVec3> locationOrigins;
		MissionRuntime() : available( false ) {}
	};
	std::vector<MissionRuntime> missions;
	int missionsAvailable;

	// observer memory (per map)
	int lastLoot;
	int lastHealth;
	bool wasDead;
	int lastMissionResult;
	int lastKnockouts;
	int lastKills;
	idStr lastLocation;
	std::vector<int> lastObjectiveStates;
	std::map<std::string, int> lastAiAlert;
	int lastMetricsPushMs;

	// metric handles
	uint32_t mHealth, mLoot, mLightgem, mMaxAlert, mDistance;
	uint32_t mStealthScore, mLootFraction, mKnockouts, mKills, mObjectivesComplete, mLocationsVisited;

	std::vector<uint8_t> pollBuf;
	std::vector<uint8_t> inputBuf;
	std::vector<std::vector<uint8_t>> deferredRequests;

	// frames: submitted from the render backend thread, counted for poll_input
	std::mutex captureMutex;			// held while a frame is being submitted; taken by Shutdown
	std::atomic<uint64_t> framesSubmitted;
	std::atomic<bool> capturing;
	std::atomic<double> captureGameTime { 0.0 };
	uint32_t frameWarnings = 0;
	bool audioDiscontinuity = true;	// audio thread only after hook registration
	std::atomic<uint64_t> audioBlocksSubmitted { 0 };
	std::atomic<uint64_t> audioBlocksDropped { 0 };
	double mouseCarryX, mouseCarryY;	// fractional mouse motion not yet injected
	int inputWarnings;
	std::string dumpFramePath;			// arcade_dumpframe: write the next captured frame here (PPM, upright)
public:
	void RequestFrameDump( const char *path );
};

extern CArcadeIntegration arcadeIntegration;

#endif // __ARCADE_INTEGRATION_H__
