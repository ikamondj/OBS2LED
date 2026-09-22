#include "global-output.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

namespace obs2led {
namespace {

// Frontend callbacks run on OBS's UI thread. The private settings source owns
// the renderer/transport and synchronizes target changes with the graphics thread.
obs_source_t *output = nullptr;
bool initialized = false;
bool collection_changing = false;

void set_target(obs_source_t *target)
{
    if (!output) return;
    calldata_t args{};
    calldata_set_ptr(&args, "target", target);
    proc_handler_call(obs_source_get_proc_handler(output), "set_target", &args);
    calldata_free(&args);
}

void refresh_target()
{
    if (!output || collection_changing) return;
    // Preview is a separate scene in Studio Mode. Otherwise OBS displays its
    // main output (including transitions). This is independent of preview visibility.
    obs_source_t *target = obs_frontend_preview_program_mode_active()
        ? obs_frontend_get_current_preview_scene() : obs_get_output_source(0);
    set_target(target);
    obs_source_release(target);
}

void show_settings(void *)
{
    if (!output || collection_changing) return;
    refresh_target();
    obs_frontend_open_source_properties(output);
}

void save_settings(obs_data_t *data, bool saving, void *)
{
    if (!output) return;
    if (saving) {
        auto *settings = obs_source_get_settings(output);
        auto *snapshot = obs_data_create();
        obs_data_apply(snapshot, settings);
        obs_data_set_obj(data, "obs2led_global_output", snapshot);
        obs_data_release(snapshot);
        obs_data_release(settings);
    } else {
        collection_changing = true;
        set_target(nullptr);
        auto *settings = obs_data_get_obj(data, "obs2led_global_output");
        if (!settings) settings = obs_data_create();
        obs_source_reset_settings(output, settings);
        obs_data_release(settings);
        obs_source_update_properties(output);
        // The scene collection's sources may not exist yet. Bind only after
        // FINISHED_LOADING / SCENE_COLLECTION_CHANGED to avoid capturing old scenes.
    }
}

void frontend_event(obs_frontend_event event, void *)
{
    switch (event) {
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
        collection_changing = true;
        set_target(nullptr);
        break;
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
        collection_changing = false;
        refresh_target();
        break;
    case OBS_FRONTEND_EVENT_SCENE_CHANGED:
    case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
    case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
    case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
    case OBS_FRONTEND_EVENT_TRANSITION_CHANGED:
    case OBS_FRONTEND_EVENT_TRANSITION_STOPPED:
        refresh_target();
        break;
    case OBS_FRONTEND_EVENT_EXIT:
        shutdown_global_output();
        break;
    default:
        break;
    }
}

} // namespace

void initialize_global_output()
{
    if (initialized || !obs_frontend_get_main_window()) return;
    const char *label = nullptr;
    if (!obs_module_get_string("Global.Name", &label)) label = "OBS2LED - Global Output";
    output = obs_source_create_private("obs2led_global_output", label, nullptr);
    if (!output) {
        blog(LOG_ERROR, "[obs2led] Cannot initialize global output settings");
        return;
    }
    initialized = true;
    obs_frontend_add_save_callback(save_settings, nullptr);
    obs_frontend_add_event_callback(frontend_event, nullptr);
    obs_frontend_add_tools_menu_item(label, show_settings, nullptr);
}

void shutdown_global_output()
{
    if (!initialized) return;
    initialized = false;
    obs_frontend_remove_save_callback(save_settings, nullptr);
    obs_frontend_remove_event_callback(frontend_event, nullptr);
    set_target(nullptr);
    obs_source_release(output);
    output = nullptr;
}

} // namespace obs2led
