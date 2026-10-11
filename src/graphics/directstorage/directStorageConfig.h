#pragma once

#include "common/common.h"

#include <cstdint>
#include <string>

namespace Config {

enum class DirectStorageMode : uint32_t {
	Disabled = 0,
	Enabled = 1,
};

bool DirectStorageEnabled();
void SetDirectStorageEnabled(bool enabled);
DirectStorageMode GetDirectStorageMode();

} // namespace Config
