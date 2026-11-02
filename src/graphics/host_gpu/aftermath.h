#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_AFTERMATH_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_AFTERMATH_H_

// KYTY_AFTERMATH=1 (default off, NVIDIA on Windows): NVIDIA Nsight Aftermath GPU crash dumps.
// The SDK is not part of this repository: CMake's KYTY_AFTERMATH_SDK names an extracted Nsight
// Aftermath SDK, whose headers this file's implementation is built against; without it the switch
// only logs that the build has no Aftermath support. At run time GFSDK_Aftermath_Lib.x64.dll is
// loaded from the emulator's folder (it is not shipped with Kyty; it comes with the SDK). On a
// device loss the dump (and any shader debug information) is written to _AftermathDumps next to
// the executable, open it with Nsight Graphics.
namespace Libs::Graphics::Aftermath {

[[nodiscard]] bool Requested();
// Before vkCreateDevice: loads the library and enables crash dump collection for Vulkan. True when
// dumps are being collected (the device then needs VK_NV_device_diagnostics_config).
bool Initialize();
[[nodiscard]] bool Active();
// After a device loss was seen: waits (bounded) until the driver has handed over the dump.
void WaitForDump();

} // namespace Libs::Graphics::Aftermath

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_AFTERMATH_H_ */
