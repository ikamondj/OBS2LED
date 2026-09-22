#include "frontend-stub.hpp"

namespace frontend_test {
bool studio_mode = false;
obs_source_t *preview = nullptr;
obs_source_t *properties_source = nullptr;
obs_frontend_cb menu = nullptr;
obs_frontend_event_cb event = nullptr;
obs_frontend_save_cb save = nullptr;
int menu_count = 0;
}

using namespace frontend_test;

void *obs_frontend_get_main_window() { return &menu_count; }
bool obs_frontend_preview_program_mode_active() { return studio_mode; }
obs_source_t *obs_frontend_get_current_preview_scene() { return obs_source_get_ref(preview); }
void obs_frontend_open_source_properties(obs_source_t *source) { properties_source = source; }
void obs_frontend_add_tools_menu_item(const char *, obs_frontend_cb callback, void *)
{
    menu = callback;
    ++menu_count;
}
void obs_frontend_add_event_callback(obs_frontend_event_cb callback, void *) { event = callback; }
void obs_frontend_remove_event_callback(obs_frontend_event_cb, void *) { event = nullptr; }
void obs_frontend_add_save_callback(obs_frontend_save_cb callback, void *) { save = callback; }
void obs_frontend_remove_save_callback(obs_frontend_save_cb, void *) { save = nullptr; }

// The frontend controller's fallback label; the loaded plugin has its real locale.
extern "C" bool obs_module_get_string(const char *, const char **) { return false; }
