#include "graphics/host_gpu/vramBudget.h"

#include "common/liveSwitch.h"
#include "common/emulatorConfig.h"

namespace Libs::Graphics::VramBudget {

namespace {

int64_t ParseBudgetMode(const char* value) {
	return value == nullptr || value[0] == '\0' ? -1 : Live::ParseDefaultOff(value);
}

Live::Switch g_vram_gc_budget("KYTY_VRAM_GC_BUDGET", ParseBudgetMode);

} // namespace

bool GcEnabled() noexcept {
	return UseBudgetCollector(g_vram_gc_budget.Get(), Config::DirectStorageEnabled());
}

} // namespace Libs::Graphics::VramBudget
