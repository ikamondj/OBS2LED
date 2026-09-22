#include <obs-module.h>

#include "video-filter.hpp"
#include "global-output.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs2led", "en-US")

MODULE_EXPORT const char *obs_module_name()
{
    return "OBS2LED";
}

MODULE_EXPORT const char *obs_module_description()
{
    return "Stream source filters or the global OBS preview to RGB LED receivers over UDP or USB serial.";
}

MODULE_EXPORT void obs_module_post_load()
{
    obs2led::initialize_global_output();
}

MODULE_EXPORT void obs_module_unload()
{
    obs2led::shutdown_global_output();
}

MODULE_EXPORT bool obs_module_load()
{
    const char *name = nullptr;
    if (!obs_module_get_string("Filter.Name", &name))
        blog(LOG_WARNING, "[obs2led] Missing locale data. Install the plugin's data/locale directory alongside its binaries; see README.md.");
    obs2led::register_video_filter();
    blog(LOG_INFO, "[obs2led] Loaded version %s", OBS2LED_VERSION);
    return true;
}
