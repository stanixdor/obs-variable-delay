#include "delay-controller.hpp"
#include "delay-dock.hpp"
#include "hold-pipeline.hpp"
#include "plugin-support.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

constexpr const char *DockId = "obs_dynamic_delay_controls";
dynamic_delay::DelayController *controller = nullptr;
QPointer<dynamic_delay::DelayDock> dock;

} // namespace

const char *obs_module_description(void)
{
	return "Adds and removes a compressed-packet delay while OBS outputs remain active.";
}

bool obs_module_load(void)
{
	dynamic_delay::HoldPipeline::register_output_type();

	controller = new dynamic_delay::DelayController;
	dock = new dynamic_delay::DelayDock(*controller);
	if (!obs_frontend_add_dock_by_id(DockId, obs_module_text("Dock.Title"), dock.data())) {
		delete dock.data();
		dock = nullptr;
		delete controller;
		controller = nullptr;
		obs_log(LOG_ERROR, "could not register the Dynamic Delay dock");
		return false;
	}

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	if (dock)
		obs_frontend_remove_dock(DockId);
	dock = nullptr;
	delete controller;
	controller = nullptr;
	obs_log(LOG_INFO, "plugin unloaded");
}
