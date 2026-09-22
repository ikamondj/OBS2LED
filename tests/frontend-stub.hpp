#pragma once

#include <obs-frontend-api.h>

// Minimal frontend used by the GPU test. Rendering and UDP use real libobs;
// only UI actions/events are simulated so tests do not alter the user's OBS.
namespace frontend_test {
extern bool studio_mode;
extern obs_source_t *preview;
extern obs_source_t *properties_source;
extern obs_frontend_cb menu;
extern obs_frontend_event_cb event;
extern obs_frontend_save_cb save;
extern int menu_count;
}
