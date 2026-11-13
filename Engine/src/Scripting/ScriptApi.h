#pragma once
#include <deque>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "Scripting/Sol.h"

namespace SE::Scripting {

// Thrown by bindings to raise a Lua error. sol2 catches it at the call
// boundary and calls lua_error; the dispatch message handler then prefixes
// the calling script's file:line.
class ScriptError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void Raise(const char* fmt, ...);

// LuaLS annotations collected while bindings register, emitted as the
// generated Engine/Scripts/types/fox.lua reference.
struct ApiDocs
{
    struct Field    { std::string name, type, doc; };
    struct Function { std::string name, signature, doc; bool method = false; };

    enum class Kind { Module, Class, Alias, Globals };

    struct Block
    {
        Kind kind = Kind::Module;
        std::string name;                    // "fox.input", "fox.Vec3", "fox.Key"
        std::string doc;
        std::string base;                    // classes: parent class
        bool declare = true;                 // emit "name = {}" (false: annotation-only type)
        std::vector<Field> fields;
        std::vector<std::string> operators;  // "add(fox.Vec3): fox.Vec3"
        std::vector<Function> functions;
        std::vector<std::string> values;     // alias members, as LuaLS literals
    };

    Block& Add(Kind kind, std::string name, std::string doc);
    std::string Emit() const;

    std::deque<Block> blocks;
};

// Signatures use "(name: type, optional?: type, ...: type) -> ret1, ret2".
// Every binding goes through these helpers so the docs cannot drift.
class ModuleBinder
{
public:
    ModuleBinder(sol::table table, sol::table getters, sol::table setters, ApiDocs::Block& doc)
        : m_table(std::move(table)), m_getters(std::move(getters)), m_setters(std::move(setters)),
          m_doc(&doc) {}

    template <typename F>
    ModuleBinder& Function(const char* name, const char* signature, const char* doc, F&& fn)
    {
        m_table.set_function(name, std::forward<F>(fn));
        m_doc->functions.push_back({ name, signature, doc, false });
        return *this;
    }

    template <typename G>
    ModuleBinder& ReadOnly(const char* name, const char* type, const char* doc, G&& getter)
    {
        m_getters.set_function(name, std::forward<G>(getter));
        m_doc->fields.push_back({ name, type, doc });
        return *this;
    }

    template <typename G, typename S>
    ModuleBinder& Property(const char* name, const char* type, const char* doc, G&& getter, S&& setter)
    {
        m_getters.set_function(name, std::forward<G>(getter));
        m_setters.set_function(name, std::forward<S>(setter));
        m_doc->fields.push_back({ name, type, doc });
        return *this;
    }

    // Documents a member implemented in Lua (Engine/Scripts/fox/runtime.lua).
    ModuleBinder& LuaFunction(const char* name, const char* signature, const char* doc)
    {
        m_doc->functions.push_back({ name, signature, doc, false });
        return *this;
    }

    sol::table Table() const { return m_table; }

private:
    sol::table m_table;
    sol::table m_getters;
    sol::table m_setters;
    ApiDocs::Block* m_doc;
};

template <typename T>
class ClassBinder
{
public:
    // Creates the usertype in place: sol::usertype cannot be copied or moved
    // (its reference base is private), so it must never pass through a parameter.
    template <typename... Args>
    ClassBinder(sol::table& owner, const std::string& name, ApiDocs::Block& doc, Args&&... args)
        : m_type(owner.new_usertype<T>(name, std::forward<Args>(args)...)), m_doc(&doc) {}

    template <typename M>
    ClassBinder& Field(const char* name, const char* luaType, const char* doc, M&& member)
    {
        m_type[name] = std::forward<M>(member);
        m_doc->fields.push_back({ name, luaType, doc });
        return *this;
    }

    template <typename G>
    ClassBinder& ReadOnly(const char* name, const char* luaType, const char* doc, G&& getter)
    {
        m_type[name] = sol::property(std::forward<G>(getter));
        m_doc->fields.push_back({ name, luaType, doc });
        return *this;
    }

    template <typename G, typename S>
    ClassBinder& Property(const char* name, const char* luaType, const char* doc, G&& getter, S&& setter)
    {
        m_type[name] = sol::property(std::forward<G>(getter), std::forward<S>(setter));
        m_doc->fields.push_back({ name, luaType, doc });
        return *this;
    }

    // Called with colon syntax; the signature omits self.
    template <typename F>
    ClassBinder& Method(const char* name, const char* signature, const char* doc, F&& fn)
    {
        m_type[name] = std::forward<F>(fn);
        m_doc->functions.push_back({ name, signature, doc, true });
        return *this;
    }

    // Called with dot syntax on the class table (fox.Vec3.is(v)).
    template <typename F>
    ClassBinder& Static(const char* name, const char* signature, const char* doc, F&& fn)
    {
        m_type[name] = std::forward<F>(fn);
        m_doc->functions.push_back({ name, signature, doc, false });
        return *this;
    }

    // annotation is the LuaLS operator text ("add(fox.Vec3): fox.Vec3"); empty
    // for metamethods LuaLS has no syntax for (__eq, __tostring).
    template <typename F>
    ClassBinder& Operator(sol::meta_function op, const char* annotation, F&& fn)
    {
        m_type[op] = std::forward<F>(fn);
        if (annotation && *annotation)
            m_doc->operators.push_back(annotation);
        return *this;
    }

    // Extra LuaLS operator line for an overloaded metamethod.
    ClassBinder& DocOperator(const char* annotation)
    {
        m_doc->operators.push_back(annotation);
        return *this;
    }

private:
    sol::usertype<T> m_type;
    ApiDocs::Block* m_doc;
};

class ScriptApi
{
public:
    ScriptApi(sol::state& lua, ApiDocs& docs);

    sol::state& Lua() { return m_lua; }
    sol::table Fox() { return m_fox; }
    ApiDocs& Docs() { return m_docs; }

    // fox.<name> namespace ("input", "lighting.sun"); created on first use.
    ModuleBinder Module(const std::string& name, const char* doc);

    // fox.<name> usertype.
    template <typename T, typename... Args>
    ClassBinder<T> Class(const std::string& name, const char* doc, Args&&... args)
    {
        return ClassBinder<T>(m_fox, name, m_docs.Add(ApiDocs::Kind::Class, "fox." + name, doc),
                              std::forward<Args>(args)...);
    }

    template <typename F>
    void Global(const char* name, const char* signature, const char* doc, F&& fn)
    {
        m_lua.set_function(name, std::forward<F>(fn));
        GlobalsDoc().functions.push_back({ name, signature, doc, false });
    }

    // Annotation-only declarations for types implemented in Lua or used only
    // as documentation (fox.Script, fox.PropertySpec, fox.Key).
    ApiDocs::Block& DocClass(const std::string& name, const char* doc, const std::string& base = {},
                             bool declare = false);
    ApiDocs::Block& DocAlias(const std::string& name, const char* doc, std::vector<std::string> values);

    // Makes every module table strict (unknown fields raise errors) and wires
    // its properties. Call once, after the runtime prelude has run.
    void Finalize();

private:
    struct ModuleState
    {
        std::string name;
        sol::table table;
        sol::table getters;
        sol::table setters;
        ApiDocs::Block* doc = nullptr;
    };

    ModuleState& EnsureModule(const std::string& name, const char* doc);
    ApiDocs::Block& GlobalsDoc();

    sol::state& m_lua;
    ApiDocs& m_docs;
    sol::table m_fox;
    std::deque<ModuleState> m_modules;
    ApiDocs::Block* m_globals = nullptr;
};

} // namespace SE::Scripting
