
#include "main.h"
#include "editor/editor.h"
#include "eol/settings.h"
#include "game/game.h"
#include "game/level_load.h"
#include "game/qopen.h"
#include "game/recorder.h"
#include "game/state.h"
#include "menu/pic.h"
#include "menu/play.h"
#include "menu/rec_list.h"
#include "physics/init.h"
#include "pic/abc8.h"
#include "pic/pic8.h"
#include "platform/implementation.h"
#include "renderer/render.h"
#include <emscripten.h>

constexpr int FPS_TEST = 1;

long long last_time = 0;

void main_loop() {
    long long new_time = get_milliseconds();
    printf("%d\n", new_time - last_time);
    last_time = new_time;

    extern int game_loop_frame();
    game_loop_frame();
}

void async_main_loop(void* arg) {
    emscripten_async_call(async_main_loop, nullptr, 0);
    main_loop();
}

void load_level() {
    finame filename = "QWQUU003.LEV";

    bool map_viewer = configure_game();
    loading_screen();

    load_level_play(filename);
    Rec1->erase(filename);
    Rec2->erase(filename);

    extern void game_loop_init(const char* filename, CameraMode camera_mode);
    game_loop_init(filename, CameraMode::Normal);
}

void main_emscripten() {
    init_qopen();

    init_menu_pictures();

    State = new state;
    if (!State) {
        internal_error("memory");
    }

    merge_states();
    eol_settings::sync_controls_to_state(State);

    init_physics_data();

    rec_list::build_cache();

    // Load globals
    EditorWhiteFont = new abc8("kisbetu1.abc", 1, 19); // "small letter 1"
    EditorBlackFont = new abc8("kisbetu2.abc", 1, 19); // "small letter 2"

    init_renderer();

    Rec1 = new recorder;
    Rec2 = new recorder;

    create_editor_palette();

    // Initialize stopwatch, just in case
    stopwatch_reset();

    // Test
    test_player();
    strcpy(State->player1, "Test");
    strcpy(State->player2, "Test");

    // Settings
    EolSettings->persist_show_fps(true);
    EolSettings->persist_show_ups(true);

    load_level();

    // Test 1: Synced to screen Hz (requestAnimationFrame)
    if (FPS_TEST == 1) {
        emscripten_set_main_loop(main_loop, 0, false);
    }

    // Test 2: Best FPS (setTimeout)
    // According to HTML spec, the fastest tick rate that is possible is 4 ms (i.e. 250 FPS)
    // https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#timers
    if (FPS_TEST == 2) {
        emscripten_set_main_loop(main_loop, 2000, false);
        emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 0);
    }

    // Test 3: faster setTimeout that also works when minimized
    if (FPS_TEST == 3) {
        async_main_loop(nullptr);
    }
}
