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
#include "ArcadeIntegration.h"
#include "vendor_pb.h"

#include "../Game_local.h"
#include "../Player.h"
#include "../Misc.h"
#include "../ai/AI.h"
#include "../ai/Memory.h"
#include "../Inventory/Inventory.h"
#include "../Missions/MissionManager.h"
#include "../Objectives/MissionData.h"
#include "../Objectives/Objective.h"
#include "../Objectives/ObjectiveComponent.h"
#include "../DifficultyManager.h"
#include "../../framework/Licensee.h"
#include "../../framework/KeyInput.h"
#include "../../renderer/RenderSystem.h"
#include "../../idlib/RevisionTracker.h"

#include <stdarg.h>

CArcadeIntegration arcadeIntegration;

idCVar arcade_enable( "arcade_enable", "0", CVAR_GAME | CVAR_BOOL | CVAR_INIT,
	"Load the arcade game SDK (libarcade_sdk.so / arcade_sdk.dll next to the executable) and expose the game as an agent research environment. Set on the command line: +set arcade_enable 1" );
idCVar arcade_autoReady( "arcade_autoReady", "1", CVAR_GAME | CVAR_BOOL,
	"When the arcade SDK is active, skip the 'press attack to start' screen after a map loads." );
idCVar arcade_metricsIntervalMs( "arcade_metricsIntervalMs", "100", CVAR_GAME | CVAR_INTEGER,
	"How often the arcade SDK receives metric samples, in milliseconds.", 0, 10000 );
idCVar arcade_mapLoadTimeoutSec( "arcade_mapLoadTimeoutSec", "120", CVAR_GAME | CVAR_INTEGER,
	"Fail StartChallenge if the map has not loaded after this many seconds." );
idCVar arcade_buildId( "arcade_buildId", "", CVAR_GAME | CVAR_INIT,
	"Build id registered with the arcade SDK (InitRequest.build_id). Empty = engine version + revision. Set it to the id the build is submitted under." );

// ---------------------------------------------------------------------------
// Constants from the SDK protos
// ---------------------------------------------------------------------------

static const char *CHALLENGES_SERVICE = "arcade.sdk.v1.ArcadeChallenges";
static const char *VENDOR_SERVICE = "thedarkmod.v1.Game";
static const char *VENDOR_EVENT_TYPE = "thedarkmod.v1.GameEvent";

// Report.body oneof
enum { REPORT_CHALLENGE_STARTED = 10, REPORT_CHALLENGE_COMPLETED = 11, REPORT_METRIC_SAMPLE = 12, REPORT_LOG_LINE = 13, REPORT_EVENT = 14 };
// VariationDef.kind / VariationValue.value oneof
enum { VAR_ENUM = 10, VAR_INT = 11, VAR_FLOAT = 12, VAR_BOOL = 13 };
// MetricKind
enum { METRIC_SCALAR = 1, METRIC_COUNTER = 2, METRIC_BOOL = 3 };
// GameEvent oneof (vendor.proto)
enum {
	EV_PLAYER_DIED = 1, EV_LOOT_PICKED_UP = 2, EV_AI_ALERT_CHANGED = 3, EV_LOCATION_CHANGED = 4, EV_PLAYER_DAMAGED = 5,
	EV_MISSION_COMPLETED = 6, EV_MAP_LOADED = 7, EV_OBJECTIVE_CHANGED = 8, EV_AI_KNOCKED_OUT = 9, EV_AI_KILLED = 10, EV_MISSION_ENDED = 11
};
// arcade_log levels
enum { LOG_DEBUG = 1, LOG_INFO = 2, LOG_WARN = 3, LOG_ERROR = 4 };

static const float UNITS_TO_METERS = 0.0254f;	// idTech4: 1 unit = 1 inch
static const float LIGHTGEM_MAX = 32.0f;		// DARKMOD_LG_MAX

// What the agent sees: every presented frame downscaled to this size (InitRequest.video)
static const int ARCADE_VIDEO_WIDTH = 640;
static const int ARCADE_VIDEO_HEIGHT = 360;
static const int ARCADE_TICK_HZ = 60;
// Pacing enum (arcade_sdk.proto)
enum { PACING_REAL_TIME = 1, PACING_LOCKSTEP = 2 };
// Input.events oneof / InputEvent members (arcade_sdk.proto)
enum { INEV_KEY = 1, INEV_BUTTON = 2, INEV_MOUSE_MOVE = 3, INEV_WHEEL = 4 };

// ---------------------------------------------------------------------------
// The missions of the arcade build and what an agent can be asked to do in them.
// Objective indices are the mission author's 1-based numbering (obj1_desc ... in
// the map's atdm:target_addobjectives entity).
// ---------------------------------------------------------------------------

static const CArcadeIntegration::MissionInfo MISSIONS[] = {
	{ "newjob", "prologue9", "A New Job",
	  "You are Corbin, a thief in the city of Bridgeport. Tonight you plan to rob Lord Rothwick, a nobleman staying at Canonbury Tavern with a purse of expensive rubies, and afterwards meet a contact in the courtyard south of the tavern. "
	  "You start in the back alleys near your rented room. The City Watch patrols the main streets, the tavern entrance is guarded, and the tavern staff and guests are inside. Read notes and journals you find: they tell you where things are." },
	{ "stlucia", "saintlucia", "Tears of St. Lucia",
	  "You are Corbin, a thief. A client wants the relic that makes the statue of St. Lucia weep; it is kept in an ornate box inside a Builder church. The job must look like an accident and ordinary theft: damage the statue, steal loot, and do not kill anyone. "
	  "You start on the streets outside at night. The church is patrolled by armed Builder guards; there are other ways in than the front door, including the sewers. Keys are carried by guards and can be pickpocketed." },
};
static const int NUM_MISSIONS = sizeof( MISSIONS ) / sizeof( MISSIONS[0] );

static const CArcadeIntegration::ObjectiveSpec OBJECTIVES[] = {
	{ "newjob", "enter-tavern", { 1, 0, 0 }, "Find your way to Canonbury Tavern and get inside." },
	{ "newjob", "find-clue", { 2, 3, 0 }, "Search Lord Rothwick's room in the tavern and read his journal to learn where the rubies are kept." },
	{ "newjob", "steal-rubies", { 4, 0, 0 }, "Get Lord Rothwick's rubies from the innkeeper's safe. His journal, in his room upstairs, says where they are; the safe key is somewhere in the tavern." },
	{ "newjob", "meet-contact", { 7, 0, 0 }, "Meet your contact in the south-east corner of Royston Court, the courtyard south of Canonbury Tavern. He only turns up once you have the rubies." },
	{ "stlucia", "steal-relic", { 1, 0, 0 }, "Find the ornate box holding the relic that makes the statue of St. Lucia weep, and take it." },
	{ "stlucia", "damage-statue", { 4, 0, 0 }, "Find a way to damage the statue of St. Lucia so it looks like an accident." },
	{ "stlucia", "loot-quota", { 5, 6, 7 }, "Steal enough loot to make the job look like ordinary theft (the amount depends on difficulty; check your objectives)." },
	{ "stlucia", "escape", { 9, 0, 0 }, "Get back to where you started to leave the area. This only counts once the relic is taken, the statue is damaged and the loot quota is met." },
};
static const int NUM_OBJECTIVES = sizeof( OBJECTIVES ) / sizeof( OBJECTIVES[0] );

static const CArcadeIntegration::LocationSpec LOCATIONS[] = {
	{ "newjob", "inn", "the common room of Canonbury Tavern" },
	{ "newjob", "kitchen", "the tavern kitchen" },
	{ "newjob", "kitchen_up", "the loft above the tavern kitchen" },
	{ "newjob", "room_down", "the tavern's downstairs back room" },
	{ "stlucia", "main_road", "the main road outside" },
	{ "stlucia", "church_yard", "the church yard" },
	{ "stlucia", "church_entrance_front", "the front entrance of the church" },
	{ "stlucia", "church_interior", "the nave inside the church" },
	{ "stlucia", "church_lucia", "the statue of St. Lucia inside the church" },
	{ "stlucia", "church_reading_room", "the church reading room" },
	{ "stlucia", "kitchen", "the church kitchen" },
	{ "stlucia", "church_downstairs", "the lower floor of the church" },
	{ "stlucia", "basement_main", "the church basement" },
	{ "stlucia", "acolyte_room", "the acolyte's room" },
	{ "stlucia", "generator_room", "the generator room under the church" },
	{ "stlucia", "secret_sewer", "the sewer" },
	{ "stlucia", "beggar_room", "the beggar's room" },
	{ "stlucia", "vent_1", "the ventilation shaft above the church" },
};
static const int NUM_LOCATIONS = sizeof( LOCATIONS ) / sizeof( LOCATIONS[0] );

static const char *DIFFICULTY_NAMES[] = { "easy", "medium", "hard" };
static const char *STEALTH_NAMES[] = { "any", "unseen", "ghost" };

// Challenge ids
static const char *CH_COMPLETE_MISSION = "complete-mission";
static const char *CH_STEAL_LOOT = "steal-loot";
static const char *CH_KNOCKOUT = "knockout";
static const char *CH_EXPLORE = "explore";
static const char *CH_OBJECTIVE_SUFFIX = "-objective";	// newjob-objective, stlucia-objective
static const char *CH_REACH_SUFFIX = "-reach";			// newjob-reach, stlucia-reach

static const char *CONTROLS_HINT =
	"Controls: WASD to move, mouse to look, Shift to run, Ctrl to creep, C to crouch, Space to jump or mantle onto ledges. "
	"Left mouse button attacks with the selected weapon (the blackjack knocks out an unaware guard hit from behind; the sword kills). "
	"Right mouse button is 'frob': pick up items and loot, open doors and chests, read notes, use keys and switches. "
	"O shows your objectives, I opens the inventory, F/Q/E lean. The light gem at the bottom of the screen shows how visible you are: stay in the shadows, avoid guards' line of sight, and move slowly on noisy floors.";

// ---------------------------------------------------------------------------
// "arcade_probe <entityName>": diagnostic for objective volumes (console)
// ---------------------------------------------------------------------------

static void Arcade_Probe_f( const idCmdArgs &args ) {
	idPlayer *player = gameLocal.GetLocalPlayer();
	if ( !player ) { common->Printf( "arcade_probe: no player\n" ); return; }
	common->Printf( "player '%s' origin %s m_bIsObjective=%d clipModel=%p contents=%d\n", player->GetName(), player->GetPhysics()->GetOrigin().ToString(),
		(int)player->m_bIsObjective, (void *)player->GetPhysics()->GetClipModel(), player->GetPhysics()->GetContents() );
	if ( args.Argc() < 2 ) return;
	idEntity *ent = gameLocal.FindEntity( args.Argv( 1 ) );
	if ( !ent ) { common->Printf( "arcade_probe: entity '%s' not found\n", args.Argv( 1 ) ); return; }
	const idBounds &b = ent->GetPhysics()->GetAbsBounds();
	common->Printf( "entity '%s' class %s origin %s absBounds %s .. %s thinkFlags=%d clipModel=%p\n", ent->GetName(), ent->GetType()->classname,
		ent->GetPhysics()->GetOrigin().ToString(), b[0].ToString(), b[1].ToString(), ent->thinkFlags, (void *)ent->GetPhysics()->GetClipModel() );
	common->Printf( "player origin inside absBounds: %d\n", (int)b.ContainsPoint( player->GetPhysics()->GetOrigin() ) );
	// replicate CObjectiveLocation::Think's test against the entity's inline brush model
	const char *modelName = ent->spawnArgs.GetString( "model" );
	cmHandle_t h = collisionModelManager->LoadModel( modelName, false );
	idBounds mb;
	mb.Clear();
	if ( h ) collisionModelManager->GetModelBounds( h, mb );
	int contents = 0;
	if ( h ) collisionModelManager->GetModelContents( h, contents );
	int inside = h ? gameLocal.clip.ContentsModel( player->GetPhysics()->GetOrigin(), player->GetPhysics()->GetClipModel(), player->GetPhysics()->GetAxis(), -1,
		h, ent->GetPhysics()->GetOrigin(), ent->GetPhysics()->GetAxis() ) : -1;
	common->Printf( "collision model '%s': handle %d bounds %s .. %s contents 0x%x; player ContentsModel test = 0x%x\n", modelName, (int)h, mb[0].ToString(), mb[1].ToString(), contents, inside );
	idClip_ClipModelList list;
	int n = gameLocal.clip.ClipModelsTouchingBounds( b, -1, list );
	common->Printf( "%d clip models touch the bounds:\n", n );
	for ( int i = 0; i < n; i++ ) {
		idEntity *e = list[i]->GetEntity();
		common->Printf( "  %s (trace=%d, isObjective=%d)\n", e ? e->GetName() : "<null>", (int)list[i]->IsTraceModel(), e ? (int)e->m_bIsObjective : -1 );
	}
}

// "arcade_dumpframe <file.ppm>": writes the next frame handed to the SDK, as the agent
// would see it (upright, RGB), to check orientation and colour order.
static void Arcade_DumpFrame_f( const idCmdArgs &args ) {
	if ( args.Argc() < 2 ) { common->Printf( "usage: arcade_dumpframe <file.ppm>\n" ); return; }
	arcadeIntegration.RequestFrameDump( args.Argv( 1 ) );
}

// ---------------------------------------------------------------------------

std::string CArcadeIntegration::VarValue::ToString() const {
	switch ( kind ) {
	case ENUM: return s;
	case INT: return std::string( va( "%lld", (long long)i ) );
	case FLOAT: return std::string( va( "%g", d ) );
	case BOOL: return b ? "true" : "false";
	default: return std::string();
	}
}

CArcadeIntegration::CArcadeIntegration() :
	active( false ), inFrame( false ), state( STATE_IDLE ), mapGeneration( 0 ), observedGeneration( -1 ), missionsAvailable( 0 ),
	lastLoot( 0 ), lastHealth( 0 ), wasDead( false ), lastMissionResult( 0 ), lastKnockouts( 0 ), lastKills( 0 ), lastMetricsPushMs( 0 ),
	mHealth( 0 ), mLoot( 0 ), mLightgem( 0 ), mMaxAlert( 0 ), mDistance( 0 ),
	mStealthScore( 0 ), mLootFraction( 0 ), mKnockouts( 0 ), mKills( 0 ), mObjectivesComplete( 0 ), mLocationsVisited( 0 ),
	framesSubmitted( 0 ), capturing( false ), mouseCarryX( 0.0 ), mouseCarryY( 0.0 ), inputWarnings( 0 )
{}

// ===========================================================================
// Lifecycle
// ===========================================================================

void CArcadeIntegration::Init() {
	if ( active ) {
		return;
	}
	if ( !arcade_enable.GetBool() ) {
		return;
	}

	common->Printf( "--------- Arcade SDK ----------\n" );
	cmdSystem->AddCommand( "arcade_probe", Arcade_Probe_f, CMD_FL_GAME, "arcade SDK diagnostic: player objective flag and what an objective volume's clip query sees" );
	cmdSystem->AddCommand( "arcade_dumpframe", Arcade_DumpFrame_f, CMD_FL_GAME, "arcade SDK diagnostic: write the next frame submitted to the SDK as a PPM file" );
	if ( !sdk.Load() ) {
		common->Warning( "Arcade SDK disabled: %s", sdk.GetError() );
		return;
	}
	common->Printf( "Loaded %s (version %s)\n", sdk.GetPath(), sdk.version() );

	DiscoverMissions();
	if ( missionsAvailable == 0 ) {
		common->Warning( "Arcade SDK: none of the arcade missions (%s) is in the search path; launch with +set fs_currentfm arcade (see arcade/stage_build.sh). Challenges cannot start.",
			va( "%s, %s", MISSIONS[0].map, MISSIONS[1].map ) );
	}

	ArcadeProto::Writer init;
	BuildInitRequest( init );
	ArcadeStatus status = sdk.init( init.Data(), init.Size() );
	if ( status != ARCADE_STATUS_OK ) {
		common->Warning( "Arcade SDK: arcade_init failed (%d): %s", (int)status, sdk.last_error() );
		sdk.Unload();
		return;
	}

	active = true;
	ResolveMetricHandles();
	pollBuf.resize( ARCADE_MAX_REQUEST_BYTES );
	inputBuf.resize( ARCADE_MAX_INPUT_BYTES );

	uint32_t instances = sdk.instance_count();
	if ( instances != 1 ) {
		common->Warning( "Arcade SDK: %u instances requested but this build hosts one world per process; only instance 0 is served", instances );
	}

	// frames: every presented frame, downscaled, goes to the SDK from the render backend
	framesSubmitted = 0;
	capturing = true;
	R_SetFrameCaptureHook( ARCADE_VIDEO_WIDTH, ARCADE_VIDEO_HEIGHT, FrameCaptureThunk, this );

	Log( LOG_INFO, "The Dark Mod arcade integration ready (%d of %d missions available, %dx%d frames, real-time pacing)",
		missionsAvailable, NUM_MISSIONS, ARCADE_VIDEO_WIDTH, ARCADE_VIDEO_HEIGHT );
	common->Printf( "-------------------------------\n" );
}

void CArcadeIntegration::Shutdown() {
	if ( !active ) {
		return;
	}
	if ( state == STATE_RUNNING ) {
		CompleteAttempt( OUTCOME_ABORTED, 0.0, "game shutting down" );
	} else if ( state == STATE_LOADING ) {
		Fail( attempt.pendingRequestId, "game shutting down" );
		state = STATE_IDLE;
	}

	// stop the backend from submitting frames, and wait for one in flight to finish
	R_SetFrameCaptureHook( 0, 0, nullptr, nullptr );
	capturing = false;
	std::lock_guard<std::mutex> captureLock( captureMutex );

	ArcadeStatus status = sdk.shutdown();
	active = false;
	if ( status == ARCADE_STATUS_OK ) {
		sdk.Unload();
	} else {
		// SHUTDOWN_INCOMPLETE: a thread is still inside the library. Never unload it
		// in that case; the process is exiting anyway.
		common->Warning( "Arcade SDK: arcade_shutdown returned %d: %s (library left loaded)", (int)status, sdk.last_error() );
	}
}

void CArcadeIntegration::OnMapStarted() {
	mapGeneration++;
}

void CArcadeIntegration::OnMapShutdown() {
	if ( !active ) {
		return;
	}
	if ( state == STATE_RUNNING ) {
		CompleteAttempt( OUTCOME_ABORTED, 0.0, "map unloaded" );
	}
}

// ===========================================================================
// Registration
// ===========================================================================

void CArcadeIntegration::DiscoverMissions() {
	missions.assign( NUM_MISSIONS, MissionRuntime() );
	missionsAvailable = 0;
	for ( int m = 0; m < NUM_MISSIONS; m++ ) {
		MissionRuntime &rt = missions[m];
		idMapFile mapFile;
		if ( !mapFile.Parse( va( "maps/%s", MISSIONS[m].map ) ) ) {
			common->Printf( "Arcade SDK: mission '%s' (maps/%s) not found\n", MISSIONS[m].id, MISSIONS[m].map );
			continue;
		}
		for ( int i = 0; i < mapFile.GetNumEntities(); i++ ) {
			const idDict &args = mapFile.GetEntity( i )->epairs;
			if ( idStr::Icmp( args.GetString( "classname" ), "info_location" ) != 0 ) {
				continue;
			}
			const char *name = args.GetString( "name" );
			if ( !name[0] || rt.locationNames.FindIndex( name ) >= 0 ) {
				continue;
			}
			rt.locationNames.Append( name );
			rt.locationOrigins.Append( args.GetVector( "origin" ) );
		}
		rt.available = true;
		missionsAvailable++;
		common->Printf( "Arcade SDK: mission '%s' (%s): %d locations\n", MISSIONS[m].id, MISSIONS[m].display, rt.locationNames.Num() );
	}
}

namespace {
	using ArcadeProto::Writer;

	void AddEnumVar( Writer &challenge, const char *name, const char *desc, const idStrList &values, const char *def ) {
		Writer kind;
		for ( int i = 0; i < values.Num(); i++ ) {
			kind.String( 1, values[i].c_str() );
		}
		kind.String( 2, def );
		Writer var;
		var.String( 1, name );
		var.String( 2, desc );
		var.PutMessageAlways( VAR_ENUM, kind );
		challenge.PutMessage( 5, var );
	}

	void AddIntVar( Writer &challenge, const char *name, const char *desc, int64_t mn, int64_t mx, int64_t def, int64_t step ) {
		Writer kind;
		kind.Int64( 1, mn );
		kind.Int64( 2, mx );
		kind.Int64( 3, def );
		kind.Int64( 4, step );
		Writer var;
		var.String( 1, name );
		var.String( 2, desc );
		var.PutMessageAlways( VAR_INT, kind );
		challenge.PutMessage( 5, var );
	}

	void AddMetricDef( Writer &init, const char *name, int kind, const char *unit, const char *desc ) {
		Writer m;
		m.String( 1, name );
		m.Enum( 2, kind );
		m.String( 3, unit );
		m.String( 4, desc );
		init.PutMessage( 5, m );
	}

	void AddChallenge( Writer &init, Writer &c, const char *id, const char *display, const char *instruction, const char *description, const char *metrics[], int timeoutS ) {
		c.String( 1, id );
		c.String( 2, display );
		c.String( 3, instruction );
		c.String( 4, description );
		for ( int i = 0; metrics[i]; i++ ) {
			c.String( 6, metrics[i] );
		}
		c.UInt32( 7, timeoutS );
		init.PutMessage( 4, c );
	}

	const char *DIFFICULTY_DESC = "Mission difficulty: changes guard count and placement, loot targets and some objectives";
	const char *MINUTES_DESC = "Time limit for the attempt, in minutes";
	const char *STEALTH_DESC = "any: no constraint. unseen: fails if a guard searches for or spots the player. ghost: fails if a guard so much as becomes suspicious";
}

bool CArcadeIntegration::IsMissionChallenge( const std::string &id ) const {
	return id == CH_COMPLETE_MISSION || id == CH_STEAL_LOOT || id == CH_KNOCKOUT || id == CH_EXPLORE;
}

int CArcadeIntegration::MissionIndexForChallenge( const std::string &id ) const {
	for ( int m = 0; m < NUM_MISSIONS; m++ ) {
		std::string prefix = MISSIONS[m].id;
		if ( id == prefix + CH_OBJECTIVE_SUFFIX || id == prefix + CH_REACH_SUFFIX ) {
			return m;
		}
	}
	return -1;
}

const char *CArcadeIntegration::ChallengeInstructionTemplate( const std::string &id ) const {
	if ( id == CH_COMPLETE_MISSION ) return "Play the mission '{mission}' on {difficulty} difficulty and complete every mandatory objective within {minutes} minutes.";
	if ( id == CH_STEAL_LOOT ) return "In the mission '{mission}' ({difficulty} difficulty), steal at least {percent}% of all the loot in the level within {minutes} minutes. Stealth rule: {stealth}.";
	if ( id == CH_KNOCKOUT ) return "In the mission '{mission}' ({difficulty} difficulty), knock out {count} guard(s) with the blackjack within {minutes} minutes without killing anyone.";
	if ( id == CH_EXPLORE ) return "Explore the mission '{mission}' ({difficulty} difficulty) for {minutes} minutes and visit as many distinct areas as you can.";
	if ( MissionIndexForChallenge( id ) >= 0 ) {
		if ( id.size() > strlen( CH_REACH_SUFFIX ) && id.compare( id.size() - strlen( CH_REACH_SUFFIX ), std::string::npos, CH_REACH_SUFFIX ) == 0 ) {
			return "Find your way to {location} within {minutes} minutes ({difficulty} difficulty). Stealth rule: {stealth}.";
		}
		return "In this mission ({difficulty} difficulty), complete the objective '{objective}' within {minutes} minutes.";
	}
	return "";
}

void CArcadeIntegration::BuildInitRequest( ArcadeProto::Writer &init ) {
	init.String( 1, "thedarkmod" );
	if ( arcade_buildId.GetString()[0] ) {
		init.String( 2, arcade_buildId.GetString() );
	} else {
		int revision = RevisionTracker::Instance().GetHighestRevision();
		init.String( 2, revision > 0 ? va( "%s r%d", ENGINE_VERSION, revision ) : ENGINE_VERSION );
	}
	init.UInt32( 3, ARCADE_SDK_ABI_VERSION );

	idStrList missionIds, difficulties, stealth;
	for ( int m = 0; m < NUM_MISSIONS; m++ ) {
		if ( missions[m].available ) {
			missionIds.Append( MISSIONS[m].id );
		}
	}
	if ( missionIds.Num() == 0 ) {
		missionIds.Append( MISSIONS[0].id );	// registration must be well-formed even if nothing can start
	}
	for ( int i = 0; i < 3; i++ ) difficulties.Append( DIFFICULTY_NAMES[i] );
	for ( int i = 0; i < 3; i++ ) stealth.Append( STEALTH_NAMES[i] );

	// --- complete-mission
	{
		static const char *metrics[] = { "mission/objectives_complete", "mission/stealth_score", "mission/loot_fraction", "ai/knockouts", "ai/kills", "player/health", nullptr };
		Writer c;
		AddEnumVar( c, "mission", "Which mission to play", missionIds, missionIds[0].c_str() );
		AddEnumVar( c, "difficulty", DIFFICULTY_DESC, difficulties, "easy" );
		AddIntVar( c, "minutes", MINUTES_DESC, 10, 120, 45, 5 );
		AddChallenge( init, c, CH_COMPLETE_MISSION, "Complete the mission", ChallengeInstructionTemplate( CH_COMPLETE_MISSION ),
			"The full game loop: the mission's own objectives judge the attempt (mission complete = success, mission failed or death = failure). "
			"Requires exploration, reading in-game notes, stealth around patrols, lock/key puzzles and long-horizon planning. Score rewards success and penalises alerts raised.",
			metrics, 7200 );
	}
	// --- per-mission objective challenges
	for ( int m = 0; m < NUM_MISSIONS; m++ ) {
		if ( !missions[m].available ) continue;
		idStrList slugs;
		for ( int i = 0; i < NUM_OBJECTIVES; i++ ) {
			if ( idStr::Cmp( OBJECTIVES[i].mission, MISSIONS[m].id ) == 0 ) slugs.Append( OBJECTIVES[i].slug );
		}
		static const char *metrics[] = { "mission/objectives_complete", "mission/stealth_score", "ai/knockouts", "ai/kills", nullptr };
		Writer c;
		AddEnumVar( c, "objective", "Which of the mission's objectives to complete", slugs, slugs[0].c_str() );
		AddEnumVar( c, "difficulty", DIFFICULTY_DESC, difficulties, "easy" );
		AddIntVar( c, "minutes", MINUTES_DESC, 5, 60, 20, 5 );
		std::string id = std::string( MISSIONS[m].id ) + CH_OBJECTIVE_SUFFIX;
		AddChallenge( init, c, id.c_str(), va( "%s: complete an objective", MISSIONS[m].display ), ChallengeInstructionTemplate( id ),
			va( "One objective of '%s', judged by the game's objective system. The objectives build on each other (e.g. the rubies can only be found after reading the journal), so later ones imply the earlier ones.", MISSIONS[m].display ),
			metrics, 3600 );
	}
	// --- per-mission reach-location challenges
	for ( int m = 0; m < NUM_MISSIONS; m++ ) {
		if ( !missions[m].available ) continue;
		idStrList entities;
		for ( int i = 0; i < NUM_LOCATIONS; i++ ) {
			if ( idStr::Cmp( LOCATIONS[i].mission, MISSIONS[m].id ) == 0 && missions[m].locationNames.FindIndex( LOCATIONS[i].entity ) >= 0 ) {
				entities.Append( LOCATIONS[i].entity );
			}
		}
		if ( entities.Num() == 0 ) continue;
		static const char *metrics[] = { "player/distance_travelled_m", "mission/stealth_score", "ai/max_alert_index", nullptr };
		Writer c;
		AddEnumVar( c, "location", "Target area (an info_location of the map; the instruction names it in plain words)", entities, entities[0].c_str() );
		AddEnumVar( c, "stealth", STEALTH_DESC, stealth, "any" );
		AddEnumVar( c, "difficulty", DIFFICULTY_DESC, difficulties, "easy" );
		AddIntVar( c, "minutes", MINUTES_DESC, 2, 30, 10, 1 );
		std::string id = std::string( MISSIONS[m].id ) + CH_REACH_SUFFIX;
		AddChallenge( init, c, id.c_str(), va( "%s: reach a place", MISSIONS[m].display ), ChallengeInstructionTemplate( id ),
			va( "Navigation and infiltration in '%s' from the mission start. Success when the player stands in the target area; the stealth rule turns it into a ghosting test.", MISSIONS[m].display ),
			metrics, 1800 );
	}
	// --- steal-loot
	{
		static const char *metrics[] = { "mission/loot_fraction", "player/loot", "mission/stealth_score", nullptr };
		Writer c;
		AddEnumVar( c, "mission", "Which mission to play", missionIds, missionIds[0].c_str() );
		AddIntVar( c, "percent", "Share of the level's total loot value to collect", 10, 100, 30, 10 );
		AddEnumVar( c, "stealth", STEALTH_DESC, stealth, "any" );
		AddEnumVar( c, "difficulty", DIFFICULTY_DESC, difficulties, "easy" );
		AddIntVar( c, "minutes", MINUTES_DESC, 5, 60, 20, 5 );
		AddChallenge( init, c, CH_STEAL_LOOT, "Steal loot", ChallengeInstructionTemplate( CH_STEAL_LOOT ),
			"Thievery: find and frob valuables (coins, goblets, plates, jewellery, paintings) spread through the level. Judged against the game's total loot count for the map at this difficulty; score is the fraction of the target reached.",
			metrics, 3600 );
	}
	// --- knockout
	{
		static const char *metrics[] = { "ai/knockouts", "ai/kills", "mission/stealth_score", "player/health", nullptr };
		Writer c;
		AddEnumVar( c, "mission", "Which mission to play", missionIds, missionIds[0].c_str() );
		AddIntVar( c, "count", "Guards to knock out", 1, 4, 1, 1 );
		AddEnumVar( c, "difficulty", DIFFICULTY_DESC, difficulties, "easy" );
		AddIntVar( c, "minutes", MINUTES_DESC, 5, 40, 15, 5 );
		AddChallenge( init, c, CH_KNOCKOUT, "Knock out guards", ChallengeInstructionTemplate( CH_KNOCKOUT ),
			"Melee stealth: select the blackjack (weapon 1) and hit an unaware guard from behind. Judged by the mission statistics: success when the knockout count is reached; any kill fails the attempt.",
			metrics, 2400 );
	}
	// --- explore
	{
		static const char *metrics[] = { "explore/locations_visited", "player/distance_travelled_m", "mission/stealth_score", nullptr };
		Writer c;
		AddEnumVar( c, "mission", "Which mission to play", missionIds, missionIds[0].c_str() );
		AddEnumVar( c, "difficulty", DIFFICULTY_DESC, difficulties, "easy" );
		AddIntVar( c, "minutes", MINUTES_DESC, 3, 30, 10, 1 );
		AddChallenge( init, c, CH_EXPLORE, "Explore", ChallengeInstructionTemplate( CH_EXPLORE ),
			"Coverage baseline: always succeeds at the time limit unless the player dies. Score is the fraction of the map's named areas the player entered.",
			metrics, 1800 );
	}

	AddMetricDef( init, "player/health", METRIC_SCALAR, "hp", "Player health, 100 = full" );
	AddMetricDef( init, "player/loot", METRIC_COUNTER, "value", "Total loot value carried" );
	AddMetricDef( init, "player/lightgem", METRIC_SCALAR, "ratio", "How lit the player is, 0 dark .. 1 fully lit" );
	AddMetricDef( init, "ai/max_alert_index", METRIC_SCALAR, "index", "Highest alert index of any living AI right now (0 relaxed .. 5 combat)" );
	AddMetricDef( init, "player/distance_travelled_m", METRIC_COUNTER, "m", "Distance the player has moved during the attempt" );
	AddMetricDef( init, "mission/stealth_score", METRIC_COUNTER, "points", "TDM stealth score: alerts weighted by seriousness plus 5 per sighting; 0 is perfect" );
	AddMetricDef( init, "mission/loot_fraction", METRIC_SCALAR, "ratio", "Loot found / loot available in the map" );
	AddMetricDef( init, "mission/objectives_complete", METRIC_COUNTER, "count", "Mandatory objectives completed" );
	AddMetricDef( init, "ai/knockouts", METRIC_COUNTER, "count", "AI knocked out this map" );
	AddMetricDef( init, "ai/kills", METRIC_COUNTER, "count", "AI killed this map" );
	AddMetricDef( init, "explore/locations_visited", METRIC_COUNTER, "count", "Distinct named areas entered during the attempt" );

	init.Bytes( 6, arcade_vendor_pb, arcade_vendor_pb_len );
	init.String( 7, CHALLENGES_SERVICE );
	init.String( 7, VENDOR_SERVICE );
	init.String( 8, VENDOR_EVENT_TYPE );

	// ABI 2: pacing, frame size, tick rate, instances. The engine runs on the wall
	// clock and one process hosts one world.
	init.Enum( 10, PACING_REAL_TIME );
	{
		Writer video;	// VideoSize { 1 width, 2 height }
		video.UInt32( 1, ARCADE_VIDEO_WIDTH );
		video.UInt32( 2, ARCADE_VIDEO_HEIGHT );
		init.PutMessageAlways( 11, video );
	}
	init.UInt32( 12, ARCADE_TICK_HZ );
	init.UInt32( 13, 1 );

	// CoordinateSystem { 1 up, 2 handedness, 3 euler_order }: idTech4 is Z-up,
	// right-handed, and idAngles apply yaw, then pitch, then roll.
	{
		Writer cs;
		cs.Enum( 1, 5 );	// AXIS_POS_Z
		cs.Enum( 2, 2 );	// HANDEDNESS_RIGHT
		cs.Enum( 3, 1 );	// EULER_ORDER_YAW_PITCH_ROLL
		init.PutMessageAlways( 9, cs );
	}
}

void CArcadeIntegration::ResolveMetricHandles() {
	mHealth = sdk.metric_handle( "player/health" );
	mLoot = sdk.metric_handle( "player/loot" );
	mLightgem = sdk.metric_handle( "player/lightgem" );
	mMaxAlert = sdk.metric_handle( "ai/max_alert_index" );
	mDistance = sdk.metric_handle( "player/distance_travelled_m" );
	mStealthScore = sdk.metric_handle( "mission/stealth_score" );
	mLootFraction = sdk.metric_handle( "mission/loot_fraction" );
	mKnockouts = sdk.metric_handle( "ai/knockouts" );
	mKills = sdk.metric_handle( "ai/kills" );
	mObjectivesComplete = sdk.metric_handle( "mission/objectives_complete" );
	mLocationsVisited = sdk.metric_handle( "explore/locations_visited" );
}

// ===========================================================================
// Frame
// ===========================================================================

void CArcadeIntegration::Frame( bool insideMapLoad ) {
	if ( !active || inFrame ) {
		return;
	}
	inFrame = true;

	PollRequests( insideMapLoad );

	if ( !insideMapLoad ) {
		PollInput();
		Observe();
		if ( state == STATE_LOADING ) {
			AdvanceLoading();
		}
		if ( state == STATE_RUNNING ) {
			Judge();
		}
	}

	inFrame = false;
}

void CArcadeIntegration::PollRequests( bool insideMapLoad ) {
	// Drain the queue every frame: the poll is the SDK's liveness signal.
	for ( int guard = 0; guard < 1024; guard++ ) {
		size_t n = 0;
		ArcadeStatus status = sdk.poll_request( pollBuf.data(), pollBuf.size(), &n );
		if ( status == ARCADE_STATUS_BUFFER_TOO_SMALL ) {
			pollBuf.resize( n );
			continue;
		}
		if ( status != ARCADE_STATUS_OK ) {
			Log( LOG_WARN, "arcade_poll_request failed (%d): %s", (int)status, sdk.last_error() );
			break;
		}
		if ( n == 0 ) {
			break;
		}
		if ( insideMapLoad ) {
			// Game state is being torn down / built up: answer later, on a real frame.
			deferredRequests.push_back( std::vector<uint8_t>( pollBuf.begin(), pollBuf.begin() + n ) );
		} else {
			Dispatch( pollBuf.data(), n );
		}
	}

	if ( !insideMapLoad && !deferredRequests.empty() ) {
		std::vector<std::vector<uint8_t>> pending;
		pending.swap( deferredRequests );
		for ( size_t i = 0; i < pending.size(); i++ ) {
			Dispatch( pending[i].data(), pending[i].size() );
		}
	}
}

void CArcadeIntegration::Dispatch( const uint8_t *data, size_t len ) {
	// RpcRequest { 1 request_id, 2 service, 3 method, 4 timeout_ms, 5 request, 6 instance }
	ArcadeProto::Reader r( data, len );
	uint64_t reqId = 0;
	uint64_t instance = 0;
	std::string service, method;
	const uint8_t *body = nullptr;
	size_t bodyLen = 0;

	int field;
	ArcadeProto::WireType wt;
	while ( r.Next( field, wt ) ) {
		switch ( field ) {
		case 1: reqId = r.Varint(); break;
		case 2: service = r.String(); break;
		case 3: method = r.String(); break;
		case 5: r.Bytes( body, bodyLen ); break;
		case 6: instance = r.Varint(); break;
		default: r.Skip( wt ); break;
		}
	}
	if ( r.HadError() || reqId == 0 ) {
		Log( LOG_WARN, "undecodable RpcRequest (%zu bytes)", len );
		return;
	}
	if ( instance != 0 ) {
		Fail( reqId, "this build hosts a single world; instance %llu does not exist", (unsigned long long)instance );
		return;
	}

	ArcadeProto::Reader req( body, bodyLen );
	if ( service == CHALLENGES_SERVICE ) {
		if ( method == "StartChallenge" ) { HandleStartChallenge( reqId, req ); return; }
		if ( method == "StopChallenge" ) { HandleStopChallenge( reqId, req ); return; }
		if ( method == "Ping" ) { HandlePing( reqId ); return; }
	} else if ( service == VENDOR_SERVICE ) {
		if ( method == "TeleportPlayer" ) { HandleTeleportPlayer( reqId, req ); return; }
		if ( method == "GetPlayerState" ) { HandleGetPlayerState( reqId ); return; }
		if ( method == "ListLocations" ) { HandleListLocations( reqId ); return; }
		if ( method == "ListAi" ) { HandleListAi( reqId ); return; }
		if ( method == "ExecConsoleCommand" ) { HandleExecConsoleCommand( reqId, req ); return; }
		if ( method == "GetMissionState" ) { HandleGetMissionState( reqId ); return; }
	}
	Fail( reqId, "unhandled method %s/%s", service.c_str(), method.c_str() );
}

void CArcadeIntegration::Respond( uint64_t reqId, const ArcadeProto::Writer &msg ) {
	ArcadeStatus status = sdk.respond( reqId, msg.Data(), msg.Size() );
	if ( status != ARCADE_STATUS_OK ) {
		Log( LOG_WARN, "arcade_respond(%llu) failed (%d): %s", (unsigned long long)reqId, (int)status, sdk.last_error() );
	}
}

void CArcadeIntegration::Fail( uint64_t reqId, const char *fmt, ... ) {
	char text[1024];
	va_list args;
	va_start( args, fmt );
	idStr::vsnPrintf( text, sizeof( text ), fmt, args );
	va_end( args );
	Log( LOG_WARN, "request %llu failed: %s", (unsigned long long)reqId, text );
	sdk.fail( reqId, text );
}

// ===========================================================================
// ArcadeChallenges service
// ===========================================================================

const CArcadeIntegration::ObjectiveSpec *CArcadeIntegration::FindObjectiveSpec( int mission, const std::string &slug ) const {
	if ( mission < 0 ) return nullptr;
	for ( int i = 0; i < NUM_OBJECTIVES; i++ ) {
		if ( idStr::Cmp( OBJECTIVES[i].mission, MISSIONS[mission].id ) == 0 && slug == OBJECTIVES[i].slug ) {
			return &OBJECTIVES[i];
		}
	}
	return nullptr;
}

const CArcadeIntegration::LocationSpec *CArcadeIntegration::FindLocationSpec( int mission, const std::string &entity ) const {
	if ( mission < 0 ) return nullptr;
	for ( int i = 0; i < NUM_LOCATIONS; i++ ) {
		if ( idStr::Cmp( LOCATIONS[i].mission, MISSIONS[mission].id ) == 0 && entity == LOCATIONS[i].entity ) {
			return &LOCATIONS[i];
		}
	}
	return nullptr;
}

const char *CArcadeIntegration::LocationDisplayName( int mission, const char *entity ) const {
	const LocationSpec *spec = FindLocationSpec( mission, entity );
	if ( spec ) {
		return spec->display;
	}
	// fall back to the entity name with underscores as spaces
	static idStr humanized;
	humanized = entity;
	humanized.Replace( "_", " " );
	return humanized.c_str();
}

bool CArcadeIntegration::ValidateAttempt( const Attempt &a, std::string &error ) const {
	if ( a.mission < 0 || a.mission >= NUM_MISSIONS ) {
		error = "unknown mission";
		return false;
	}
	if ( !missions[a.mission].available ) {
		error = va( "mission '%s' is not in the search path", MISSIONS[a.mission].id );
		return false;
	}
	const std::string &id = a.challengeId;
	std::string prefix = MISSIONS[a.mission].id;
	if ( id == prefix + CH_OBJECTIVE_SUFFIX ) {
		VarMap::const_iterator it = a.vars.find( "objective" );
		if ( it == a.vars.end() || !FindObjectiveSpec( a.mission, it->second.s ) ) {
			error = va( "unknown objective '%s'", it == a.vars.end() ? "" : it->second.s.c_str() );
			return false;
		}
	} else if ( id == prefix + CH_REACH_SUFFIX ) {
		VarMap::const_iterator it = a.vars.find( "location" );
		if ( it == a.vars.end() || missions[a.mission].locationNames.FindIndex( it->second.s.c_str() ) < 0 ) {
			error = va( "unknown location '%s'", it == a.vars.end() ? "" : it->second.s.c_str() );
			return false;
		}
	} else if ( !IsMissionChallenge( id ) ) {
		error = va( "unknown challenge '%s'", id.c_str() );
		return false;
	}
	if ( a.limitSeconds <= 0 ) {
		error = "missing 'minutes' variation";
		return false;
	}
	return true;
}

void CArcadeIntegration::HandleStartChallenge( uint64_t reqId, ArcadeProto::Reader req ) {
	// StartChallengeRequest { 1 challenge_id, 2 variations[], 3 run_id, 4 seed }
	Attempt a;
	int field;
	ArcadeProto::WireType wt;
	while ( req.Next( field, wt ) ) {
		switch ( field ) {
		case 1: a.challengeId = req.String(); break;
		case 2: {
			// VariationValue { 1 name, oneof 10 enum_value / 11 int_value / 12 float_value / 13 bool_value }
			ArcadeProto::Reader v = req.ReadMessage();
			std::string name;
			VarValue value;
			int f2;
			ArcadeProto::WireType w2;
			while ( v.Next( f2, w2 ) ) {
				switch ( f2 ) {
				case 1: name = v.String(); break;
				case VAR_ENUM: value.kind = VarValue::ENUM; value.s = v.String(); break;
				case VAR_INT: value.kind = VarValue::INT; value.i = v.Int64(); break;
				case VAR_FLOAT: value.kind = VarValue::FLOAT; value.d = v.Double(); break;
				case VAR_BOOL: value.kind = VarValue::BOOL; value.b = v.ReadBool(); break;
				default: v.Skip( w2 ); break;
				}
			}
			if ( !name.empty() ) {
				a.vars[name] = value;
			}
			break;
		}
		case 3: a.runId = req.String(); break;
		case 4: a.seed = req.Varint(); break;
		default: req.Skip( wt ); break;
		}
	}
	if ( req.HadError() ) {
		Fail( reqId, "bad StartChallengeRequest" );
		return;
	}

	// derive mission, difficulty, limit, stealth rule from the variations
	a.mission = MissionIndexForChallenge( a.challengeId );
	if ( a.mission < 0 ) {
		VarMap::const_iterator it = a.vars.find( "mission" );
		if ( it != a.vars.end() ) {
			for ( int m = 0; m < NUM_MISSIONS; m++ ) {
				if ( it->second.s == MISSIONS[m].id ) a.mission = m;
			}
		}
	}
	{
		VarMap::const_iterator it = a.vars.find( "difficulty" );
		a.difficulty = 0;
		if ( it != a.vars.end() ) {
			for ( int d = 0; d < 3; d++ ) {
				if ( it->second.s == DIFFICULTY_NAMES[d] ) a.difficulty = d;
			}
		}
	}
	{
		VarMap::const_iterator it = a.vars.find( "minutes" );
		a.limitSeconds = ( it != a.vars.end() && it->second.kind == VarValue::INT ) ? (int)it->second.i * 60 : 0;
	}
	{
		VarMap::const_iterator it = a.vars.find( "stealth" );
		a.stealth = STEALTH_ANY;
		if ( it != a.vars.end() ) {
			for ( int s = 0; s < 3; s++ ) {
				if ( it->second.s == STEALTH_NAMES[s] ) a.stealth = (StealthRule)s;
			}
		}
	}

	std::string error;
	if ( !ValidateAttempt( a, error ) ) {
		Fail( reqId, "%s", error.c_str() );
		return;
	}
	if ( state == STATE_LOADING ) {
		Fail( reqId, "a challenge is already starting" );
		return;
	}
	if ( state == STATE_RUNNING ) {
		CompleteAttempt( OUTCOME_ABORTED, 0.0, "superseded by a new StartChallenge" );
	}

	a.pendingRequestId = reqId;
	a.mapGenerationAtRequest = mapGeneration;
	a.loadIssuedMs = Sys_Milliseconds();
	attempt = a;
	state = STATE_LOADING;

	Log( LOG_INFO, "StartChallenge %s run=%s seed=%llu: loading %s (%s, %s difficulty)",
		a.challengeId.c_str(), a.runId.c_str(), (unsigned long long)a.seed, MISSIONS[a.mission].map, MISSIONS[a.mission].display, DIFFICULTY_NAMES[a.difficulty] );
	// Reloading the map is our "reset the world": executed by the event loop on the next frame.
	cvarSystem->SetCVarInteger( "tdm_difficulty", a.difficulty );
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "map %s\n", MISSIONS[a.mission].map ) );
}

void CArcadeIntegration::AdvanceLoading() {
	if ( mapGeneration > attempt.mapGenerationAtRequest && gameLocal.GameState() == GAMESTATE_ACTIVE && gameLocal.GetLocalPlayer() ) {
		if ( CurrentMissionIndex() != attempt.mission ) {
			Fail( attempt.pendingRequestId, "loaded map '%s' is not the requested mission", gameLocal.GetMapName() );
			state = STATE_IDLE;
			return;
		}
		StartRunning();
		return;
	}
	int waited = Sys_Milliseconds() - attempt.loadIssuedMs;
	// The "map" command loads synchronously inside the event loop (pumping GUIFrame, which
	// only polls). So once a normal frame runs a few seconds after we queued it, the map has
	// either come up (handled above) or its load failed and the game fell back to the menu.
	if ( waited > 3000 && mapGeneration == attempt.mapGenerationAtRequest && gameLocal.GameState() != GAMESTATE_STARTUP ) {
		Fail( attempt.pendingRequestId, "map '%s' failed to load (see the game console / qconsole.log for the error)", MISSIONS[attempt.mission].map );
		state = STATE_IDLE;
		return;
	}
	if ( waited > arcade_mapLoadTimeoutSec.GetInteger() * 1000 ) {
		Fail( attempt.pendingRequestId, "map '%s' did not load within %d seconds", MISSIONS[attempt.mission].map, arcade_mapLoadTimeoutSec.GetInteger() );
		state = STATE_IDLE;
	}
}

void CArcadeIntegration::StartRunning() {
	idPlayer *player = gameLocal.GetLocalPlayer();
	player->ForceReady();		// skip the "press attack to start" overlay
	gameLocal.random.SetSeed( (int)( attempt.seed ^ ( attempt.seed >> 32 ) ) );

	attempt.startGameTime = gameLocal.time;
	attempt.lastOrigin = player->GetPhysics()->GetOrigin();
	attempt.distanceUnits = 0.0f;
	attempt.maxAlertSeen = 0;
	attempt.visited.clear();
	const char *loc = PlayerLocationName( player );
	if ( loc[0] ) attempt.visited.insert( loc );

	ArcadeProto::Writer resp;	// StartChallengeResponse { 1 instruction, 2 game_time_s }
	resp.String( 1, ResolveInstruction( attempt ) );
	resp.Double( 2, GameTimeS() );
	Respond( attempt.pendingRequestId, resp );
	attempt.pendingRequestId = 0;
	state = STATE_RUNNING;

	ReportChallengeStarted();
	Log( LOG_INFO, "challenge %s running on %s (limit %ds, difficulty %d)", attempt.challengeId.c_str(), MISSIONS[attempt.mission].map, attempt.limitSeconds, attempt.difficulty );
}

void CArcadeIntegration::HandleStopChallenge( uint64_t reqId, ArcadeProto::Reader req ) {
	std::string reason;
	int field;
	ArcadeProto::WireType wt;
	while ( req.Next( field, wt ) ) {
		if ( field == 1 ) reason = req.String(); else req.Skip( wt );
	}
	if ( state == STATE_RUNNING ) {
		CompleteAttempt( OUTCOME_ABORTED, 0.0, reason.empty() ? "stopped by controller" : reason.c_str() );
	} else if ( state == STATE_LOADING ) {
		Fail( attempt.pendingRequestId, "stopped before the map finished loading" );
		state = STATE_IDLE;
	}
	Respond( reqId, ArcadeProto::Writer() );	// StopChallengeResponse {}
}

void CArcadeIntegration::HandlePing( uint64_t reqId ) {
	ArcadeProto::Writer resp;	// PingResponse { 1 game_time_s }
	resp.Double( 1, GameTimeS() );
	Respond( reqId, resp );
}

std::string CArcadeIntegration::ResolveInstruction( const Attempt &a ) const {
	const MissionInfo &mi = MISSIONS[a.mission];
	std::string text = ChallengeInstructionTemplate( a.challengeId );

	// substitute the variation values, with human-readable stand-ins where the raw value is an id
	for ( VarMap::const_iterator it = a.vars.begin(); it != a.vars.end(); ++it ) {
		std::string placeholder = "{" + it->first + "}";
		std::string value = it->second.ToString();
		if ( it->first == "mission" ) value = mi.display;
		if ( it->first == "location" ) value = LocationDisplayName( a.mission, it->second.s.c_str() );
		if ( it->first == "objective" ) {
			const ObjectiveSpec *spec = FindObjectiveSpec( a.mission, it->second.s );
			if ( spec ) value = spec->summary;
		}
		if ( it->first == "stealth" ) {
			if ( it->second.s == STEALTH_NAMES[STEALTH_UNSEEN] ) value = "unseen (no guard may start searching for you or spot you)";
			else if ( it->second.s == STEALTH_NAMES[STEALTH_GHOST] ) value = "ghost (no guard may even become suspicious)";
			else value = "none (being noticed is allowed, but lowers your score)";
		}
		size_t pos;
		while ( ( pos = text.find( placeholder ) ) != std::string::npos ) {
			text.replace( pos, placeholder.size(), value );
		}
	}

	std::string full = std::string( "Mission: " ) + mi.display + ". " + mi.summary + "\n\nTask: " + text;
	if ( a.challengeId == CH_COMPLETE_MISSION ) {
		full += " Press O at any time to see the current objectives; the mission ends by itself when the last mandatory one is done. Dying fails the mission.";
	} else if ( a.challengeId == CH_STEAL_LOOT ) {
		full += " Your loot total is shown when you pick something up.";
	} else if ( a.challengeId == CH_KNOCKOUT ) {
		full += " Approach from behind while the guard is unaware, with the blackjack raised, and strike the head. A killed guard fails the attempt.";
	}
	full += "\n\n" + std::string( CONTROLS_HINT );
	return full;
}

// ===========================================================================
// Judging
// ===========================================================================

int CArcadeIntegration::CurrentMissionIndex() const {
	idStr map = gameLocal.GetMapName();		// "maps/prologue9.map"
	map.StripPath();
	map.StripFileExtension();
	for ( int m = 0; m < NUM_MISSIONS; m++ ) {
		if ( map.Icmp( MISSIONS[m].map ) == 0 ) return m;
	}
	return -1;
}

bool CArcadeIntegration::Snapshot( MissionSnapshot &out ) const {
	memset( &out, 0, sizeof( out ) );
	out.mission = CurrentMissionIndex();
	gameState_t gs = gameLocal.GameState();
	if ( gs != GAMESTATE_ACTIVE && gs != GAMESTATE_COMPLETED ) {
		return false;
	}
	CMissionData *md = gameLocal.m_MissionData.get();
	if ( !md ) {
		return false;
	}
	out.difficulty = gameLocal.m_DifficultyManager.GetDifficultyLevel();
	out.result = gameLocal.m_MissionResult;
	out.lootFound = md->GetFoundLoot();
	out.lootTotal = md->GetMissionLoot();
	out.stealthScore = md->GetStealthScore();
	out.timesSeen = md->GetNumberTimesPlayerSeen();
	out.timesSuspicious = md->GetNumberTimesAISuspicious();
	out.timesSearched = md->GetNumberTimesAISearched();
	out.knockouts = md->GetStatOverall( COMP_KO );
	out.kills = md->GetStatOverall( COMP_KILL );
	out.damageReceived = md->GetDamageReceived();
	out.pocketsPicked = md->GetPocketsPicked();
	out.objectivesTotal = md->GetNumObjectives();
	for ( int i = 0; i < out.objectivesTotal; i++ ) {
		const CObjective &obj = md->GetObjective( i );
		if ( obj.m_bMandatory && obj.m_bApplies ) {
			out.objectivesMandatory++;
			if ( md->GetCompletionState( i ) == STATE_COMPLETE ) out.objectivesMandatoryComplete++;
		}
	}
	return true;
}

float CArcadeIntegration::StealthFactor( const MissionSnapshot &s ) const {
	// 1.0 for a perfect ghost, falling towards 0.25 as alerts and sightings pile up
	return 0.25f + 0.75f / ( 1.0f + s.stealthScore / 10.0f );
}

bool CArcadeIntegration::StealthViolated( const MissionSnapshot &s, StealthRule rule, const char *&why ) const {
	if ( rule == STEALTH_ANY ) return false;
	if ( s.timesSeen > 0 ) { why = "a guard spotted you"; return true; }
	if ( s.timesSearched > 0 ) { why = "a guard started searching for you"; return true; }
	if ( rule == STEALTH_GHOST && s.timesSuspicious > 0 ) { why = "a guard became suspicious"; return true; }
	return false;
}

bool CArcadeIntegration::ObjectiveSpecComplete( const ObjectiveSpec &spec, bool &failed, std::string &text ) const {
	CMissionData *md = gameLocal.m_MissionData.get();
	failed = false;
	text.clear();
	if ( !md ) return false;
	bool anyApplies = false;
	for ( int k = 0; k < 3 && spec.indices[k]; k++ ) {
		int idx = spec.indices[k] - 1;
		if ( idx < 0 || idx >= md->GetNumObjectives() ) continue;
		const CObjective &obj = md->GetObjective( idx );
		if ( !obj.m_bApplies ) continue;
		anyApplies = true;
		if ( text.empty() ) text = common->Translate( obj.m_text.c_str() );
		int st = md->GetCompletionState( idx );
		if ( st == STATE_COMPLETE ) return true;
		if ( st == STATE_FAILED ) failed = true;
	}
	if ( !anyApplies ) {
		// none of the alternatives is active at this difficulty: the mission's own completion is the fallback
		return gameLocal.m_MissionResult == MISSION_COMPLETE;
	}
	return false;
}

void CArcadeIntegration::Judge() {
	idPlayer *player = gameLocal.GetLocalPlayer();
	MissionSnapshot s;
	if ( !Snapshot( s ) || !player ) {
		return;	// map is going away; OnMapShutdown() reports the abort
	}

	// bookkeeping
	const idVec3 &origin = player->GetPhysics()->GetOrigin();
	float step = ( origin - attempt.lastOrigin ).Length();
	if ( step < 200.0f ) {	// ignore teleports
		attempt.distanceUnits += step;
	}
	attempt.lastOrigin = origin;
	attempt.maxAlertSeen = Max( attempt.maxAlertSeen, MaxAiAlertIndex() );
	const char *loc = PlayerLocationName( player );
	if ( loc[0] ) attempt.visited.insert( loc );

	double elapsed = ( gameLocal.time - attempt.startGameTime ) * 0.001;
	bool timeUp = attempt.limitSeconds > 0 && elapsed >= attempt.limitSeconds;
	const std::string &id = attempt.challengeId;
	std::string prefix = MISSIONS[attempt.mission].id;
	const float stealth = StealthFactor( s );
	const char *statsLine = va( "stealth score %.0f, seen %d times, %d knockouts, %d kills, loot %d/%d, %.1f s",
		s.stealthScore, s.timesSeen, s.knockouts, s.kills, s.lootFound, s.lootTotal, elapsed );

	// universal failure conditions
	if ( player->health <= 0 ) {
		CompleteAttempt( OUTCOME_FAILURE, 0.0, va( "player died (%s)", statsLine ) );
		return;
	}
	if ( s.result == MISSION_FAILED ) {
		CompleteAttempt( OUTCOME_FAILURE, 0.0, va( "mission failed (%s)", statsLine ) );
		return;
	}
	const char *why = nullptr;
	if ( StealthViolated( s, attempt.stealth, why ) ) {
		CompleteAttempt( OUTCOME_FAILURE, 0.0, va( "stealth rule broken: %s (%s)", why, statsLine ) );
		return;
	}

	if ( id == CH_COMPLETE_MISSION ) {
		if ( s.result == MISSION_COMPLETE ) {
			CompleteAttempt( OUTCOME_SUCCESS, 0.5 + 0.5 * stealth, va( "mission complete (%s)", statsLine ) );
			return;
		}
		if ( timeUp ) {
			double frac = s.objectivesMandatory > 0 ? (double)s.objectivesMandatoryComplete / s.objectivesMandatory : 0.0;
			CompleteAttempt( OUTCOME_TIMEOUT, 0.5 * frac, va( "%d/%d mandatory objectives when time ran out (%s)", s.objectivesMandatoryComplete, s.objectivesMandatory, statsLine ) );
			return;
		}
	} else if ( id == prefix + CH_OBJECTIVE_SUFFIX ) {
		const ObjectiveSpec *spec = FindObjectiveSpec( attempt.mission, attempt.vars["objective"].s );
		bool failed = false;
		std::string text;
		if ( spec && ObjectiveSpecComplete( *spec, failed, text ) ) {
			CompleteAttempt( OUTCOME_SUCCESS, stealth, va( "objective '%s' complete (%s)", text.c_str(), statsLine ) );
			return;
		}
		if ( failed ) {
			CompleteAttempt( OUTCOME_FAILURE, 0.0, va( "objective '%s' failed (%s)", text.c_str(), statsLine ) );
			return;
		}
	} else if ( id == prefix + CH_REACH_SUFFIX ) {
		const std::string &target = attempt.vars["location"].s;
		if ( target == loc ) {
			double score = 0.5 + 0.5 * Max( 0.0, 1.0 - elapsed / attempt.limitSeconds );
			CompleteAttempt( OUTCOME_SUCCESS, score * stealth, va( "reached %s (%s)", LocationDisplayName( attempt.mission, target.c_str() ), statsLine ) );
			return;
		}
	} else if ( id == CH_STEAL_LOOT ) {
		int percent = (int)attempt.vars["percent"].i;
		int target = s.lootTotal > 0 ? ( s.lootTotal * percent + 99 ) / 100 : 0;
		if ( s.lootTotal > 0 && s.lootFound >= target ) {
			CompleteAttempt( OUTCOME_SUCCESS, stealth, va( "stole %d of %d loot (target %d = %d%%) (%s)", s.lootFound, s.lootTotal, target, percent, statsLine ) );
			return;
		}
		if ( timeUp ) {
			CompleteAttempt( OUTCOME_TIMEOUT, target > 0 ? Min( 1.0, (double)s.lootFound / target ) : 0.0, va( "%d/%d loot (target %d) when time ran out (%s)", s.lootFound, s.lootTotal, target, statsLine ) );
			return;
		}
	} else if ( id == CH_KNOCKOUT ) {
		int count = (int)attempt.vars["count"].i;
		if ( s.kills > 0 ) {
			CompleteAttempt( OUTCOME_FAILURE, 0.0, va( "someone was killed (%s)", statsLine ) );
			return;
		}
		if ( s.knockouts >= count ) {
			CompleteAttempt( OUTCOME_SUCCESS, stealth, va( "%d knockouts (%s)", s.knockouts, statsLine ) );
			return;
		}
		if ( timeUp ) {
			CompleteAttempt( OUTCOME_TIMEOUT, count > 0 ? (double)s.knockouts / count : 0.0, va( "%d/%d knockouts when time ran out (%s)", s.knockouts, count, statsLine ) );
			return;
		}
	} else if ( id == CH_EXPLORE ) {
		if ( timeUp ) {
			int total = missions[attempt.mission].locationNames.Num();
			double frac = total > 0 ? (double)attempt.visited.size() / total : 0.0;
			CompleteAttempt( OUTCOME_SUCCESS, frac, va( "visited %d of %d areas, travelled %.0f m (%s)", (int)attempt.visited.size(), total, attempt.distanceUnits * UNITS_TO_METERS, statsLine ) );
			return;
		}
	}

	if ( timeUp ) {
		CompleteAttempt( OUTCOME_TIMEOUT, 0.0, va( "time limit of %d s reached (%s)", attempt.limitSeconds, statsLine ) );
	}
}

void CArcadeIntegration::CompleteAttempt( Outcome outcome, double score, const char *detail ) {
	if ( state != STATE_RUNNING ) {
		return;
	}
	state = STATE_IDLE;

	double elapsed = ( gameLocal.time - attempt.startGameTime ) * 0.001;
	MissionSnapshot s;
	bool haveStats = Snapshot( s );

	// ChallengeCompleted { 1 challenge_id, 2 run_id, 3 outcome, 4 score, 5 detail, 6 final_metrics[] }
	ArcadeProto::Writer done;
	done.String( 1, attempt.challengeId );
	done.String( 2, attempt.runId );
	done.Enum( 3, outcome );
	done.Double( 4, score );
	done.String( 5, detail );
	struct { const char *name; double value; } finals[] = {
		{ "time/elapsed_s", elapsed },
		{ "player/distance_travelled_m", attempt.distanceUnits * UNITS_TO_METERS },
		{ "ai/max_alert_index", attempt.maxAlertSeen },
		{ "explore/locations_visited", (double)attempt.visited.size() },
		{ "mission/stealth_score", haveStats ? s.stealthScore : 0.0 },
		{ "mission/times_seen", haveStats ? s.timesSeen : 0.0 },
		{ "mission/loot_fraction", haveStats && s.lootTotal > 0 ? (double)s.lootFound / s.lootTotal : 0.0 },
		{ "player/loot", haveStats ? s.lootFound : 0.0 },
		{ "ai/knockouts", haveStats ? s.knockouts : 0.0 },
		{ "ai/kills", haveStats ? s.kills : 0.0 },
		{ "mission/objectives_complete", haveStats ? s.objectivesMandatoryComplete : 0.0 },
		{ "mission/damage_received", haveStats ? s.damageReceived : 0.0 },
	};
	for ( size_t i = 0; i < sizeof( finals ) / sizeof( finals[0] ); i++ ) {
		ArcadeProto::Writer m;
		m.String( 1, finals[i].name );
		m.Double( 2, finals[i].value );
		done.PutMessageAlways( 6, m );
	}

	ArcadeProto::Writer report;
	report.Double( 2, GameTimeS() );
	report.PutMessageAlways( REPORT_CHALLENGE_COMPLETED, done );
	sdk.report( 0, report.Data(), report.Size() );

	static const char *OUTCOME_NAMES[] = { "unspecified", "success", "failure", "timeout", "aborted" };
	Log( LOG_INFO, "challenge %s %s: %s (score %.3f)", attempt.challengeId.c_str(), OUTCOME_NAMES[outcome], detail, score );
}

// ===========================================================================
// Vendor service: thedarkmod.v1.Game
// ===========================================================================

void CArcadeIntegration::WriteVec3( ArcadeProto::Writer &w, int field, const idVec3 &v ) {
	ArcadeProto::Writer sub;
	sub.Float( 1, v.x );
	sub.Float( 2, v.y );
	sub.Float( 3, v.z );
	w.PutMessageAlways( field, sub );
}

bool CArcadeIntegration::ReadVec3( ArcadeProto::Reader r, idVec3 &out ) {
	out.Zero();
	int field;
	ArcadeProto::WireType wt;
	while ( r.Next( field, wt ) ) {
		switch ( field ) {
		case 1: out.x = r.Float(); break;
		case 2: out.y = r.Float(); break;
		case 3: out.z = r.Float(); break;
		default: r.Skip( wt ); break;
		}
	}
	return !r.HadError();
}

void CArcadeIntegration::HandleTeleportPlayer( uint64_t reqId, ArcadeProto::Reader req ) {
	idPlayer *player = gameLocal.GetLocalPlayer();
	if ( !player || gameLocal.GameState() != GAMESTATE_ACTIVE ) {
		Fail( reqId, "no map loaded" );
		return;
	}
	idVec3 position = player->GetPhysics()->GetOrigin();
	idAngles angles = player->viewAngles;
	bool havePosition = false;
	int field;
	ArcadeProto::WireType wt;
	while ( req.Next( field, wt ) ) {
		switch ( field ) {
		case 1: havePosition = ReadVec3( req.ReadMessage(), position ); break;
		case 2: angles.yaw = req.Float(); break;
		case 3: angles.pitch = req.Float(); break;
		default: req.Skip( wt ); break;
		}
	}
	if ( req.HadError() || !havePosition ) {
		Fail( reqId, "bad TeleportPlayerRequest (position required)" );
		return;
	}
	angles.roll = 0.0f;
	player->Teleport( position, angles, nullptr );
	if ( state == STATE_RUNNING ) {
		attempt.lastOrigin = player->GetPhysics()->GetOrigin();
	}
	Respond( reqId, ArcadeProto::Writer() );	// TeleportPlayerResponse {}
}

void CArcadeIntegration::HandleGetPlayerState( uint64_t reqId ) {
	idPlayer *player = gameLocal.GetLocalPlayer();
	if ( !player || ( gameLocal.GameState() != GAMESTATE_ACTIVE && gameLocal.GameState() != GAMESTATE_COMPLETED ) ) {
		Fail( reqId, "no map loaded" );
		return;
	}
	// PlayerState (see vendor.proto)
	ArcadeProto::Writer s;
	WriteVec3( s, 1, player->GetPhysics()->GetOrigin() );
	{
		ArcadeProto::Writer view;	// arcade.common.v1.Euler { 1 pitch_deg, 2 yaw_deg, 3 roll_deg }
		view.Float( 1, player->viewAngles.pitch );
		view.Float( 2, player->viewAngles.yaw );
		view.Float( 3, player->viewAngles.roll );
		s.PutMessageAlways( 2, view );
	}
	WriteVec3( s, 3, player->GetPhysics()->GetLinearVelocity() );
	s.Int32( 4, player->health );
	s.PutBool( 5, player->health <= 0 );
	s.Int32( 6, PlayerLoot( player ) );
	s.String( 7, PlayerLocationName( player ) );
	s.Float( 8, player->GetCurrentLightgemValue() / LIGHTGEM_MAX );
	s.Int32( 9, MaxAiAlertIndex() );
	s.Double( 10, gameLocal.time * 0.001 );
	s.PutBool( 11, player->GetPlayerPhysics() && player->GetPlayerPhysics()->IsCrouching() );
	Respond( reqId, s );
}

void CArcadeIntegration::HandleListLocations( uint64_t reqId ) {
	int m = CurrentMissionIndex();
	if ( m < 0 || !missions[m].available ) {
		Fail( reqId, "no arcade mission map loaded" );
		return;
	}
	ArcadeProto::Writer resp;
	const MissionRuntime &rt = missions[m];
	for ( int i = 0; i < rt.locationNames.Num(); i++ ) {
		ArcadeProto::Writer loc;
		loc.String( 1, rt.locationNames[i].c_str() );
		WriteVec3( loc, 2, rt.locationOrigins[i] );
		loc.String( 3, LocationDisplayName( m, rt.locationNames[i].c_str() ) );
		resp.PutMessageAlways( 1, loc );
	}
	Respond( reqId, resp );
}

void CArcadeIntegration::HandleListAi( uint64_t reqId ) {
	idPlayer *player = gameLocal.GetLocalPlayer();
	if ( !player || gameLocal.GameState() < GAMESTATE_ACTIVE ) {
		Fail( reqId, "no map loaded" );
		return;
	}
	const idVec3 &playerOrigin = player->GetPhysics()->GetOrigin();
	ArcadeProto::Writer resp;
	for ( idEntity *ent = gameLocal.spawnedEntities.Next(); ent != nullptr; ent = ent->spawnNode.Next() ) {
		if ( !ent->IsType( idAI::Type ) ) {
			continue;
		}
		idAI *ai = static_cast<idAI *>( ent );
		ArcadeProto::Writer a;
		a.String( 1, ai->GetName() );
		WriteVec3( a, 2, ai->GetPhysics()->GetOrigin() );
		a.Int32( 3, ai->health );
		a.PutBool( 4, ai->health <= 0 );
		a.Int32( 5, ai->AI_AlertIndex.IsLinked() ? (int)(float)ai->AI_AlertIndex : 0 );
		a.Float( 6, ( ai->GetPhysics()->GetOrigin() - playerOrigin ).Length() );
		resp.PutMessageAlways( 1, a );
	}
	Respond( reqId, resp );
}

void CArcadeIntegration::HandleExecConsoleCommand( uint64_t reqId, ArcadeProto::Reader req ) {
	std::string command;
	int field;
	ArcadeProto::WireType wt;
	while ( req.Next( field, wt ) ) {
		if ( field == 1 ) command = req.String(); else req.Skip( wt );
	}
	if ( req.HadError() || command.empty() ) {
		Fail( reqId, "bad ExecConsoleCommandRequest (command required)" );
		return;
	}
	Log( LOG_INFO, "console command from controller: %s", command.c_str() );
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, ( command + "\n" ).c_str() );
	Respond( reqId, ArcadeProto::Writer() );
}

void CArcadeIntegration::HandleGetMissionState( uint64_t reqId ) {
	MissionSnapshot s;
	if ( !Snapshot( s ) ) {
		Fail( reqId, "no map loaded" );
		return;
	}
	CMissionData *md = gameLocal.m_MissionData.get();
	ArcadeProto::Writer resp;	// MissionState, see vendor.proto
	if ( s.mission >= 0 ) resp.String( 1, MISSIONS[s.mission].id );
	idStr map = gameLocal.GetMapName();
	map.StripPath();
	map.StripFileExtension();
	resp.String( 2, map.c_str() );
	resp.Int32( 3, s.difficulty );
	resp.Int32( 4, s.result );
	for ( int i = 0; i < md->GetNumObjectives(); i++ ) {
		const CObjective &obj = md->GetObjective( i );
		ArcadeProto::Writer o;
		o.Int32( 1, i + 1 );
		o.String( 2, common->Translate( obj.m_text.c_str() ) );
		o.PutBool( 3, obj.m_bMandatory );
		o.PutBool( 4, obj.m_bVisible );
		o.PutBool( 5, obj.m_bApplies );
		o.Int32( 6, md->GetCompletionState( i ) );
		resp.PutMessageAlways( 5, o );
	}
	resp.Int32( 6, s.lootFound );
	resp.Int32( 7, s.lootTotal );
	resp.Float( 8, s.stealthScore );
	resp.Int32( 9, s.timesSeen );
	resp.Int32( 10, s.timesSuspicious );
	resp.Int32( 11, s.timesSearched );
	resp.Int32( 12, s.knockouts );
	resp.Int32( 13, s.kills );
	resp.Int32( 14, s.damageReceived );
	resp.Int32( 15, s.pocketsPicked );
	Respond( reqId, resp );
}

// ===========================================================================
// Observation: events + metrics
// ===========================================================================

int CArcadeIntegration::PlayerLoot( idPlayer *player ) const {
	int gold = 0, jewelry = 0, goods = 0;
	if ( player->Inventory() ) {
		player->Inventory()->GetLoot( gold, jewelry, goods );
	}
	return gold + jewelry + goods;
}

const char *CArcadeIntegration::PlayerLocationName( idPlayer *player ) const {
	idLocationEntity *loc = gameLocal.LocationForPoint( player->GetPhysics()->GetOrigin() );
	return loc ? loc->GetName() : "";
}

int CArcadeIntegration::MaxAiAlertIndex( idAI **culprit ) const {
	int maxAlert = 0;
	if ( culprit ) {
		*culprit = nullptr;
	}
	for ( idEntity *ent = gameLocal.spawnedEntities.Next(); ent != nullptr; ent = ent->spawnNode.Next() ) {
		if ( !ent->IsType( idAI::Type ) ) {
			continue;
		}
		idAI *ai = static_cast<idAI *>( ent );
		if ( ai->health <= 0 || !ai->AI_AlertIndex.IsLinked() ) {
			continue;
		}
		int idx = (int)(float)ai->AI_AlertIndex;
		if ( idx > maxAlert ) {
			maxAlert = idx;
			if ( culprit ) {
				*culprit = ai;
			}
		}
	}
	return maxAlert;
}

void CArcadeIntegration::ResetObservers() {
	lastLoot = 0;
	lastHealth = 0;
	wasDead = false;
	lastMissionResult = MISSION_INPROGRESS;
	lastKnockouts = 0;
	lastKills = 0;
	lastLocation = "";
	lastObjectiveStates.clear();
	lastAiAlert.clear();
	lastMetricsPushMs = 0;
}

void CArcadeIntegration::Observe() {
	MissionSnapshot s;
	if ( !Snapshot( s ) ) {
		observedGeneration = -1;
		return;
	}
	idPlayer *player = gameLocal.GetLocalPlayer();
	if ( !player ) {
		return;
	}
	CMissionData *md = gameLocal.m_MissionData.get();

	if ( observedGeneration != mapGeneration ) {
		// first frame on a new map
		observedGeneration = mapGeneration;
		ResetObservers();
		lastLoot = PlayerLoot( player );
		lastHealth = player->health;
		wasDead = player->health <= 0;
		lastLocation = PlayerLocationName( player );
		lastMissionResult = s.result;
		lastKnockouts = s.knockouts;
		lastKills = s.kills;
		lastObjectiveStates.assign( s.objectivesTotal, STATE_INCOMPLETE );
		for ( int i = 0; i < s.objectivesTotal; i++ ) lastObjectiveStates[i] = md->GetCompletionState( i );
		ArcadeProto::Writer ev;
		ev.String( 1, gameLocal.GetMapName() );
		ReportEvent( EV_MAP_LOADED, ev );
	}

	if ( arcade_autoReady.GetBool() && !player->IsReady() ) {
		player->ForceReady();
	}

	// loot
	int loot = PlayerLoot( player );
	if ( loot > lastLoot ) {
		ArcadeProto::Writer ev;
		ev.Int32( 1, loot - lastLoot );
		ev.Int32( 2, loot );
		ReportEvent( EV_LOOT_PICKED_UP, ev );
	}
	lastLoot = loot;

	// health / death
	if ( player->health < lastHealth ) {
		ArcadeProto::Writer ev;
		ev.Int32( 1, lastHealth - player->health );
		ev.Int32( 2, player->health );
		ReportEvent( EV_PLAYER_DAMAGED, ev );
	}
	lastHealth = player->health;
	bool dead = player->health <= 0;
	if ( dead && !wasDead ) {
		ArcadeProto::Writer ev;
		WriteVec3( ev, 1, player->GetPhysics()->GetOrigin() );
		ReportEvent( EV_PLAYER_DIED, ev );
	}
	wasDead = dead;

	// location
	const char *location = PlayerLocationName( player );
	if ( lastLocation != location ) {
		ArcadeProto::Writer ev;
		ev.String( 1, location );
		ev.String( 2, lastLocation.c_str() );
		ReportEvent( EV_LOCATION_CHANGED, ev );
		lastLocation = location;
	}

	// AI alert transitions
	const idVec3 &playerOrigin = player->GetPhysics()->GetOrigin();
	int maxAlert = 0;
	for ( idEntity *ent = gameLocal.spawnedEntities.Next(); ent != nullptr; ent = ent->spawnNode.Next() ) {
		if ( !ent->IsType( idAI::Type ) ) {
			continue;
		}
		idAI *ai = static_cast<idAI *>( ent );
		if ( ai->health <= 0 || !ai->AI_AlertIndex.IsLinked() ) {
			continue;
		}
		int idx = (int)(float)ai->AI_AlertIndex;
		maxAlert = Max( maxAlert, idx );
		std::map<std::string, int>::iterator it = lastAiAlert.find( ai->GetName() );
		int previous = ( it == lastAiAlert.end() ) ? 0 : it->second;
		if ( idx != previous ) {
			ArcadeProto::Writer ev;
			ev.String( 1, ai->GetName() );
			ev.Int32( 2, idx );
			ev.Int32( 3, previous );
			ev.Float( 4, ( ai->GetPhysics()->GetOrigin() - playerOrigin ).Length() );
			ReportEvent( EV_AI_ALERT_CHANGED, ev );
			lastAiAlert[ai->GetName()] = idx;
		}
	}

	// knockouts / kills (from the mission statistics)
	if ( s.knockouts > lastKnockouts ) {
		ArcadeProto::Writer ev;
		ev.Int32( 1, s.knockouts );
		ReportEvent( EV_AI_KNOCKED_OUT, ev );
	}
	lastKnockouts = s.knockouts;
	if ( s.kills > lastKills ) {
		ArcadeProto::Writer ev;
		ev.Int32( 1, s.kills );
		ReportEvent( EV_AI_KILLED, ev );
	}
	lastKills = s.kills;

	// objectives
	if ( (int)lastObjectiveStates.size() != s.objectivesTotal ) {
		lastObjectiveStates.assign( s.objectivesTotal, STATE_INCOMPLETE );
	}
	for ( int i = 0; i < s.objectivesTotal; i++ ) {
		int st = md->GetCompletionState( i );
		if ( st != lastObjectiveStates[i] ) {
			ArcadeProto::Writer ev;
			ev.Int32( 1, i + 1 );
			ev.String( 2, common->Translate( md->GetObjective( i ).m_text.c_str() ) );
			ev.Int32( 3, st );
			ev.Int32( 4, lastObjectiveStates[i] );
			ReportEvent( EV_OBJECTIVE_CHANGED, ev );
			lastObjectiveStates[i] = st;
		}
	}

	// mission end
	if ( s.result != lastMissionResult && ( s.result == MISSION_COMPLETE || s.result == MISSION_FAILED ) ) {
		ArcadeProto::Writer ev;
		ev.Int32( 1, s.result );
		ReportEvent( EV_MISSION_ENDED, ev );
		if ( s.result == MISSION_COMPLETE ) {
			ReportEvent( EV_MISSION_COMPLETED, ArcadeProto::Writer() );
		}
	}
	lastMissionResult = s.result;

	// metrics
	int now = Sys_Milliseconds();
	if ( now - lastMetricsPushMs >= arcade_metricsIntervalMs.GetInteger() ) {
		lastMetricsPushMs = now;
		double t = GameTimeS();
		sdk.push_f32_metric( 0, mHealth, (float)player->health, t );
		sdk.push_f32_metric( 0, mLoot, (float)loot, t );
		sdk.push_f32_metric( 0, mLightgem, player->GetCurrentLightgemValue() / LIGHTGEM_MAX, t );
		sdk.push_f32_metric( 0, mMaxAlert, (float)maxAlert, t );
		sdk.push_f32_metric( 0, mStealthScore, s.stealthScore, t );
		sdk.push_f32_metric( 0, mLootFraction, s.lootTotal > 0 ? (float)s.lootFound / s.lootTotal : 0.0f, t );
		sdk.push_f32_metric( 0, mKnockouts, (float)s.knockouts, t );
		sdk.push_f32_metric( 0, mKills, (float)s.kills, t );
		sdk.push_f32_metric( 0, mObjectivesComplete, (float)s.objectivesMandatoryComplete, t );
		if ( state == STATE_RUNNING ) {
			sdk.push_f32_metric( 0, mDistance, attempt.distanceUnits * UNITS_TO_METERS, t );
			sdk.push_f32_metric( 0, mLocationsVisited, (float)attempt.visited.size(), t );
		}
	}
}

// ===========================================================================
// Frames out (render backend thread) and input in (main thread)
// ===========================================================================

void CArcadeIntegration::FrameCaptureThunk( const unsigned char *rgba, int width, int height, int stride, void *user ) {
	static_cast<CArcadeIntegration *>( user )->OnFrameCaptured( rgba, width, height, stride );
}

void CArcadeIntegration::OnFrameCaptured( const unsigned char *rgba, int width, int height, int stride ) {
	// Runs on the render backend thread, right before the buffers are swapped.
	std::lock_guard<std::mutex> lock( captureMutex );
	if ( !capturing || !active || !sdk.submit_frame ) {
		return;
	}
	ArcadeFrame frame;
	memset( &frame, 0, sizeof( frame ) );
	frame.frame_index = framesSubmitted.load() + 1;
	frame.game_time_s = GameTimeS();
	frame.pixels = rgba;
	frame.width = width;
	frame.height = height;
	frame.stride = stride;
	frame.format = ARCADE_PIXEL_FORMAT_RGBA8;
	frame.flags = ARCADE_FRAME_FLIP_Y;	// glReadPixels rows are bottom-up
	ArcadeStatus status = sdk.submit_frame( 0, &frame );
	if ( status == ARCADE_STATUS_OK ) {
		framesSubmitted = frame.frame_index;
	} else if ( inputWarnings++ < 5 ) {
		common->Warning( "Arcade SDK: arcade_submit_frame failed (%d): %s", (int)status, sdk.last_error() );
	}

	if ( !dumpFramePath.empty() ) {
		// what the SDK will show after honouring FLIP_Y: top row first, RGB
		FILE *f = fopen( dumpFramePath.c_str(), "wb" );
		if ( f ) {
			fprintf( f, "P6\n%d %d\n255\n", width, height );
			for ( int y = height - 1; y >= 0; y-- ) {
				const unsigned char *row = rgba + y * stride;
				for ( int x = 0; x < width; x++ ) {
					fwrite( row + x * 4, 1, 3, f );
				}
			}
			fclose( f );
			common->Printf( "Arcade: wrote %s (%dx%d)\n", dumpFramePath.c_str(), width, height );
		} else {
			common->Warning( "Arcade: cannot write %s", dumpFramePath.c_str() );
		}
		dumpFramePath.clear();
	}
}

void CArcadeIntegration::RequestFrameDump( const char *path ) {
	std::lock_guard<std::mutex> lock( captureMutex );
	dumpFramePath = path;
}

int CArcadeIntegration::KeyCodeToTdmKey( int code ) {
	// arcade.sdk.v1.KeyCode (W3C KeyboardEvent.code positions) -> framework/KeyInput.h keynums
	if ( code >= 20 && code <= 45 ) return 'a' + ( code - 20 );			// KEY_A .. KEY_Z
	if ( code >= 6 && code <= 15 ) return '0' + ( code - 6 );			// DIGIT0 .. DIGIT9
	if ( code >= 160 && code <= 171 ) return K_F1 + ( code - 160 );		// F1 .. F12
	if ( code >= 172 && code <= 174 ) return K_F13 + ( code - 172 );	// F13 .. F15
	switch ( code ) {
	case 1: return '`';		// BACKQUOTE
	case 2: return '\\';	// BACKSLASH
	case 3: return '[';
	case 4: return ']';
	case 5: return ',';
	case 16: return '=';
	case 46: return '-';
	case 47: return '.';
	case 48: return '\'';
	case 49: return ';';
	case 50: return '/';
	case 51: case 52: return K_ALT;
	case 53: return K_BACKSPACE;
	case 54: return K_CAPSLOCK;
	case 55: return K_MENU;
	case 56: case 57: return K_CTRL;
	case 58: return K_ENTER;
	case 59: return K_LWIN;
	case 60: return K_RWIN;
	case 61: case 62: return K_SHIFT;
	case 63: return K_SPACE;
	case 64: return K_TAB;
	case 73: return K_DEL;
	case 74: return K_END;
	case 76: return K_HOME;
	case 77: return K_INS;
	case 78: return K_PGDN;
	case 79: return K_PGUP;
	case 80: return K_DOWNARROW;
	case 81: return K_LEFTARROW;
	case 82: return K_RIGHTARROW;
	case 83: return K_UPARROW;
	case 84: return K_KP_NUMLOCK;
	case 85: return K_KP_INS;			// NUMPAD0
	case 86: return K_KP_END;
	case 87: return K_KP_DOWNARROW;
	case 88: return K_KP_PGDN;
	case 89: return K_KP_LEFTARROW;
	case 90: return K_KP_5;
	case 91: return K_KP_RIGHTARROW;
	case 92: return K_KP_HOME;
	case 93: return K_KP_UPARROW;
	case 94: return K_KP_PGUP;			// NUMPAD9
	case 95: return K_KP_PLUS;
	case 100: return K_KP_DEL;			// NUMPAD_DECIMAL
	case 101: return K_KP_SLASH;
	case 102: return K_KP_ENTER;
	case 103: return K_KP_EQUALS;
	case 110: return K_KP_STAR;
	case 114: return K_KP_MINUS;
	case 115: return K_ESCAPE;
	case 119: return K_SCROLL;
	case 120: return K_PAUSE;
	default: return 0;					// no TDM equivalent: ignored
	}
}

void CArcadeIntegration::ApplyInputEvent( ArcadeProto::Reader event ) {
	// InputEvent { oneof kind: 1 key { 1 code, 2 down }, 2 button { 1 button, 2 down },
	//              3 mouse_move { 1 dx, 2 dy }, 4 wheel { 1 dx, 2 dy } }
	int kind;
	ArcadeProto::WireType kt;
	if ( !event.Next( kind, kt ) || kt != ArcadeProto::WIRE_LENGTH ) {
		return;
	}
	ArcadeProto::Reader body = event.ReadMessage();
	int f;
	ArcadeProto::WireType t;
	int intA = 0;
	bool down = false;
	double dx = 0.0, dy = 0.0;
	while ( body.Next( f, t ) ) {
		if ( kind == INEV_MOUSE_MOVE || kind == INEV_WHEEL ) {
			if ( f == 1 ) dx = body.Double(); else if ( f == 2 ) dy = body.Double(); else body.Skip( t );
		} else {
			if ( f == 1 ) intA = body.Int32(); else if ( f == 2 ) down = body.ReadBool(); else body.Skip( t );
		}
	}
	switch ( kind ) {
	case INEV_KEY: {
		int key = KeyCodeToTdmKey( intA );
		if ( key ) Sys_InjectKeyEvent( key, down );
		break;
	}
	case INEV_BUTTON:
		// MouseButton: 1 left, 2 right, 3 middle, 4 back, 5 forward -> K_MOUSE1..5
		if ( intA >= 1 && intA <= 5 ) Sys_InjectMouseButton( intA - 1, down );
		break;
	case INEV_MOUSE_MOVE: {
		// raw counts, +y down: the same convention as the window system's relative motion
		mouseCarryX += dx;
		mouseCarryY += dy;
		int ix = (int)mouseCarryX, iy = (int)mouseCarryY;
		mouseCarryX -= ix;
		mouseCarryY -= iy;
		Sys_InjectMouseDelta( ix, iy );
		break;
	}
	case INEV_WHEEL: {
		int notches = (int)( dy > 0 ? dy + 0.5 : dy - 0.5 );
		if ( notches ) Sys_InjectMouseWheel( notches );
		break;
	}
	default:
		break;
	}
}

void CArcadeIntegration::PollInput() {
	if ( !sdk.poll_input ) {
		return;
	}
	size_t n = 0;
	ArcadeStatus status = sdk.poll_input( 0, framesSubmitted.load(), 0, inputBuf.data(), inputBuf.size(), &n );
	if ( status == ARCADE_STATUS_BUFFER_TOO_SMALL ) {
		inputBuf.resize( n );
		return;
	}
	if ( status != ARCADE_STATUS_OK ) {
		if ( inputWarnings++ < 5 ) {
			Log( LOG_WARN, "arcade_poll_input failed (%d): %s", (int)status, sdk.last_error() );
		}
		return;
	}
	if ( n == 0 ) {
		return;
	}
	// Input { 1 frame_index, 2 driven, 3 keys_down[], 4 buttons_down[], 5 mouse_dx, 6 mouse_dy,
	//         7 wheel_dx, 8 wheel_dy, 9 events[], 10 dropped_events }
	// The ordered edges (9) carry everything, motion included, so they are what we apply;
	// the held-state and sums would double-count them.
	ArcadeProto::Reader in( inputBuf.data(), n );
	bool driven = false;
	uint32_t dropped = 0;
	std::vector<ArcadeProto::Reader> events;
	int field;
	ArcadeProto::WireType wt;
	while ( in.Next( field, wt ) ) {
		switch ( field ) {
		case 2: driven = in.ReadBool(); break;
		case 9: events.push_back( in.ReadMessage() ); break;
		case 10: dropped = (uint32_t)in.Varint(); break;
		default: in.Skip( wt ); break;
		}
	}
	if ( in.HadError() ) {
		if ( inputWarnings++ < 5 ) Log( LOG_WARN, "undecodable Input (%zu bytes)", n );
		return;
	}
	if ( !driven ) {
		return;
	}
	for ( size_t i = 0; i < events.size(); i++ ) {
		ApplyInputEvent( events[i] );
	}
	if ( dropped && inputWarnings++ < 5 ) {
		Log( LOG_WARN, "agent input: %u events dropped by the SDK this step", dropped );
	}
}

// ===========================================================================
// Reports
// ===========================================================================

double CArcadeIntegration::GameTimeS() const {
	// Monotonic engine clock since launch: orders reports across map reloads,
	// unlike gameLocal.time which restarts with every map.
	return Sys_Milliseconds() * 0.001;
}

void CArcadeIntegration::ReportChallengeStarted() {
	ArcadeProto::Writer started;	// ChallengeStarted { 1 challenge_id, 2 run_id }
	started.String( 1, attempt.challengeId );
	started.String( 2, attempt.runId );
	ArcadeProto::Writer report;
	report.Double( 2, GameTimeS() );
	report.PutMessageAlways( REPORT_CHALLENGE_STARTED, started );
	sdk.report( 0, report.Data(), report.Size() );
}

void CArcadeIntegration::ReportEvent( int oneofField, const ArcadeProto::Writer &body ) {
	ArcadeProto::Writer event;	// thedarkmod.v1.GameEvent
	event.PutMessageAlways( oneofField, body );
	ArcadeProto::Writer report;
	report.Double( 2, GameTimeS() );
	report.Bytes( REPORT_EVENT, event.Data(), event.Size() );
	ArcadeStatus status = sdk.report( 0, report.Data(), report.Size() );
	if ( status != ARCADE_STATUS_OK ) {
		common->Warning( "Arcade SDK: arcade_report(event %d) failed (%d): %s", oneofField, (int)status, sdk.last_error() );
	}
}

void CArcadeIntegration::Log( int level, const char *fmt, ... ) {
	char text[1024];
	va_list args;
	va_start( args, fmt );
	idStr::vsnPrintf( text, sizeof( text ), fmt, args );
	va_end( args );
	common->Printf( "Arcade: %s\n", text );
	if ( sdk.IsLoaded() && sdk.log ) {
		sdk.log( level, "tdm", text );
	}
}
