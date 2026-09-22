#include "video-filter.hpp"
#include "output.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#include <array>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace obs2led {
namespace {

void global_render(void *data, uint32_t, uint32_t) noexcept;

struct Snapshot {
    OutputConfig config;
    uint64_t generation;
    bool valid;
    bool enabled;
};

struct VideoOutput {
    explicit VideoOutput(obs_source_t *context, bool global = false) : source(context), global(global)
    {
        if (global) output.set_active(false);
    }
    ~VideoOutput()
    {
        if (global) obs_remove_main_render_callback(global_render, this);
        obs_enter_graphics();
        if (target_showing) obs_source_dec_showing(global_target);
        obs_source_release(global_target);
        reset_surfaces();
        gs_texrender_destroy(original);
        gs_texrender_destroy(scaled);
        gs_samplerstate_destroy(point_sampler);
        obs_leave_graphics();
    }
    void reset_surfaces()
    {
        for (auto *surface : stages) gs_stagesurface_destroy(surface);
        stages = {};
        staged = {};
        stage_index = 0;
    }
    obs_source_t *source; // Borrowed; OBS owns the source.
    const bool global;
    Output output;
    std::mutex settings_mutex;
    std::shared_ptr<const Snapshot> settings;
    std::shared_ptr<const Snapshot> current_settings()
    {
        std::lock_guard lock(settings_mutex);
        return settings;
    }
    // Everything below belongs to OBS's video/graphics thread.
    // UI-side changes to the global target/activity hold OBS's graphics lock.
    obs_source_t *global_target = nullptr; // Owned reference, never a scene filter.
    bool target_showing = false;
    gs_texrender_t *original = nullptr;
    uint32_t canvas_width = 0;
    uint32_t canvas_height = 0;
    gs_texrender_t *scaled = nullptr;
    gs_samplerstate_t *point_sampler = nullptr;
    std::array<gs_stagesurf_t *, 2> stages{};
    std::array<bool, 2> staged{};
    size_t stage_index = 0;
    uint64_t graphics_generation = 0;
    uint64_t last_capture = 0;
    uint64_t next_capture = 0;
    bool active = true;
    bool capture_error_logged = false;
};

const char *text(const char *key) { return obs_module_text(key); }

void sync_global_activity(VideoOutput &filter)
{
    const auto snapshot = filter.current_settings();
    const bool enabled = snapshot && snapshot->enabled && snapshot->valid && filter.global_target;
    if (enabled != filter.target_showing) {
        if (enabled) obs_source_inc_showing(filter.global_target);
        else obs_source_dec_showing(filter.global_target);
        filter.target_showing = enabled;
    }
    if (filter.active != enabled) {
        filter.staged = {};
        filter.last_capture = filter.next_capture = 0;
    }
    filter.active = enabled;
    filter.output.set_active(enabled);
}

void set_global_target(void *data, calldata_t *params) noexcept
{
    auto &filter = *static_cast<VideoOutput *>(data);
    auto *target = static_cast<obs_source_t *>(calldata_ptr(params, "target"));
    if (target == filter.source) return;
    obs_enter_graphics();
    if (target != filter.global_target) {
        auto *previous = filter.global_target;
        if (filter.target_showing) obs_source_dec_showing(previous);
        filter.target_showing = false;
        filter.global_target = obs_source_get_ref(target);
        filter.staged = {};
        filter.last_capture = filter.next_capture = 0;
        sync_global_activity(filter);
        obs_source_release(previous);
    }
    obs_leave_graphics();
}

OutputConfig read_settings(obs_data_t *settings)
{
    OutputConfig c;
    c.transport = std::string_view(obs_data_get_string(settings, "transport")) == "usb" ? Transport::usb : Transport::udp;
    c.address = obs_data_get_string(settings, "address");
    c.device = obs_data_get_string(settings, "device");
    auto integer = [settings](const char *name) {
        const auto value = obs_data_get_int(settings, name);
        return value < 0 || value > 10000000 ? -1 : static_cast<int>(value);
    };
    c.port = integer("port"); c.width = integer("width"); c.height = integer("height");
    c.fps = integer("fps"); c.baud = integer("baud");
    const std::string_view scaling = obs_data_get_string(settings, "scaling");
    if (scaling == "bilinear") c.scaling = Scaling::bilinear;
    else if (scaling == "bicubic") c.scaling = Scaling::bicubic;
    else if (scaling == "lanczos") c.scaling = Scaling::lanczos;
    else if (scaling == "area") c.scaling = Scaling::area;
    return c;
}

void defaults(obs_data_t *settings) noexcept
{
    obs_data_set_default_string(settings, "transport", "udp");
    obs_data_set_default_string(settings, "address", "");
    obs_data_set_default_int(settings, "port", 9090);
    obs_data_set_default_string(settings, "device", "");
    obs_data_set_default_int(settings, "baud", 2000000);
    obs_data_set_default_int(settings, "width", 64);
    obs_data_set_default_int(settings, "height", 32);
    obs_data_set_default_int(settings, "fps", 30);
    obs_data_set_default_string(settings, "scaling", "nearest");
}

void global_defaults(obs_data_t *settings) noexcept
{
    defaults(settings);
    obs_data_set_default_bool(settings, "enabled", false);
}

void update(void *data, obs_data_t *settings) noexcept
{
    auto &filter = *static_cast<VideoOutput *>(data);
    try {
        const auto config = read_settings(settings);
        const auto generation = filter.output.configure(config);
        auto snapshot = std::make_shared<Snapshot>(Snapshot{config, generation, validate(config).empty(),
            !filter.global || obs_data_get_bool(settings, "enabled")});
        {
            std::lock_guard lock(filter.settings_mutex);
            filter.settings = std::move(snapshot);
        }
        if (filter.global) {
            obs_enter_graphics();
            sync_global_activity(filter);
            obs_leave_graphics();
        }
    } catch (const std::exception &error) {
        filter.output.set_active(false);
        { std::lock_guard lock(filter.settings_mutex); filter.settings.reset(); }
        blog(LOG_ERROR, "[obs2led] Cannot apply settings: %s", error.what());
    }
}

void *create_global(obs_data_t *settings, obs_source_t *source) noexcept
{
    try {
        auto output = std::make_unique<VideoOutput>(source, true);
        update(output.get(), settings);
        proc_handler_add(obs_source_get_proc_handler(source), "void set_target(ptr target)",
                         set_global_target, output.get());
        obs_add_main_render_callback(global_render, output.get());
        return output.release();
    } catch (const std::exception &error) {
        blog(LOG_ERROR, "[obs2led] Cannot create global output: %s", error.what());
        return nullptr;
    }
}

void *create(obs_data_t *settings, obs_source_t *source) noexcept
{
    try {
        auto filter = std::make_unique<VideoOutput>(source);
        update(filter.get(), settings);
        return filter.release();
    } catch (const std::exception &error) {
        blog(LOG_ERROR, "[obs2led] Cannot create filter: %s", error.what());
        return nullptr;
    }
}

void destroy(void *data) noexcept { delete static_cast<VideoOutput *>(data); }
const char *get_name(void *) noexcept
{
    const char *name = nullptr;
    return obs_module_get_string("Filter.Name", &name) ? name : "OBS2LED";
}

void fill_devices(obs_properties_t *props, const std::string &selected)
{
    auto *list = obs_properties_get(props, "device");
    obs_property_list_clear(list);
    obs_property_list_add_string(list, text("Device.Select"), "");
    bool found = selected.empty();
    try {
        for (const auto &device : serial_devices()) {
            obs_property_list_add_string(list, device.label.c_str(), device.path.c_str());
            found |= device.path == selected;
        }
    } catch (const std::exception &error) {
        blog(LOG_WARNING, "[obs2led] USB device discovery: %s", error.what());
    }
    if (!found) {
        const auto label = std::string(text("Device.Disconnected")) + " " + selected;
        obs_property_list_add_string(list, label.c_str(), selected.c_str());
    }
}

bool changed(void *data, obs_properties_t *props, obs_property_t *, obs_data_t *settings) noexcept
{
    try {
        const auto config = read_settings(settings);
        const bool usb = config.transport == Transport::usb;
        for (const auto *name : {"address", "port"}) obs_property_set_visible(obs_properties_get(props, name), !usb);
        for (const auto *name : {"device", "baud", "usb_help"}) obs_property_set_visible(obs_properties_get(props, name), usb);
        auto status = validate(config);
        const bool invalid = !status.empty();
        if (!invalid) {
            auto *filter = static_cast<VideoOutput *>(data);
            const auto current = filter ? filter->current_settings() : nullptr;
            status = current && current->config == config ? filter->output.status() : text("Status.Applying");
        }
        if (obs_properties_get(props, "enabled") && !obs_data_get_bool(settings, "enabled"))
            status = text("Global.Stopped");
        auto *property = obs_properties_get(props, "status");
        obs_property_set_description(property, status.c_str());
        obs_property_text_set_info_type(property, invalid ? OBS_TEXT_INFO_WARNING : OBS_TEXT_INFO_NORMAL);
    } catch (const std::exception &error) {
        blog(LOG_WARNING, "[obs2led] Properties: %s", error.what());
    }
    return true;
}

bool refresh(obs_properties_t *props, obs_property_t *, void *data) noexcept
{
    auto *filter = static_cast<VideoOutput *>(data);
    if (!filter) return false;
    obs_data_t *settings = obs_source_get_settings(filter->source);
    try { fill_devices(props, obs_data_get_string(settings, "device")); }
    catch (const std::exception &error) { blog(LOG_WARNING, "[obs2led] Refresh devices: %s", error.what()); }
    changed(filter, props, nullptr, settings);
    obs_data_release(settings);
    return true;
}

obs_properties_t *make_properties(void *data, bool global) noexcept
{
    auto *props = obs_properties_create();
    if (global) {
        obs_properties_add_bool(props, "enabled", text("Global.Enabled"));
        obs_properties_add_text(props, "global_help", text("Global.Help"), OBS_TEXT_INFO);
        obs_property_set_modified_callback2(obs_properties_get(props, "enabled"), changed, data);
    }
    auto *transport = obs_properties_add_list(props, "transport", text("Transport"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(transport, text("Transport.Socket"), "udp");
    obs_property_list_add_string(transport, text("Transport.USB"), "usb");
    obs_properties_add_text(props, "address", text("Address"), OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, "port", text("Port"), 1, 65535, 1);
    obs_properties_add_list(props, "device", text("Device"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    auto *baud = obs_properties_add_list(props, "baud", text("Baud"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    for (const int rate : {115200, 230400, 460800, 921600, 1000000, 2000000, 3000000}) {
        const auto label = std::to_string(rate);
        obs_property_list_add_int(baud, label.c_str(), rate);
    }
    obs_properties_add_text(props, "usb_help", text("USB.Help"), OBS_TEXT_INFO);
    obs_properties_add_int(props, "width", text("Width"), 1, max_dimension, 1);
    obs_properties_add_int(props, "height", text("Height"), 1, max_dimension, 1);
    obs_properties_add_int(props, "fps", text("FPS"), 1, 60, 1);
    auto *scaling = obs_properties_add_list(props, "scaling", text("Scaling"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(scaling, text("Scaling.Nearest"), "nearest");
    obs_property_list_add_string(scaling, text("Scaling.Bilinear"), "bilinear");
    obs_property_list_add_string(scaling, text("Scaling.Bicubic"), "bicubic");
    obs_property_list_add_string(scaling, text("Scaling.Lanczos"), "lanczos");
    obs_property_list_add_string(scaling, text("Scaling.Area"), "area");
    obs_properties_add_text(props, "status", text("Status.Applying"), OBS_TEXT_INFO);
    obs_properties_add_button2(props, "refresh", text("Refresh"), refresh, data);
    for (const auto *name : {"transport", "address", "port", "device", "baud", "width", "height", "fps", "scaling"})
        obs_property_set_modified_callback2(obs_properties_get(props, name), changed, data);
    if (data) refresh(props, nullptr, data);
    else {
        auto *settings = obs_data_create();
        defaults(settings);
        changed(nullptr, props, nullptr, settings);
        obs_data_release(settings);
    }
    return props;
}

obs_properties_t *properties(void *data) noexcept { return make_properties(data, false); }
obs_properties_t *global_properties(void *data) noexcept { return make_properties(data, true); }

void tick(void *data, float) noexcept
{
    auto &filter = *static_cast<VideoOutput *>(data);
    const bool active = obs_source_enabled(filter.source);
    filter.output.set_active(active);
    if (active != filter.active) {
        filter.active = active;
        filter.staged = {};
        filter.last_capture = 0;
        filter.next_capture = 0;
    }
}

void capture(VideoOutput &filter, const Snapshot &snapshot)
{
    const auto &c = snapshot.config;
    auto *target = filter.global ? filter.global_target : obs_filter_get_target(filter.source);
    if (!target || obs_source_removed(target) || (!filter.global && !obs_filter_get_parent(filter.source))) return;
    const auto width = filter.global ? filter.canvas_width : obs_source_get_base_width(target);
    const auto height = filter.global ? filter.canvas_height : obs_source_get_base_height(target);
    if (!width || !height) return;
    if (filter.graphics_generation != snapshot.generation) {
        filter.reset_surfaces();
        filter.graphics_generation = snapshot.generation;
        filter.last_capture = 0;
        filter.next_capture = 0;
    }
    const uint64_t now = obs_get_video_frame_time();
    const uint64_t interval = 1000000000ull / static_cast<uint64_t>(c.fps);
    if (now == filter.last_capture || (filter.next_capture && now + 1000000 < filter.next_capture)) return;
    if (filter.last_capture && now > filter.last_capture + interval * 3) filter.staged = {};
    filter.last_capture = now;
    // Preserve cadence across fractional OBS frame periods; don't emit catch-up bursts.
    if (!filter.next_capture || now > filter.next_capture + interval) filter.next_capture = now + interval;
    else filter.next_capture += interval;
    if (!filter.scaled) filter.scaled = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    if (!filter.point_sampler) {
        gs_sampler_info sampler{};
        sampler.filter = GS_FILTER_POINT;
        sampler.address_u = sampler.address_v = sampler.address_w = GS_ADDRESS_CLAMP;
        filter.point_sampler = gs_samplerstate_create(&sampler);
    }
    for (auto *&stage : filter.stages)
        if (!stage) stage = gs_stagesurface_create(c.width, c.height, GS_RGBA);
    if (!filter.scaled || !filter.point_sampler || !filter.stages[0] || !filter.stages[1])
        throw std::runtime_error("Cannot allocate GPU readback resources");

    const gs_color_space spaces[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED, GS_CS_709_SCRGB};
    const auto space = obs_source_get_color_space(target, std::size(spaces), spaces);
    if (filter.global) {
        const auto format = gs_get_format_from_space(space);
        if (filter.original && gs_texrender_get_format(filter.original) != format) {
            gs_texrender_destroy(filter.original);
            filter.original = nullptr;
        }
        if (!filter.original) filter.original = gs_texrender_create(format, GS_ZS_NONE);
        if (!filter.original) throw std::runtime_error("Cannot allocate global capture texture");
        gs_texrender_reset(filter.original);
        if (!gs_texrender_begin_with_color_space(filter.original, width, height, space)) return;
        gs_blend_state_push();
        gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
        vec4 clear{};
        gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
        gs_ortho(0, static_cast<float>(width), 0, static_cast<float>(height), -100, 100);
        obs_source_video_render(target);
        gs_blend_state_pop();
        gs_texrender_end(filter.original);
    } else if (!obs_source_process_filter_begin_with_color_space(filter.source, gs_get_format_from_space(space),
                                                                 space, OBS_NO_DIRECT_RENDERING)) return;
    gs_texrender_reset(filter.scaled);
    if (!gs_texrender_begin_with_color_space(filter.scaled, c.width, c.height, GS_CS_SRGB)) return;
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    vec4 black{};
    gs_clear(GS_CLEAR_COLOR, &black, 0, 0);
    gs_ortho(0, static_cast<float>(c.width), 0, static_cast<float>(c.height), -100, 100);

    obs_base_effect type = OBS_EFFECT_DEFAULT;
    if (c.scaling == Scaling::bicubic) type = OBS_EFFECT_BICUBIC;
    else if (c.scaling == Scaling::lanczos) type = OBS_EFFECT_LANCZOS;
    else if (c.scaling == Scaling::area) type = OBS_EFFECT_AREA;
    auto *effect = obs_get_base_effect(type);
    vec2 dimensions{}, reciprocal{};
    vec2_set(&dimensions, static_cast<float>(width), static_cast<float>(height));
    vec2_set(&reciprocal, 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
    if (auto *p = gs_effect_get_param_by_name(effect, "base_dimension")) gs_effect_set_vec2(p, &dimensions);
    if (auto *p = gs_effect_get_param_by_name(effect, "base_dimension_i")) gs_effect_set_vec2(p, &reciprocal);
    if (auto *p = gs_effect_get_param_by_name(effect, "undistort_factor")) gs_effect_set_float(p, 1.0f);
    const float multiplier = space == GS_CS_709_SCRGB ? 80.0f / obs_get_video_sdr_white_level() : 1.0f;
    if (auto *p = gs_effect_get_param_by_name(effect, "multiplier")) gs_effect_set_float(p, multiplier);
    if (c.scaling == Scaling::nearest)
        gs_effect_set_next_sampler(gs_effect_get_param_by_name(effect, "image"), filter.point_sampler);
    const bool upscale = c.scaling == Scaling::area && static_cast<uint32_t>(c.width) >= width && static_cast<uint32_t>(c.height) >= height;
    const char *technique = upscale ? "DrawUpscale" : "Draw";
    if (space == GS_CS_709_EXTENDED) technique = upscale ? "DrawUpscaleTonemap" : "DrawTonemap";
    else if (space == GS_CS_709_SCRGB) technique = upscale ? "DrawUpscaleMultiplyTonemap" : "DrawMultiplyTonemap";
    if (filter.global) {
        auto *texture = gs_texrender_get_texture(filter.original);
        const bool previous = gs_framebuffer_srgb_enabled();
        gs_enable_framebuffer_srgb(true);
        gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), texture);
        while (gs_effect_loop(effect, technique)) gs_draw_sprite(texture, 0, c.width, c.height);
        gs_enable_framebuffer_srgb(previous);
    } else {
        obs_source_process_filter_tech_end(filter.source, effect, c.width, c.height, technique);
    }
    gs_blend_state_pop();
    gs_texrender_end(filter.scaled);

    const auto write_index = filter.stage_index;
    const auto read_index = (write_index + 1) % filter.stages.size();
    gs_stage_texture(filter.stages[write_index], gs_texrender_get_texture(filter.scaled));
    filter.staged[write_index] = true;
    filter.stage_index = read_index;
    if (filter.staged[read_index]) {
        uint8_t *pixels = nullptr;
        uint32_t stride = 0;
        if (gs_stagesurface_map(filter.stages[read_index], &pixels, &stride)) {
            std::vector<uint8_t> rgb;
            try { rgb = rgb_from_rgba(pixels, stride, c.width, c.height); }
            catch (...) { gs_stagesurface_unmap(filter.stages[read_index]); throw; }
            gs_stagesurface_unmap(filter.stages[read_index]);
            filter.output.submit(std::move(rgb), snapshot.generation);
        }
    }
}

void render(void *data, gs_effect_t *) noexcept
{
    auto &filter = *static_cast<VideoOutput *>(data);
    std::shared_ptr<const Snapshot> snapshot;
    {
        std::unique_lock lock(filter.settings_mutex, std::try_to_lock);
        if (lock) snapshot = filter.settings;
    }
    if (snapshot && snapshot->enabled && snapshot->valid && filter.output.ready() && filter.active) {
        try {
            capture(filter, *snapshot);
            filter.capture_error_logged = false;
        } catch (const std::exception &error) {
            if (!filter.capture_error_logged) blog(LOG_ERROR, "[obs2led] Frame capture: %s", error.what());
            filter.capture_error_logged = true;
        }
    }
    // The LED output is a side channel; OBS keeps the original size and color space.
    if (!filter.global) obs_source_skip_video_filter(filter.source);
}

void global_render(void *data, uint32_t, uint32_t) noexcept
{
    auto &output = *static_cast<VideoOutput *>(data);
    obs_video_info video{};
    if (!obs_get_video_info(&video)) return;
    const auto width = video.base_width;
    const auto height = video.base_height;
    // Capture the whole canvas, including empty space around resized scene
    // content, just as OBS draws Preview. Do not crop to the target's bounds.
    // Other OBS video mixes may invoke this callback too; use the main canvas
    // dimensions and the common frame-time limiter for all invocations.
    if (output.canvas_width != width || output.canvas_height != height) {
        output.canvas_width = width;
        output.canvas_height = height;
        output.staged = {};
        output.last_capture = output.next_capture = 0;
    }
    render(data, nullptr);
}

gs_color_space get_color_space(void *data, size_t count, const gs_color_space *preferred_spaces) noexcept
{
    const auto &filter = *static_cast<VideoOutput *>(data);
    auto *target = obs_filter_get_target(filter.source);
    return target ? obs_source_get_color_space(target, count, preferred_spaces) : GS_CS_SRGB;
}

} // namespace

void register_video_filter()
{
    obs_source_info info{};
    info.id = "obs2led_video_filter";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
    info.get_name = get_name;
    info.create = create;
    info.destroy = destroy;
    info.get_defaults = defaults;
    info.get_properties = properties;
    info.update = update;
    info.video_tick = tick;
    info.video_render = render;
    info.video_get_color_space = get_color_space;
    obs_register_source(&info);

    // A private settings host gives Tools the same native OBS properties UI.
    // It has no video/audio output and is hidden from the Add Source menu.
    obs_source_info global{};
    global.id = "obs2led_global_output";
    global.type = OBS_SOURCE_TYPE_INPUT;
    global.output_flags = OBS_SOURCE_CAP_DISABLED;
    global.get_name = get_name;
    global.create = create_global;
    global.destroy = destroy;
    global.get_defaults = global_defaults;
    global.get_properties = global_properties;
    global.update = update;
    obs_register_source(&global);
}

} // namespace obs2led
