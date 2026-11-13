#include "Scripting/ScriptContext.h"

namespace SE::Scripting {

// Every fox.* module, in reference order. Adding an API: one call in the
// matching Bindings/Bind*.cpp, then `FoxProject api` to refresh the stubs.
void RegisterAllBindings(ScriptApi& api, ScriptContext& ctx)
{
    BindCore(api, ctx);
    BindMath(api, ctx);
    BindInput(api, ctx);
    BindScene(api, ctx);
    BindUi(api, ctx);
}

} // namespace SE::Scripting
