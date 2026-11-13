#include "Scripting/ScriptApi.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace SE::Scripting {

void Raise(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list sizeArgs;
    va_copy(sizeArgs, args);
    const int needed = vsnprintf(nullptr, 0, fmt, sizeArgs);
    va_end(sizeArgs);
    std::string message(needed > 0 ? static_cast<size_t>(needed) : 0, '\0');
    if (needed > 0)
        vsnprintf(message.data(), message.size() + 1, fmt, args);
    va_end(args);
    throw ScriptError(message);
}

// --- Documentation ----------------------------------------------------------

namespace {

std::string Trim(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// Splits at commas that are not nested inside (), <>, {} or [].
std::vector<std::string> SplitTopLevel(const std::string& text)
{
    std::vector<std::string> parts;
    int depth = 0;
    std::string current;
    for (char c : text)
    {
        if (c == '(' || c == '<' || c == '{' || c == '[') ++depth;
        if (c == ')' || c == '>' || c == '}' || c == ']') --depth;
        if (c == ',' && depth == 0)
        {
            parts.push_back(Trim(current));
            current.clear();
            continue;
        }
        current += c;
    }
    if (!Trim(current).empty())
        parts.push_back(Trim(current));
    return parts;
}

struct Param { std::string name; std::string type; };

struct Signature
{
    std::vector<Param> params;
    std::vector<std::string> returns;
};

Signature ParseSignature(const std::string& text)
{
    Signature sig;
    const size_t open = text.find('(');
    if (open == std::string::npos) return sig;
    int depth = 0;
    size_t close = std::string::npos;
    for (size_t i = open; i < text.size(); ++i)
    {
        if (text[i] == '(') ++depth;
        if (text[i] == ')' && --depth == 0) { close = i; break; }
    }
    if (close == std::string::npos) return sig;

    for (const std::string& part : SplitTopLevel(text.substr(open + 1, close - open - 1)))
    {
        const size_t colon = part.find(':');
        Param param;
        param.name = Trim(part.substr(0, colon));
        param.type = colon == std::string::npos ? "any" : Trim(part.substr(colon + 1));
        sig.params.push_back(std::move(param));
    }

    const size_t arrow = text.find("->", close);
    if (arrow != std::string::npos)
        sig.returns = SplitTopLevel(text.substr(arrow + 2));
    return sig;
}

void EmitDoc(std::string& out, const std::string& doc)
{
    size_t start = 0;
    while (start <= doc.size() && !doc.empty())
    {
        const size_t end = doc.find('\n', start);
        out += "---" + doc.substr(start, end == std::string::npos ? std::string::npos : end - start) + "\n";
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

void EmitFunction(std::string& out, const std::string& owner, const ApiDocs::Function& fn)
{
    EmitDoc(out, fn.doc);
    const Signature sig = ParseSignature(fn.signature);
    std::string args;
    for (const Param& param : sig.params)
    {
        out += "---@param " + param.name + " " + param.type + "\n";
        std::string argName = param.name;
        if (!argName.empty() && argName.back() == '?')
            argName.pop_back();
        args += (args.empty() ? "" : ", ") + argName;
    }
    for (const std::string& ret : sig.returns)
        out += "---@return " + ret + "\n";

    std::string qualified = fn.name;
    if (!owner.empty())
        qualified = owner + (fn.method ? ":" : ".") + fn.name;
    out += "function " + qualified + "(" + args + ") end\n\n";
}

} // namespace

ApiDocs::Block& ApiDocs::Add(Kind kind, std::string name, std::string doc)
{
    Block& block = blocks.emplace_back();
    block.kind = kind;
    block.name = std::move(name);
    block.doc = std::move(doc);
    return block;
}

std::string ApiDocs::Emit() const
{
    std::string out =
        "---@meta\n"
        "-- FoxEngine Lua API reference.\n"
        "-- GENERATED from Engine/src/Scripting/Bindings by `FoxProject api`; do not edit.\n"
        "-- Script components, lifecycle and conventions: see AGENTS.md (Scripting).\n\n";

    for (const Block& block : blocks)
    {
        switch (block.kind)
        {
        case Kind::Alias:
            EmitDoc(out, block.doc);
            out += "---@alias " + block.name + "\n";
            for (const std::string& value : block.values)
                out += "---| " + value + "\n";
            out += "\n";
            break;

        case Kind::Globals:
            for (const Function& fn : block.functions)
                EmitFunction(out, {}, fn);
            break;

        case Kind::Module:
        case Kind::Class:
            EmitDoc(out, block.doc);
            out += "---@class " + block.name;
            if (!block.base.empty())
                out += " : " + block.base;
            out += "\n";
            for (const Field& field : block.fields)
            {
                out += "---@field " + field.name + " " + field.type;
                if (!field.doc.empty())
                {
                    // A @field description must stay on the annotation's line;
                    // a raw newline would leave uncommented text in the stub.
                    std::string doc = field.doc;
                    std::replace(doc.begin(), doc.end(), '\n', ' ');
                    out += " " + doc;
                }
                out += "\n";
            }
            for (const std::string& op : block.operators)
                out += "---@operator " + op + "\n";
            if (block.declare)
                out += block.name + " = {}\n";
            out += "\n";
            for (const Function& fn : block.functions)
                EmitFunction(out, block.name, fn);
            break;
        }
    }
    return out;
}

// --- Binder -----------------------------------------------------------------

ScriptApi::ScriptApi(sol::state& lua, ApiDocs& docs)
    : m_lua(lua), m_docs(docs)
{
    m_fox = m_lua.create_table();
    // Not globals().raw_set: sol2 3.5 skips pushing the global table for a raw
    // set with a string key and writes into the registry instead.
    m_lua.set("fox", m_fox);

    ModuleState& root = m_modules.emplace_back();
    root.name = "fox";
    root.table = m_fox;
    root.getters = m_lua.create_table();
    root.setters = m_lua.create_table();
    root.doc = &m_docs.Add(ApiDocs::Kind::Module, "fox",
        "Root of the FoxEngine scripting API. Module tables are strict: reading or\n"
        "writing an unknown field raises an error instead of returning nil.");
}

ScriptApi::ModuleState& ScriptApi::EnsureModule(const std::string& name, const char* doc)
{
    const std::string fullName = name.empty() ? "fox" : "fox." + name;
    for (ModuleState& module : m_modules)
    {
        if (module.name == fullName)
        {
            if (doc && *doc && module.doc->doc.empty())
                module.doc->doc = doc;
            return module;
        }
    }

    // Create parents first so the emitted reference declares them in order.
    const size_t dot = name.rfind('.');
    ModuleState& parent = dot == std::string::npos ? m_modules.front()
                                                   : EnsureModule(name.substr(0, dot), nullptr);
    const std::string leaf = dot == std::string::npos ? name : name.substr(dot + 1);

    sol::table table = m_lua.create_table();
    parent.table.raw_set(leaf, table);

    ModuleState& module = m_modules.emplace_back();
    module.name = fullName;
    module.table = table;
    module.getters = m_lua.create_table();
    module.setters = m_lua.create_table();
    module.doc = &m_docs.Add(ApiDocs::Kind::Module, fullName, doc ? doc : "");
    return module;
}

ModuleBinder ScriptApi::Module(const std::string& name, const char* doc)
{
    ModuleState& module = EnsureModule(name, doc);
    return ModuleBinder(module.table, module.getters, module.setters, *module.doc);
}

ApiDocs::Block& ScriptApi::GlobalsDoc()
{
    if (!m_globals)
        m_globals = &m_docs.Add(ApiDocs::Kind::Globals, "", "");
    return *m_globals;
}

ApiDocs::Block& ScriptApi::DocClass(const std::string& name, const char* doc, const std::string& base,
                                    bool declare)
{
    ApiDocs::Block& block = m_docs.Add(ApiDocs::Kind::Class, name, doc);
    block.base = base;
    block.declare = declare;
    return block;
}

ApiDocs::Block& ScriptApi::DocAlias(const std::string& name, const char* doc, std::vector<std::string> values)
{
    ApiDocs::Block& block = m_docs.Add(ApiDocs::Kind::Alias, name, doc);
    block.values = std::move(values);
    return block;
}

void ScriptApi::Finalize()
{
    static const char* k_sealSource = R"lua(
local name, t, getters, setters = ...
setmetatable(t, {
    __index = function(_, key)
        local get = getters[key]
        if get ~= nil then return get() end
        error(string.format("%s has no field '%s'", name, tostring(key)), 2)
    end,
    __newindex = function(_, key, value)
        local set = setters[key]
        if set ~= nil then set(value) return end
        if getters[key] ~= nil then
            error(string.format("%s.%s is read-only", name, tostring(key)), 2)
        end
        error(string.format("%s has no field '%s'", name, tostring(key)), 2)
    end,
})
)lua";

    sol::load_result seal = m_lua.load(k_sealSource, "=fox.seal", sol::load_mode::text);
    sol::protected_function sealFn = seal;
    for (ModuleState& module : m_modules)
        sealFn(module.name, module.table, module.getters, module.setters);
}

} // namespace SE::Scripting
