#pragma once

#include <cstdint>
#include <filesystem>

namespace Libs::Graphics::DirectStorageIntegration {
// Optional positional read. On false the caller keeps its existing file/error path.
// Copies on the calling thread so guest page tracking observes the writes.
bool TryReadFile(const std::filesystem::path& path, uint64_t offset, void* destination,
                 uint64_t size, uint64_t* bytes_read);
}
