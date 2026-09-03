#pragma once
#include <string>
#include "Engine/Scene/SceneDescriptor.h"

namespace SE {

// Serializes a SceneDescriptor as canonical FoxLevel version 2 JSON — the
// inverse of SceneLoader/LevelAssetLoader. Legacy scenes are rewritten in the
// versioned entity format on their first save.
class SceneSaver
{
public:
    // Pretty-printed level JSON text for the descriptor. Deterministic, so it
    // doubles as a change-detection snapshot for editor dirty tracking.
    static std::string ToJson(const SceneDescriptor& desc);

    // Validates the generated level through LevelAssetLoader before writing.
    // Returns false (and logs diagnostics) without touching the file when the
    // descriptor does not round-trip.
    static bool SaveToFile(const std::string& path, const SceneDescriptor& desc);
};

} // namespace SE
