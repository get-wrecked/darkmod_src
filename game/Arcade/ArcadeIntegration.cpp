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
#include "../../framework/Licensee.h"
#include "../../idlib/RevisionTracker.h"

#include <stdarg.h>

CArcadeIntegration arcadeIntegration;

idCVar arcade_enable( "arcade_enable", "0", CVAR_GAME | CVAR_BOOL | CVAR_INIT,
	"Load the arcade game SDK (libarcade_sdk.so / arcade_sdk.dll next to the executable) and expose the game as an agent research environment. Set on the command line: +set arcade_enable 1" );
idCVar arcade_autoReady( "arcade_autoReady", "1", CVAR_GAME | CVAR_BOOL,
	"When the arcade SDK is active, skip the 'press attack to start' screen after a map loads." );
idCVar arcade_metricsIntervalMs( "arcade_metricsIntervalMs", "100", CVAR_GAME | CVAR_INTEGER,
	"How often the arcade SDK receives metric samples, in milliseconds.", 0, 10000 );
idCVar arcade_mapLoadTimeoutSec( "arcade_mapLoadTimeoutSec", "60", CVAR_GAME | CVAR_INTEGER,
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
enum { EV_PLAYER_DIED = 1, EV_LOOT_PICKED_UP = 2, EV_AI_ALERT_CHANGED = 3, EV_LOCATION_CHANGED = 4, EV_PLAYER_DAMAGED = 5, EV_MISSION_COMPLETED = 6, EV_MAP_LOADED = 7 };
// arcade_log levels
enum { LOG_DEBUG = 1, LOG_INFO = 2, LOG_WARN = 3, LOG_ERROR = 4 };

static const float UNITS_TO_METERS = 0.0254f;	// idTech4: 1 unit = 1 inch
static const float LIGHTGEM_MAX = 32.0f;		// DARKMOD_LG_MAX

// Challenge ids
static const char *CH_REACH_LOCATION = "reach-location";
static const char *CH_COLLECT_LOOT = "collect-loot";
static const char *CH_STAY_HIDDEN = "stay-hidden";
static const char *CH_FREE_ROAM = "free-roam";

static const char *ALERT_NAMES[] = { "relaxed", "observant", "suspicious", "searching", "agitated_searching", "combat" };

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
	active( false ), inFrame( false ), state( STATE_IDLE ), mapGeneration( 0 ), observedGeneration( -1 ),
	lastLoot( 0 ), lastHealth( 0 ), wasDead( false ), missionCompleteReported( false ), lastMetricsPushMs( 0 ),
	mHealth( 0 ), mLoot( 0 ), mLightgem( 0 ), mMaxAlert( 0 ), mDistance( 0 )
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
	if ( !sdk.Load() ) {
		common->Warning( "Arcade SDK disabled: %s", sdk.GetError() );
		return;
	}
	common->Printf( "Loaded %s (version %s)\n", sdk.GetPath(), sdk.version() );

	startingMap = gameLocal.m_MissionManager ? gameLocal.m_MissionManager->GetCurrentStartingMap() : idStr();
	if ( startingMap.IsEmpty() ) {
		common->Warning( "Arcade SDK: no fan mission installed (fs_currentfm); challenges cannot start until one is." );
	} else {
		common->Printf( "Arcade SDK: challenges run on map '%s'\n", startingMap.c_str() );
	}
	ParseMissionLocations();

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
	Log( LOG_INFO, "The Dark Mod arcade integration ready (%d challenges, %d locations)", 4, locationNames.Num() );
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

void CArcadeIntegration::ParseMissionLocations() {
	locationNames.Clear();
	locationOrigins.Clear();
	if ( startingMap.IsEmpty() ) {
		return;
	}

	idMapFile mapFile;
	if ( !mapFile.Parse( va( "maps/%s", startingMap.c_str() ) ) ) {
		common->Warning( "Arcade SDK: could not parse map '%s' for locations", startingMap.c_str() );
		return;
	}
	for ( int i = 0; i < mapFile.GetNumEntities(); i++ ) {
		const idDict &args = mapFile.GetEntity( i )->epairs;
		if ( idStr::Icmp( args.GetString( "classname" ), "info_location" ) != 0 ) {
			continue;
		}
		const char *name = args.GetString( "name" );
		if ( !name[0] ) {
			continue;
		}
		if ( locationNames.FindIndex( name ) >= 0 ) {
			continue;
		}
		locationNames.Append( name );
		locationOrigins.Append( args.GetVector( "origin" ) );
	}
	common->Printf( "Arcade SDK: %d info_location areas in %s\n", locationNames.Num(), startingMap.c_str() );
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

	const char *SECONDS_DESC = "Time limit for the attempt";
}

const char *CArcadeIntegration::ChallengeInstructionTemplate( const std::string &id ) const {
	if ( id == CH_REACH_LOCATION ) return "Find your way to the area named '{location}' within {seconds} seconds.";
	if ( id == CH_COLLECT_LOOT ) return "Steal loot worth at least {loot} within {seconds} seconds. Pick up valuables such as coins, goblets, vases and paintings by looking at them and pressing the use (frob) key.";
	if ( id == CH_STAY_HIDDEN ) return "Survive for {seconds} seconds without dying and without any guard becoming '{max_alert}' or more alert. Stay in the shadows and out of sight.";
	if ( id == CH_FREE_ROAM ) return "Explore the level freely for {seconds} seconds. Cover as much ground as you can.";
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

	idStrList locations = locationNames;
	if ( locations.Num() == 0 ) {
		locations.Append( "none" );
	}
	idStrList alerts;
	alerts.Append( ALERT_NAMES[ai::ESuspicious] );
	alerts.Append( ALERT_NAMES[ai::ESearching] );
	alerts.Append( ALERT_NAMES[ai::EAgitatedSearching] );
	alerts.Append( ALERT_NAMES[ai::ECombat] );

	// --- reach-location
	{
		Writer c;
		c.String( 1, CH_REACH_LOCATION );
		c.String( 2, "Reach a location" );
		c.String( 3, ChallengeInstructionTemplate( CH_REACH_LOCATION ) );
		c.String( 4, "Navigate from the mission start to a named info_location area of the map. Judged by the location system; success when the player stands inside the target area." );
		AddEnumVar( c, "location", "Target info_location entity name", locations, locations[0].c_str() );
		AddIntVar( c, "seconds", SECONDS_DESC, 30, 900, 180, 30 );
		c.String( 6, "player/distance_travelled_m" );
		c.String( 6, "ai/max_alert_index" );
		c.UInt32( 7, 900 );
		init.PutMessage( 4, c );
	}
	// --- collect-loot
	{
		Writer c;
		c.String( 1, CH_COLLECT_LOOT );
		c.String( 2, "Collect loot" );
		c.String( 3, ChallengeInstructionTemplate( CH_COLLECT_LOOT ) );
		c.String( 4, "Accumulate loot value (gold + jewelry + goods) by frobbing loot items. Success when the carried total reaches the target." );
		AddIntVar( c, "loot", "Loot value to collect", 25, 2000, 100, 25 );
		AddIntVar( c, "seconds", SECONDS_DESC, 60, 1800, 300, 30 );
		c.String( 6, "player/loot" );
		c.String( 6, "ai/max_alert_index" );
		c.UInt32( 7, 1800 );
		init.PutMessage( 4, c );
	}
	// --- stay-hidden
	{
		Writer c;
		c.String( 1, CH_STAY_HIDDEN );
		c.String( 2, "Stay hidden" );
		c.String( 3, ChallengeInstructionTemplate( CH_STAY_HIDDEN ) );
		c.String( 4, "Survive the time limit. Fails immediately if the player dies or any living AI reaches the chosen alert index (2 suspicious, 3 searching, 4 agitated searching, 5 combat)." );
		AddIntVar( c, "seconds", "How long to survive", 30, 900, 120, 30 );
		AddEnumVar( c, "max_alert", "Alert state that fails the attempt", alerts, ALERT_NAMES[ai::ESearching] );
		c.String( 6, "ai/max_alert_index" );
		c.String( 6, "player/lightgem" );
		c.UInt32( 7, 900 );
		init.PutMessage( 4, c );
	}
	// --- free-roam
	{
		Writer c;
		c.String( 1, CH_FREE_ROAM );
		c.String( 2, "Free roam" );
		c.String( 3, ChallengeInstructionTemplate( CH_FREE_ROAM ) );
		c.String( 4, "Baseline: always succeeds at the time limit (unless the player dies). Score is distance travelled in metres." );
		AddIntVar( c, "seconds", "Duration", 30, 1800, 120, 30 );
		c.String( 6, "player/distance_travelled_m" );
		c.UInt32( 7, 1800 );
		init.PutMessage( 4, c );
	}

	AddMetricDef( init, "player/health", METRIC_SCALAR, "hp", "Player health, 100 = full" );
	AddMetricDef( init, "player/loot", METRIC_COUNTER, "value", "Total loot value carried" );
	AddMetricDef( init, "player/lightgem", METRIC_SCALAR, "ratio", "How lit the player is, 0 dark .. 1 fully lit" );
	AddMetricDef( init, "ai/max_alert_index", METRIC_SCALAR, "index", "Highest alert index of any living AI (0 relaxed .. 5 combat)" );
	AddMetricDef( init, "player/distance_travelled_m", METRIC_COUNTER, "m", "Distance the player has moved during the attempt" );

	init.Bytes( 6, arcade_vendor_pb, arcade_vendor_pb_len );
	init.String( 7, CHALLENGES_SERVICE );
	init.String( 7, VENDOR_SERVICE );
	init.String( 8, VENDOR_EVENT_TYPE );

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
	// RpcRequest { 1 request_id, 2 service, 3 method, 4 timeout_ms, 5 request }
	ArcadeProto::Reader r( data, len );
	uint64_t reqId = 0;
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
		default: r.Skip( wt ); break;
		}
	}
	if ( r.HadError() || reqId == 0 ) {
		Log( LOG_WARN, "undecodable RpcRequest (%zu bytes)", len );
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

	if ( !ChallengeInstructionTemplate( a.challengeId )[0] ) {
		Fail( reqId, "unknown challenge '%s'", a.challengeId.c_str() );
		return;
	}
	if ( startingMap.IsEmpty() ) {
		Fail( reqId, "no fan mission installed; launch with +set fs_currentfm <mission>" );
		return;
	}
	if ( a.challengeId == CH_REACH_LOCATION ) {
		VarMap::const_iterator it = a.vars.find( "location" );
		if ( it == a.vars.end() || locationNames.FindIndex( it->second.s.c_str() ) < 0 ) {
			Fail( reqId, "unknown location '%s'", it == a.vars.end() ? "" : it->second.s.c_str() );
			return;
		}
	}
	if ( a.challengeId == CH_STAY_HIDDEN ) {
		VarMap::const_iterator it = a.vars.find( "max_alert" );
		if ( it == a.vars.end() || AlertThresholdFromName( it->second.s ) < 0 ) {
			Fail( reqId, "unknown alert state '%s'", it == a.vars.end() ? "" : it->second.s.c_str() );
			return;
		}
	}
	VarMap::const_iterator sec = a.vars.find( "seconds" );
	a.limitSeconds = ( sec != a.vars.end() && sec->second.kind == VarValue::INT ) ? (int)sec->second.i : 0;

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

	Log( LOG_INFO, "StartChallenge %s run=%s seed=%llu: loading map %s",
		a.challengeId.c_str(), a.runId.c_str(), (unsigned long long)a.seed, startingMap.c_str() );
	// Reloading the map is our "reset the world": executed by the event loop on the next frame.
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, va( "map %s\n", startingMap.c_str() ) );
}

void CArcadeIntegration::AdvanceLoading() {
	if ( mapGeneration > attempt.mapGenerationAtRequest && gameLocal.GameState() == GAMESTATE_ACTIVE && gameLocal.GetLocalPlayer() ) {
		StartRunning();
		return;
	}
	if ( Sys_Milliseconds() - attempt.loadIssuedMs > arcade_mapLoadTimeoutSec.GetInteger() * 1000 ) {
		Fail( attempt.pendingRequestId, "map '%s' did not load within %d seconds", startingMap.c_str(), arcade_mapLoadTimeoutSec.GetInteger() );
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
	// The fresh map resets the observers as well (see Observe), so events from the
	// previous attempt are not carried over.

	ArcadeProto::Writer resp;	// StartChallengeResponse { 1 instruction, 2 game_time_s }
	resp.String( 1, ResolveInstruction( attempt.challengeId, attempt.vars ) );
	resp.Double( 2, GameTimeS() );
	Respond( attempt.pendingRequestId, resp );
	attempt.pendingRequestId = 0;
	state = STATE_RUNNING;

	ReportChallengeStarted();
	Log( LOG_INFO, "challenge %s running (limit %ds)", attempt.challengeId.c_str(), attempt.limitSeconds );
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

std::string CArcadeIntegration::ResolveInstruction( const std::string &challengeId, const VarMap &vars ) const {
	std::string text = ChallengeInstructionTemplate( challengeId );
	for ( VarMap::const_iterator it = vars.begin(); it != vars.end(); ++it ) {
		std::string placeholder = "{" + it->first + "}";
		std::string value = it->second.ToString();
		size_t pos;
		while ( ( pos = text.find( placeholder ) ) != std::string::npos ) {
			text.replace( pos, placeholder.size(), value );
		}
	}
	return text;
}

int CArcadeIntegration::AlertThresholdFromName( const std::string &name ) const {
	for ( int i = 0; i < ai::EAlertStateNum; i++ ) {
		if ( name == ALERT_NAMES[i] ) {
			return i;
		}
	}
	return -1;
}

// ===========================================================================
// Judging
// ===========================================================================

void CArcadeIntegration::Judge() {
	idPlayer *player = gameLocal.GetLocalPlayer();
	if ( gameLocal.GameState() != GAMESTATE_ACTIVE && gameLocal.GameState() != GAMESTATE_COMPLETED ) {
		return;	// map is going away; OnMapShutdown() reports the abort
	}
	if ( !player ) {
		return;
	}

	// bookkeeping
	const idVec3 &origin = player->GetPhysics()->GetOrigin();
	float step = ( origin - attempt.lastOrigin ).Length();
	if ( step < 200.0f ) {	// ignore teleports
		attempt.distanceUnits += step;
	}
	attempt.lastOrigin = origin;
	idAI *culprit = nullptr;
	int maxAlert = MaxAiAlertIndex( &culprit );
	attempt.maxAlertSeen = Max( attempt.maxAlertSeen, maxAlert );

	double elapsed = ( gameLocal.time - attempt.startGameTime ) * 0.001;
	bool timeUp = attempt.limitSeconds > 0 && elapsed >= attempt.limitSeconds;
	bool dead = player->health <= 0;

	if ( dead ) {
		CompleteAttempt( OUTCOME_FAILURE, 0.0, "player died" );
		return;
	}

	if ( attempt.challengeId == CH_REACH_LOCATION ) {
		const std::string &target = attempt.vars["location"].s;
		if ( target == PlayerLocationName( player ) ) {
			double score = attempt.limitSeconds > 0 ? Max( 0.0, 1.0 - elapsed / attempt.limitSeconds ) : 1.0;
			CompleteAttempt( OUTCOME_SUCCESS, score, va( "reached %s after %.1f s", target.c_str(), elapsed ) );
			return;
		}
	} else if ( attempt.challengeId == CH_COLLECT_LOOT ) {
		int target = (int)attempt.vars["loot"].i;
		int loot = PlayerLoot( player );
		if ( loot >= target ) {
			CompleteAttempt( OUTCOME_SUCCESS, 1.0, va( "carrying %d loot (target %d) after %.1f s", loot, target, elapsed ) );
			return;
		}
		if ( timeUp ) {
			CompleteAttempt( OUTCOME_TIMEOUT, target > 0 ? (double)loot / target : 0.0, va( "%d/%d loot when time ran out", loot, target ) );
			return;
		}
	} else if ( attempt.challengeId == CH_STAY_HIDDEN ) {
		int threshold = AlertThresholdFromName( attempt.vars["max_alert"].s );
		if ( maxAlert >= threshold ) {
			CompleteAttempt( OUTCOME_FAILURE, attempt.limitSeconds > 0 ? elapsed / attempt.limitSeconds : 0.0,
				va( "%s reached alert '%s' after %.1f s", culprit ? culprit->GetName() : "an AI", ALERT_NAMES[Min( maxAlert, (int)ai::ECombat )], elapsed ) );
			return;
		}
		if ( timeUp ) {
			CompleteAttempt( OUTCOME_SUCCESS, 1.0, va( "survived %d s undetected", attempt.limitSeconds ) );
			return;
		}
	} else if ( attempt.challengeId == CH_FREE_ROAM ) {
		if ( timeUp ) {
			CompleteAttempt( OUTCOME_SUCCESS, attempt.distanceUnits * UNITS_TO_METERS, va( "travelled %.1f m", attempt.distanceUnits * UNITS_TO_METERS ) );
			return;
		}
	}

	if ( timeUp ) {
		CompleteAttempt( OUTCOME_TIMEOUT, 0.0, va( "time limit of %d s reached", attempt.limitSeconds ) );
	}
}

void CArcadeIntegration::CompleteAttempt( Outcome outcome, double score, const char *detail ) {
	if ( state != STATE_RUNNING ) {
		return;
	}
	state = STATE_IDLE;

	double elapsed = ( gameLocal.time - attempt.startGameTime ) * 0.001;
	idPlayer *player = gameLocal.GetLocalPlayer();

	// ChallengeCompleted { 1 challenge_id, 2 run_id, 3 outcome, 4 score, 5 detail, 6 final_metrics[] }
	ArcadeProto::Writer done;
	done.String( 1, attempt.challengeId );
	done.String( 2, attempt.runId );
	done.Enum( 3, outcome );
	done.Double( 4, score );
	done.String( 5, detail );
	{
		ArcadeProto::Writer m;
		m.String( 1, "time/elapsed_s" ); m.Double( 2, elapsed );
		done.PutMessage( 6, m );
	}
	{
		ArcadeProto::Writer m;
		m.String( 1, "player/distance_travelled_m" ); m.Double( 2, attempt.distanceUnits * UNITS_TO_METERS );
		done.PutMessageAlways( 6, m );
	}
	{
		ArcadeProto::Writer m;
		m.String( 1, "ai/max_alert_index" ); m.Double( 2, attempt.maxAlertSeen );
		done.PutMessageAlways( 6, m );
	}
	if ( player ) {
		ArcadeProto::Writer m;
		m.String( 1, "player/loot" ); m.Double( 2, PlayerLoot( player ) );
		done.PutMessageAlways( 6, m );
	}

	ArcadeProto::Writer report;
	report.Double( 2, GameTimeS() );
	report.PutMessageAlways( REPORT_CHALLENGE_COMPLETED, done );
	sdk.report( report.Data(), report.Size() );

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
	ArcadeProto::Writer resp;
	for ( int i = 0; i < locationNames.Num(); i++ ) {
		ArcadeProto::Writer loc;
		loc.String( 1, locationNames[i].c_str() );
		WriteVec3( loc, 2, locationOrigins[i] );
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
	missionCompleteReported = false;
	lastLocation = "";
	lastAiAlert.clear();
	lastMetricsPushMs = 0;
}

void CArcadeIntegration::Observe() {
	gameState_t gs = gameLocal.GameState();
	if ( gs != GAMESTATE_ACTIVE && gs != GAMESTATE_COMPLETED ) {
		observedGeneration = -1;
		return;
	}
	idPlayer *player = gameLocal.GetLocalPlayer();
	if ( !player ) {
		return;
	}

	if ( observedGeneration != mapGeneration ) {
		// first frame on a new map
		observedGeneration = mapGeneration;
		ResetObservers();
		lastLoot = PlayerLoot( player );
		lastHealth = player->health;
		wasDead = player->health <= 0;
		lastLocation = PlayerLocationName( player );
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

	// mission end
	if ( gs == GAMESTATE_COMPLETED && !missionCompleteReported ) {
		missionCompleteReported = true;
		ReportEvent( EV_MISSION_COMPLETED, ArcadeProto::Writer() );
	}

	// metrics
	int now = Sys_Milliseconds();
	if ( now - lastMetricsPushMs >= arcade_metricsIntervalMs.GetInteger() ) {
		lastMetricsPushMs = now;
		double t = GameTimeS();
		sdk.push_f32_metric( mHealth, (float)player->health, t );
		sdk.push_f32_metric( mLoot, (float)loot, t );
		sdk.push_f32_metric( mLightgem, player->GetCurrentLightgemValue() / LIGHTGEM_MAX, t );
		sdk.push_f32_metric( mMaxAlert, (float)maxAlert, t );
		if ( state == STATE_RUNNING ) {
			sdk.push_f32_metric( mDistance, attempt.distanceUnits * UNITS_TO_METERS, t );
		}
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
	sdk.report( report.Data(), report.Size() );
}

void CArcadeIntegration::ReportEvent( int oneofField, const ArcadeProto::Writer &body ) {
	ArcadeProto::Writer event;	// thedarkmod.v1.GameEvent
	event.PutMessageAlways( oneofField, body );
	ArcadeProto::Writer report;
	report.Double( 2, GameTimeS() );
	report.Bytes( REPORT_EVENT, event.Data(), event.Size() );
	ArcadeStatus status = sdk.report( report.Data(), report.Size() );
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
