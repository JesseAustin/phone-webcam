#include <obs-module.h>
#include <obs-source.h>
#include "phone-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("phone-webcam", "en-US")

bool obs_module_load(void)
{
	
	obs_register_source(&phone_source_info);
	
	blog(LOG_INFO, "Phone Webcam plugin loaded successfully (version %s)",
	     PLUGIN_VERSION);
	
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "Phone Webcam plugin unloaded");
}

const char *obs_module_name(void)
{
	return "Phone Webcam";
}

const char *obs_module_description(void)
{
	return "Use your phone as a webcam with ultra-low latency";
}