/* host.c — a complete minimal integration of the arcade game SDK in C.
 *
 * Stands in for a game with one instance: registers
 * two challenges and a metric, then runs a 60 Hz "frame loop" that serves
 * every RPC the SDK hands it, submits the frame the agent sees (a square on a
 * field), takes the agent's input (WASD / arrows / the mouse move the
 * square) and pushes a metric sample per frame. Protobuf encoding uses protobuf-c
 * (https://github.com/protobuf-c/protobuf-c): generate the message code with
 *
 *     protoc -I<sdk>/proto --c_out=. arcade_sdk.proto arcade_common.proto
 *
 * and build with
 *
 *     cl host.c arcade_sdk.pb-c.c arcade_common.pb-c.c protobuf-c.c \
 *        /I<sdk>/include /link arcade_sdk.lib
 *
 * or, on Linux,
 *
 *     cc host.c arcade_sdk.pb-c.c arcade_common.pb-c.c protobuf-c.c \
 *        -I<sdk>/include -L<sdk> -larcade_sdk -Wl,-rpath,'$ORIGIN'
 *
 * With any other protobuf library the shape is identical: encode an
 * InitRequest, poll RpcRequests, decode the request message named by
 * (service, method), encode the matching response.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

#include "arcade_sdk.h"
#include "arcade_sdk.pb-c.h"

#define CHALLENGES_SERVICE "arcade.sdk.v1.ArcadeChallenges"

static double game_time_s(void) {
    static double t = 0.0;
    return t += 1.0 / 60.0;
}

static void check(const char *what, ArcadeStatus status) {
    if (status != ARCADE_STATUS_OK) {
        fprintf(stderr, "%s failed (%d): %s\n", what, (int)status, arcade_last_error());
    }
}

#define WIDTH 640
#define HEIGHT 360

/* The one world: where the player's square is. */
static float player_x = WIDTH / 2, player_y = HEIGHT / 2;
static uint8_t pixels[WIDTH * HEIGHT * 4];

/* Send one encoded Report about `instance`. */
static void report(uint32_t instance, Arcade__Sdk__V1__Report *r) {
    size_t len = arcade__sdk__v1__report__get_packed_size(r);
    uint8_t *buf = malloc(len);
    arcade__sdk__v1__report__pack(r, buf);
    check("arcade_report", arcade_report(instance, buf, len));
    free(buf);
}

static bool held(const Arcade__Sdk__V1__Input *in, Arcade__Sdk__V1__KeyCode a, Arcade__Sdk__V1__KeyCode b) {
    for (size_t i = 0; i < in->n_keys_down; i++) {
        if (in->keys_down[i] == a || in->keys_down[i] == b) return true;
    }
    return false;
}

/* Move the square by what the agent holds and how it moved the mouse. */
static void apply_input(const Arcade__Sdk__V1__Input *in) {
    const float speed = 4.0f;
    if (held(in, ARCADE__SDK__V1__KEY_CODE__KEY_CODE_KEY_W, ARCADE__SDK__V1__KEY_CODE__KEY_CODE_ARROW_UP)) player_y -= speed;
    if (held(in, ARCADE__SDK__V1__KEY_CODE__KEY_CODE_KEY_S, ARCADE__SDK__V1__KEY_CODE__KEY_CODE_ARROW_DOWN)) player_y += speed;
    if (held(in, ARCADE__SDK__V1__KEY_CODE__KEY_CODE_KEY_A, ARCADE__SDK__V1__KEY_CODE__KEY_CODE_ARROW_LEFT)) player_x -= speed;
    if (held(in, ARCADE__SDK__V1__KEY_CODE__KEY_CODE_KEY_D, ARCADE__SDK__V1__KEY_CODE__KEY_CODE_ARROW_RIGHT)) player_x += speed;
    player_x += (float)in->mouse_dx * 0.5f;
    player_y += (float)in->mouse_dy * 0.5f;
    if (player_x < 0) player_x = 0;
    if (player_x > WIDTH - 1) player_x = WIDTH - 1;
    if (player_y < 0) player_y = 0;
    if (player_y > HEIGHT - 1) player_y = HEIGHT - 1;
}

/* What the agent sees: a green field with a white square at the player. */
static void render(void) {
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            uint8_t *p = &pixels[(y * WIDTH + x) * 4];
            bool square = abs(x - (int)player_x) < 12 && abs(y - (int)player_y) < 12;
            p[0] = square ? 255 : 40;
            p[1] = square ? 255 : 140;
            p[2] = square ? 255 : 40;
            p[3] = 255;
        }
    }
}

static void respond_start(uint64_t request_id, uint32_t instance,
                          const Arcade__Sdk__V1__StartChallengeRequest *req) {
    /* The instruction template with the variation values substituted; a real
     * game formats it from its own challenge table. */
    char instruction[256];
    snprintf(instruction, sizeof instruction, "Do challenge %s with %zu variations",
             req->challenge_id, req->n_variations);
    Arcade__Sdk__V1__StartChallengeResponse resp = ARCADE__SDK__V1__START_CHALLENGE_RESPONSE__INIT;
    resp.instruction = instruction;
    resp.game_time_s = game_time_s();
    size_t len = arcade__sdk__v1__start_challenge_response__get_packed_size(&resp);
    uint8_t *buf = malloc(len);
    arcade__sdk__v1__start_challenge_response__pack(&resp, buf);
    check("arcade_respond", arcade_respond(request_id, buf, len));
    free(buf);

    Arcade__Sdk__V1__ChallengeStarted started = ARCADE__SDK__V1__CHALLENGE_STARTED__INIT;
    started.challenge_id = req->challenge_id;
    started.run_id = req->run_id;
    Arcade__Sdk__V1__Report r = ARCADE__SDK__V1__REPORT__INIT;
    r.game_time_s = game_time_s();
    r.body_case = ARCADE__SDK__V1__REPORT__BODY_CHALLENGE_STARTED;
    r.challenge_started = &started;
    report(instance, &r);
}

static void respond_empty(uint64_t request_id, const ProtobufCMessage *msg) {
    size_t len = protobuf_c_message_get_packed_size(msg);
    uint8_t *buf = malloc(len ? len : 1);
    protobuf_c_message_pack(msg, buf);
    check("arcade_respond", arcade_respond(request_id, buf, len));
    free(buf);
}

/* Serve one polled request (for `req->instance`; this game hosts only 0).
 * Vendor services would be dispatched here too. */
static void serve(const Arcade__Sdk__V1__RpcRequest *req) {
    if (strcmp(req->service, CHALLENGES_SERVICE) == 0) {
        if (strcmp(req->method, "StartChallenge") == 0) {
            Arcade__Sdk__V1__StartChallengeRequest *r =
                arcade__sdk__v1__start_challenge_request__unpack(NULL, req->request.len, req->request.data);
            if (!r) { arcade_fail(req->request_id, "bad StartChallengeRequest"); return; }
            printf("start challenge %s\n", r->challenge_id);
            respond_start(req->request_id, req->instance, r);
            arcade__sdk__v1__start_challenge_request__free_unpacked(r, NULL);
            return;
        }
        if (strcmp(req->method, "StopChallenge") == 0) {
            Arcade__Sdk__V1__StopChallengeResponse resp = ARCADE__SDK__V1__STOP_CHALLENGE_RESPONSE__INIT;
            respond_empty(req->request_id, &resp.base);
            return;
        }
        if (strcmp(req->method, "Ping") == 0) {
            Arcade__Sdk__V1__PingResponse resp = ARCADE__SDK__V1__PING_RESPONSE__INIT;
            resp.game_time_s = game_time_s();
            respond_empty(req->request_id, &resp.base);
            return;
        }
    }
    arcade_fail(req->request_id, "unhandled method");
}

int main(void) {
    if (arcade_abi_version() != ARCADE_SDK_ABI_VERSION) {
        fprintf(stderr, "arcade_sdk.dll ABI %u, header %u\n", arcade_abi_version(), ARCADE_SDK_ABI_VERSION);
        return 1;
    }
    printf("arcade_sdk %s\n", arcade_version());

    /* ---- registration -------------------------------------------------- */
    Arcade__Sdk__V1__EnumVariation color_enum = ARCADE__SDK__V1__ENUM_VARIATION__INIT;
    char *colors[] = {"red", "blue", "green"};
    color_enum.n_values = 3; color_enum.values = colors; color_enum.default_ = "red";
    Arcade__Sdk__V1__VariationDef color = ARCADE__SDK__V1__VARIATION_DEF__INIT;
    color.name = "color"; color.kind_case = ARCADE__SDK__V1__VARIATION_DEF__KIND_ENUM; color.enum_ = &color_enum;

    Arcade__Sdk__V1__IntVariation seconds_int = ARCADE__SDK__V1__INT_VARIATION__INIT;
    seconds_int.min = 10; seconds_int.max = 120; seconds_int.default_ = 60; seconds_int.step = 10;
    Arcade__Sdk__V1__VariationDef seconds = ARCADE__SDK__V1__VARIATION_DEF__INIT;
    seconds.name = "seconds"; seconds.kind_case = ARCADE__SDK__V1__VARIATION_DEF__KIND_INT; seconds.int_ = &seconds_int;

    Arcade__Sdk__V1__VariationDef *beacon_vars[] = {&color, &seconds};
    Arcade__Sdk__V1__Challenge beacon = ARCADE__SDK__V1__CHALLENGE__INIT;
    beacon.id = "reach-the-beacon";
    beacon.display_name = "Reach the beacon";
    beacon.instruction = "Reach the {color} beacon within {seconds} seconds";
    beacon.n_variations = 2; beacon.variations = beacon_vars;
    beacon.default_timeout_s = 120;

    Arcade__Sdk__V1__Challenge coins = ARCADE__SDK__V1__CHALLENGE__INIT;
    coins.id = "collect-coins";
    coins.display_name = "Collect coins";
    coins.instruction = "Collect five coins";
    coins.default_timeout_s = 60;

    Arcade__Sdk__V1__Challenge *challenges[] = {&beacon, &coins};

    Arcade__Sdk__V1__MetricDefinition distance = ARCADE__SDK__V1__METRIC_DEFINITION__INIT;
    distance.name = "distance/travelled_m";
    distance.kind = ARCADE__SDK__V1__METRIC_KIND__METRIC_KIND_SCALAR;
    distance.unit = "m";
    Arcade__Sdk__V1__MetricDefinition *metrics[] = {&distance};

    char *services[] = {CHALLENGES_SERVICE};

    /* The world frame: required. This stand-in pretends to be a Unity game. */
    Arcade__Sdk__V1__CoordinateSystem frame = ARCADE__SDK__V1__COORDINATE_SYSTEM__INIT;
    frame.up = ARCADE__SDK__V1__AXIS__AXIS_POS_Y;
    frame.handedness = ARCADE__SDK__V1__HANDEDNESS__HANDEDNESS_LEFT;
    frame.euler_order = ARCADE__SDK__V1__EULER_ORDER__EULER_ORDER_YAW_PITCH_ROLL;

    Arcade__Sdk__V1__InitRequest init = ARCADE__SDK__V1__INIT_REQUEST__INIT;
    init.game_id = "acme-host-c";
    init.build_id = "example";
    init.abi_version = ARCADE_SDK_ABI_VERSION;
    init.n_challenges = 2; init.challenges = challenges;
    init.n_metrics = 1; init.metrics = metrics;
    init.n_services = 1; init.services = services;
    init.coordinate_system = &frame;
    /* Submits 640x360 frames; one instance. */
    Arcade__Sdk__V1__VideoSize video = ARCADE__SDK__V1__VIDEO_SIZE__INIT;
    video.width = WIDTH;
    video.height = HEIGHT;
    init.video = &video;
    init.max_instances = 1;
    /* What each input does (see apply_input() above): what the agent is told its
     * controls are. Keys by their W3C KeyboardEvent.code. */
    static char *actions[][2] = {
        {"KeyW", "Move up"}, {"ArrowUp", "Move up"},
        {"KeyS", "Move down"}, {"ArrowDown", "Move down"},
        {"KeyA", "Move left"}, {"ArrowLeft", "Move left"},
        {"KeyD", "Move right"}, {"ArrowRight", "Move right"},
        {"MouseMove", "Move the square"},
    };
    enum { N_ACTIONS = sizeof actions / sizeof actions[0] };
    Arcade__Sdk__V1__InitRequest__ActionMapEntry action_entries[N_ACTIONS];
    Arcade__Sdk__V1__InitRequest__ActionMapEntry *action_map[N_ACTIONS];
    for (size_t k = 0; k < N_ACTIONS; k++) {
        arcade__sdk__v1__init_request__action_map_entry__init(&action_entries[k]);
        action_entries[k].key = actions[k][0];
        action_entries[k].value = actions[k][1];
        action_map[k] = &action_entries[k];
    }
    init.n_action_map = N_ACTIONS; init.action_map = action_map;
    /* No vendor.proto in this example: no vendor_descriptor_set, no event_type. */

    size_t init_len = arcade__sdk__v1__init_request__get_packed_size(&init);
    uint8_t *init_buf = malloc(init_len);
    arcade__sdk__v1__init_request__pack(&init, init_buf);
    ArcadeStatus status = arcade_init(init_buf, init_len);
    free(init_buf);
    if (status != ARCADE_STATUS_OK) {
        fprintf(stderr, "arcade_init failed (%d): %s\n", (int)status, arcade_last_error());
        return 1;
    }
    uint32_t distance_metric = arcade_metric_handle("distance/travelled_m");

    /* ---- the frame loop ------------------------------------------------ */
    uint8_t *buf = malloc(ARCADE_MAX_REQUEST_BYTES);
    uint8_t *input_buf = malloc(ARCADE_MAX_INPUT_BYTES);
    for (uint64_t frame = 0; frame < 60 * 30; frame++) {      /* 30 seconds */
        size_t n = 0;
        while (arcade_poll_request(buf, ARCADE_MAX_REQUEST_BYTES, &n) == ARCADE_STATUS_OK && n > 0) {
            Arcade__Sdk__V1__RpcRequest *req = arcade__sdk__v1__rpc_request__unpack(NULL, n, buf);
            if (!req) { fprintf(stderr, "undecodable RpcRequest\n"); continue; }
            serve(req);
            arcade__sdk__v1__rpc_request__free_unpacked(req, NULL);
        }

        /* What the agent sees at this step... */
        render();
        ArcadeFrame f = {0};
        f.frame_index = frame;
        f.game_time_s = game_time_s();
        f.pixels = pixels;
        f.width = WIDTH;
        f.height = HEIGHT;
        f.format = ARCADE_PIXEL_FORMAT_RGBA8;
        check("arcade_submit_frame", arcade_submit_frame(0, &f));

        /* ...and what it did since the last frame, applied before the next
         * step. Returns at once. */
        size_t in_len = 0;
        ArcadeStatus polled = arcade_poll_input(0, frame, input_buf, ARCADE_MAX_INPUT_BYTES, &in_len);
        if (polled == ARCADE_STATUS_OK) {
            Arcade__Sdk__V1__Input *in = arcade__sdk__v1__input__unpack(NULL, in_len, input_buf);
            if (in) {
                apply_input(in);
                arcade__sdk__v1__input__free_unpacked(in, NULL);
            }
        } else {
            check("arcade_poll_input", polled);
        }

        check("arcade_push_f32_metric",
              arcade_push_f32_metric(0, distance_metric, player_x / WIDTH * 10.0f, game_time_s()));
        SLEEP_MS(16);
    }
    free(input_buf);
    free(buf);

    /* ---- shutdown ------------------------------------------------------ */
    status = arcade_shutdown();
    if (status == ARCADE_STATUS_SHUTDOWN_INCOMPLETE) {
        fprintf(stderr, "SDK threads still running; not unloading: %s\n", arcade_last_error());
    }
    return status == ARCADE_STATUS_OK ? 0 : 1;
}
