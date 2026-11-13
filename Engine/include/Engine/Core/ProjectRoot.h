#pragma once
#include <filesystem>

namespace SE {

// Selects --project (directory or project.json), otherwise the executable's
// project, the nearest project above cwd, or the configured sample project.
// Changes cwd only on success. Call before initializing engine subsystems.
bool SetProjectRootCwd(const std::filesystem::path& project = {});

// Engine resources remain independent of the selected project's cwd.
const std::filesystem::path& GetEngineRoot();
std::filesystem::path EnginePath(const std::filesystem::path& relativePath);

} // namespace SE
