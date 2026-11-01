#pragma once

namespace SE {

// Sets the working directory to the project root: walks up from the exe
// location until a directory containing Engine/ and Assets/ is found.
// Returns false if none is found within five levels.
bool SetProjectRootCwd();

} // namespace SE
