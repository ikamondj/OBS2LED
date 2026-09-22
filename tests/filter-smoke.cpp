// Windows GPU integration test. Runs a private libobs instance, never the user's OBS UI.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <obs.h>
#include "frontend-stub.hpp"
#include "global-output.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
static void require(bool condition, const char *message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

struct Pattern { gs_texture_t *texture; uint32_t width; uint32_t height; };
static const char *name(void *) { return "Test pattern"; }
static void *create(obs_data_t *settings, obs_source_t *)
{
    const bool alternate = obs_data_get_bool(settings, "alternate");
    std::vector<uint8_t> rgba(128 * 64 * 4);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 128; ++x) {
        const size_t at = (y * 128 + x) * 4;
        rgba[at] = static_cast<uint8_t>(x * 2);
        rgba[at + 1] = static_cast<uint8_t>(y * 4);
        rgba[at + 2] = (x + y) % 2 ? 255 : 0;
        rgba[at + 3] = 255;
        if (alternate) { rgba[at] = 0; rgba[at + 1] = 0; rgba[at + 2] = 255; }
    }
    const uint8_t *pixels = rgba.data();
    obs_enter_graphics();
    auto *texture = gs_texture_create(128, 64, GS_RGBA, 1, &pixels, 0);
    obs_leave_graphics();
    const bool small = obs_data_get_bool(settings, "small");
    return new Pattern{texture, small ? 64u : 128u, small ? 32u : 64u};
}
static void destroy(void *data)
{
    auto *pattern = static_cast<Pattern *>(data);
    obs_enter_graphics(); gs_texture_destroy(pattern->texture); obs_leave_graphics();
    delete pattern;
}
static uint32_t width(void *data) { return static_cast<Pattern *>(data)->width; }
static uint32_t height(void *data) { return static_cast<Pattern *>(data)->height; }
static void render(void *data, gs_effect_t *)
{
    auto *pattern = static_cast<Pattern *>(data);
    auto *texture = pattern->texture;
    auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    const bool previous = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);
    gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), texture);
    while (gs_effect_loop(effect, "Draw")) gs_draw_sprite(texture, 0, pattern->width, pattern->height);
    gs_enable_framebuffer_srgb(previous);
}

static std::vector<uint8_t> receive(SOCKET socket, int milliseconds = 2000)
{
    fd_set set; FD_ZERO(&set); FD_SET(socket, &set);
    timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
    if (select(0, &set, nullptr, nullptr, &timeout) <= 0) return {};
    std::vector<uint8_t> bytes(65536);
    const int size = recv(socket, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0);
    require(size >= 0, "UDP receive");
    bytes.resize(size);
    return bytes;
}

int main(int argc, char **argv)
{
    require(argc == 5, "Arguments: plugin DLL, plugin data directory, OBS libobs data directory, D3D11 DLL");
    WSADATA wsa{}; require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "Winsock");
    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "UDP bind");
    int address_size = sizeof(address);
    getsockname(socket, reinterpret_cast<sockaddr *>(&address), &address_size);
    require(obs_startup("en-US", nullptr, nullptr), "OBS startup");
    obs_add_data_path(argv[3]); // OBS requires a trailing slash here.
    obs_video_info video{};
    video.graphics_module = argv[4];
    video.fps_num = 60; video.fps_den = 1;
    video.base_width = video.output_width = 128;
    video.base_height = video.output_height = 64;
    video.output_format = VIDEO_FORMAT_RGBA;
    video.colorspace = VIDEO_CS_709; video.range = VIDEO_RANGE_FULL;
    video.scale_type = OBS_SCALE_BILINEAR;
    require(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "GPU initialization");
    // OBS itself already loads this DLL. The standalone test must preload it
    // because libobs loads plugins with a restricted dependency search path.
    require(LoadLibraryW(L"obs-frontend-api.dll") != nullptr, "preload OBS frontend runtime");
    obs_module_t *module = nullptr;
    require(obs_open_module(&module, argv[1], argv[2]) == MODULE_SUCCESS && obs_init_module(module), "load plugin");

    bool registered = false;
    const char *filter_id = nullptr;
    for (size_t index = 0; obs_enum_filter_types(index, &filter_id); ++index)
        registered |= std::strcmp(filter_id, "obs2led_video_filter") == 0;
    require(registered, "LED filter is listed in OBS's filter registry");
    const auto flags = obs_get_source_output_flags("obs2led_video_filter");
    require((flags & OBS_SOURCE_VIDEO) &&
            !(flags & (OBS_SOURCE_AUDIO | OBS_SOURCE_ASYNC | OBS_SOURCE_DEPRECATED | OBS_SOURCE_CAP_DISABLED)),
            "LED filter is available in Effect Filters");
    const char *filter_name = obs_source_get_display_name("obs2led_video_filter");
    require(filter_name && std::strcmp(filter_name, "OBS2LED") == 0,
            "LED filter has a recognizable menu name even without locale data");

    obs_source_info input{};
    input.id = "obs2led_pattern"; input.type = OBS_SOURCE_TYPE_INPUT;
    input.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
    input.get_name = name; input.create = create; input.destroy = destroy;
    input.get_width = width; input.get_height = height; input.video_render = render;
    obs_register_source(&input);
    auto *source = obs_source_create_private(input.id, "Pattern", nullptr);
    auto *settings = obs_data_create();
    obs_data_set_string(settings, "address", "127.0.0.1");
    obs_data_set_int(settings, "port", ntohs(address.sin_port));
    auto *filter = obs_source_create_private("obs2led_video_filter", "LED output", settings);
    require(source && filter, "source/filter creation");
    obs_source_filter_add(source, filter);
    obs_set_output_source(0, source);

    auto nearest = receive(socket);
    require(nearest.size() == 64 * 32 * 3, "default 64x32 frame reaches UDP receiver");
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 64; ++x) {
        const auto at = (y * 64 + x) * 3;
        require(std::abs(int(nearest[at]) - (4 * x + 2)) <= 1, "nearest R coordinate");
        require(std::abs(int(nearest[at + 1]) - (8 * y + 4)) <= 1, "nearest G coordinate / vertical orientation");
        require(nearest[at + 2] <= 1, "nearest selects exact texels, not interpolated checkerboard");
    }
    require(obs_source_get_width(source) == 128 && obs_source_get_height(source) == 64, "OBS dimensions preserved");
    while (!receive(socket, 0).empty()) {}
    int frames = 0;
    const auto rate_start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - rate_start < 1100ms) {
        if (!receive(socket, 100).empty()) ++frames;
    }
    require(frames >= 28 && frames <= 35, "30 FPS output maintains cadence on a 60 FPS OBS canvas");
    auto *props = obs_source_properties(filter);
    require(obs_property_visible(obs_properties_get(props, "address")), "UDP controls visible");
    require(!obs_property_visible(obs_properties_get(props, "device")), "USB controls hidden for UDP");
    obs_properties_destroy(props);

    for (const char *algorithm : {"bilinear", "bicubic", "lanczos", "area"}) {
        obs_data_set_string(settings, "scaling", algorithm);
        obs_source_update(filter, settings);
        std::this_thread::sleep_for(200ms);
        while (!receive(socket, 0).empty()) {}
        const auto frame = receive(socket);
        require(frame.size() == nearest.size() && frame != nearest, "resampling algorithm changes output pixels");
    }
    obs_data_set_int(settings, "width", 32); obs_data_set_int(settings, "height", 16);
    obs_source_update(filter, settings);
    std::this_thread::sleep_for(200ms);
    while (!receive(socket, 0).empty()) {}
    require(receive(socket).size() == 32 * 16 * 3, "live output resolution change");
    obs_source_set_enabled(filter, false);
    std::this_thread::sleep_for(150ms);
    while (!receive(socket, 0).empty()) {}
    require(receive(socket, 150).empty(), "disabled filter stops network output");
    obs_source_set_enabled(filter, true);
    require(receive(socket).size() == 32 * 16 * 3, "re-enabled filter resumes");

    obs_data_set_string(settings, "transport", "usb");
    obs_source_update(filter, settings);
    std::this_thread::sleep_for(100ms);
    props = obs_source_properties(filter);
    require(obs_property_visible(obs_properties_get(props, "device")), "USB device dropdown visible");
    require(!obs_property_visible(obs_properties_get(props, "address")), "IP controls hidden for USB");
    obs_properties_destroy(props);
    while (!receive(socket, 0).empty()) {}
    require(receive(socket, 100).empty(), "transport switch stops UDP");
    obs_data_set_string(settings, "transport", "udp");
    obs_data_set_string(settings, "address", "invalid");
    obs_source_update(filter, settings);
    require(receive(socket, 100).empty(), "invalid destination remains stopped");

    obs_set_output_source(0, nullptr);
    obs_source_filter_remove(source, filter);
    obs_source_release(filter);

    // Exercise the production frontend controller with simulated Tools/menu
    // events, but real offscreen GPU capture and network delivery.
    obs2led::initialize_global_output();
    obs2led::initialize_global_output();
    require(frontend_test::menu_count == 1 && frontend_test::menu && frontend_test::save && frontend_test::event,
            "one Tools menu action and save/event callbacks installed");
    frontend_test::menu(nullptr);
    auto *global = frontend_test::properties_source;
    require(global && (obs_source_get_output_flags(global) & OBS_SOURCE_CAP_DISABLED),
            "Tools opens private global settings, hidden from Add Source");
    auto *global_settings = obs_source_get_settings(global);
    require(!obs_data_get_bool(global_settings, "enabled"), "global output starts disabled");
    obs_data_set_string(global_settings, "address", "127.0.0.1");
    obs_data_set_int(global_settings, "port", ntohs(address.sin_port));
    obs_source_update(global, global_settings);
    obs_set_output_source(0, source);
    frontend_test::event(OBS_FRONTEND_EVENT_FINISHED_LOADING, nullptr);
    std::this_thread::sleep_for(100ms);
    while (!receive(socket, 0).empty()) {}
    require(receive(socket, 100).empty(), "configured but disabled global output sends nothing");
    obs_data_set_bool(global_settings, "enabled", true);
    obs_source_update(global, global_settings);
    require(receive(socket) == nearest, "global capture matches the filter's nearest RGB pixels");
    props = obs_source_properties(global);
    for (const auto *key : {"enabled", "transport", "address", "port", "device", "baud", "width", "height", "fps", "scaling", "refresh"})
        require(obs_properties_get(props, key) != nullptr, "global menu exposes all output controls");
    obs_properties_destroy(props);
    for (const char *algorithm : {"bilinear", "bicubic", "lanczos", "area"}) {
        obs_data_set_string(global_settings, "scaling", algorithm);
        obs_source_update(global, global_settings);
        std::this_thread::sleep_for(150ms);
        while (!receive(socket, 0).empty()) {}
        const auto frame = receive(socket);
        require(frame.size() == nearest.size() && frame != nearest, "global resampling changes pixels");
    }
    obs_data_set_string(global_settings, "scaling", "nearest");
    obs_source_update(global, global_settings);

    auto *alternate_settings = obs_data_create();
    obs_data_set_bool(alternate_settings, "alternate", true);
    auto *alternate = obs_source_create_private(input.id, "Blue scene", alternate_settings);
    obs_data_release(alternate_settings);
    require(alternate != nullptr, "second scene created");
    std::vector<uint8_t> blue(nearest.size(), 0);
    for (size_t i = 2; i < blue.size(); i += 3) blue[i] = 255;
    obs_set_output_source(0, alternate);
    frontend_test::event(OBS_FRONTEND_EVENT_SCENE_CHANGED, nullptr);
    std::this_thread::sleep_for(150ms);
    while (!receive(socket, 0).empty()) {}
    require(receive(socket) == blue, "global output follows scene changes");

    // The preview source is not on the main output, and there is no display.
    // Capturing it proves that Studio Preview works even with its pane disabled.
    auto *preview_scene = obs_scene_create_private("Preview composite");
    require(preview_scene && obs_scene_add(preview_scene, source), "create scene with nested video source");
    frontend_test::studio_mode = true;
    frontend_test::preview = obs_scene_get_source(preview_scene);
    frontend_test::event(OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED, nullptr);
    std::this_thread::sleep_for(150ms);
    while (!receive(socket, 0).empty()) {}
    require(receive(socket) == nearest, "disabled Studio Preview is captured instead of Program");
    require(obs_source_showing(source), "global preview keeps nested sources showing without a display");
    frontend_test::preview = alternate;
    frontend_test::event(OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED, nullptr);
    std::this_thread::sleep_for(150ms);
    while (!receive(socket, 0).empty()) {}
    require(receive(socket) == blue, "global output follows Studio Preview selection");
    frontend_test::preview = source;
    frontend_test::event(OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED, nullptr);
    frontend_test::studio_mode = false;
    frontend_test::event(OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED, nullptr);
    std::this_thread::sleep_for(150ms);
    while (!receive(socket, 0).empty()) {}
    require(receive(socket) == blue, "leaving Studio Mode returns to normal preview");

    auto *small_settings = obs_data_create();
    obs_data_set_bool(small_settings, "alternate", true);
    obs_data_set_bool(small_settings, "small", true);
    auto *small = obs_source_create_private(input.id, "Small blue content", small_settings);
    obs_data_release(small_settings);
    obs_set_output_source(0, small);
    frontend_test::event(OBS_FRONTEND_EVENT_SCENE_CHANGED, nullptr);
    std::this_thread::sleep_for(150ms);
    while (!receive(socket, 0).empty()) {}
    auto canvas_frame = receive(socket);
    require(canvas_frame.size() == nearest.size(), "global output still captures full canvas");
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 64; ++x) {
        const auto at = (y * 64 + x) * 3;
        require(canvas_frame[at] == 0 && canvas_frame[at + 1] == 0 &&
                canvas_frame[at + 2] == (x < 32 && y < 16 ? 255 : 0),
                "global capture preserves canvas margins around smaller content");
    }
    obs_set_output_source(0, alternate);
    frontend_test::event(OBS_FRONTEND_EVENT_SCENE_CHANGED, nullptr);
    obs_source_release(small);

    auto *saved = obs_data_create();
    frontend_test::save(saved, true, nullptr);
    frontend_test::event(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING, nullptr);
    std::this_thread::sleep_for(100ms);
    while (!receive(socket, 0).empty()) {}
    require(receive(socket, 100).empty(), "collection change suspends output");
    auto *empty_collection = obs_data_create();
    frontend_test::save(empty_collection, false, nullptr);
    frontend_test::event(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED, nullptr);
    require(!obs_data_get_bool(global_settings, "enabled") && receive(socket, 100).empty(),
            "new collection starts with global output disabled");
    frontend_test::event(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING, nullptr);
    frontend_test::save(saved, false, nullptr);
    frontend_test::event(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED, nullptr);
    require(receive(socket) == blue, "saved collection restores settings and enabled output");
    obs_data_set_int(global_settings, "width", 32);
    obs_data_set_int(global_settings, "height", 16);
    obs_source_update(global, global_settings);
    std::this_thread::sleep_for(150ms);
    while (!receive(socket, 0).empty()) {}
    require(receive(socket).size() == 32 * 16 * 3, "global resolution changes live");
    obs_data_set_bool(global_settings, "enabled", false);
    obs_source_update(global, global_settings);
    std::this_thread::sleep_for(100ms);
    while (!receive(socket, 0).empty()) {}
    require(receive(socket, 100).empty(), "global checkbox stops sending");
    obs_data_set_bool(global_settings, "enabled", true);
    obs_source_update(global, global_settings);
    require(receive(socket).size() == 32 * 16 * 3, "global checkbox resumes sending");
    obs_data_set_string(global_settings, "transport", "usb");
    obs_source_update(global, global_settings);
    props = obs_source_properties(global);
    require(obs_property_visible(obs_properties_get(props, "device")) &&
            !obs_property_visible(obs_properties_get(props, "address")), "global USB settings switch controls");
    obs_properties_destroy(props);
    std::this_thread::sleep_for(100ms);
    while (!receive(socket, 0).empty()) {}
    require(receive(socket, 100).empty(), "invalid global USB configuration stops UDP");
    frontend_test::event(OBS_FRONTEND_EVENT_EXIT, nullptr);
    require(!frontend_test::event && !frontend_test::save, "frontend callbacks removed on exit");
    obs2led::shutdown_global_output();
    obs_scene_release(preview_scene);
    obs_data_release(empty_collection); obs_data_release(saved); obs_data_release(global_settings);
    obs_set_output_source(0, nullptr);
    obs_source_release(alternate); obs_source_release(source); obs_data_release(settings);
    obs_shutdown(); closesocket(socket); WSACleanup();
    std::puts("PASS: filter and global GPU pixels, 5 scalers, scene/Studio Preview switching without a display, settings persistence, output lifecycle");
}
