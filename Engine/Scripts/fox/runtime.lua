-- FoxEngine script runtime. Loaded into every VM before any game script.
-- Implements the Lua side of the API documented in Engine/Scripts/types/fox.lua
-- (fox.script, fox.Script, fox.inspect, fox.wait, fox.test) and returns the
-- internal table the C++ ScriptSystem drives. Game scripts never require it.
local host = ...

local rt = {}
local is_vec3 = fox.Vec3.is

---------------------------------------------------------------------------
-- fox.inspect

local function sort_keys(a, b)
    local ta, tb = type(a), type(b)
    if ta ~= tb then return ta < tb end
    if ta == "number" or ta == "string" then return a < b end
    return tostring(a) < tostring(b)
end

local function is_identifier(key)
    return type(key) == "string" and key:match("^[%a_][%w_]*$") ~= nil
end

local function inspect(value, depth, indent, seen)
    local kind = type(value)
    if kind == "string" then return (string.format("%q", value):gsub("\\\n", "\\n")) end
    if kind ~= "table" then return tostring(value) end
    if seen[value] then return "<cycle>" end

    local mt = getmetatable(value)
    local instance = type(mt) == "table" and rawget(mt, "__fox_class") == true
    local prefix = instance and (tostring(rawget(mt, "__name")) .. " ") or ""
    if depth <= 0 then return prefix .. "{...}" end

    seen[value] = true
    local inner = indent .. "  "
    local parts = {}
    local count = #value
    for i = 1, count do
        parts[#parts + 1] = inspect(value[i], depth - 1, inner, seen)
    end
    local keys = {}
    for key in pairs(value) do
        local in_array = math.type(key) == "integer" and key >= 1 and key <= count
        local internal = instance and type(key) == "string" and key:sub(1, 1) == "_"
        if not in_array and not internal then keys[#keys + 1] = key end
    end
    table.sort(keys, sort_keys)
    for _, key in ipairs(keys) do
        local label = is_identifier(key) and key or ("[" .. inspect(key, depth - 1, inner, seen) .. "]")
        parts[#parts + 1] = label .. " = " .. inspect(rawget(value, key), depth - 1, inner, seen)
    end
    seen[value] = nil

    if #parts == 0 then return prefix .. "{}" end
    local line = prefix .. "{ " .. table.concat(parts, ", ") .. " }"
    if #line <= 80 and not line:find("\n", 1, true) then return line end
    return prefix .. "{\n" .. inner .. table.concat(parts, ",\n" .. inner) .. "\n" .. indent .. "}"
end

function fox.inspect(value, depth)
    return inspect(value, depth or 3, "", {})
end
rt.inspect = fox.inspect

---------------------------------------------------------------------------
-- Script classes and properties

local Script = {}
fox.Script = Script
local CLASS_MT = { __index = Script }

local PROPERTY_TYPES = {
    number = true, integer = true, boolean = true, string = true, vec3 = true,
    color = true, enum = true, entity = true, scene = true,
}
local SPEC_KEYS = {
    name = true, type = true, default = true, min = true, max = true,
    step = true, tooltip = true, options = true,
}
local RESERVED = { entity = true, transform = true, enabled = true }

local function type_name(value)
    if is_vec3(value) then return "vec3" end
    return type(value)
end

local function copy_value(value)
    if is_vec3(value) then return vec3(value.x, value.y, value.z) end
    return value
end

-- Converts a Lua or JSON-decoded value to the property's type.
-- Returns true, value or false, reason.
local function coerce(spec, value)
    local kind = spec.type
    if kind == "number" then
        if type(value) == "number" then return true, value end
    elseif kind == "integer" then
        if math.type(value) == "integer" then return true, value end
        if type(value) == "number" and value == math.floor(value) then return true, math.tointeger(value) end
    elseif kind == "boolean" then
        if type(value) == "boolean" then return true, value end
    elseif kind == "string" or kind == "scene" then
        if type(value) == "string" then return true, value end
    elseif kind == "vec3" or kind == "color" then
        if is_vec3(value) then return true, copy_value(value) end
        if type(value) == "table" and #value == 3 and type(value[1]) == "number"
            and type(value[2]) == "number" and type(value[3]) == "number" then
            return true, vec3(value[1], value[2], value[3])
        end
    elseif kind == "enum" then
        if type(value) == "string" then
            for _, option in ipairs(spec.options) do
                if option == value then return true, value end
            end
            return false, string.format("'%s' is not one of: %s", value, table.concat(spec.options, ", "))
        end
    elseif kind == "entity" then
        if value == nil then return true, nil end
        return host.resolve_entity(value)
    end
    return false, string.format("expected %s, got %s", kind, type_name(value))
end

local function default_for(spec)
    local kind = spec.type
    if kind == "number" or kind == "integer" then return 0 end
    if kind == "boolean" then return false end
    if kind == "string" or kind == "scene" then return "" end
    if kind == "vec3" then return vec3(0, 0, 0) end
    if kind == "color" then return vec3(1, 1, 1) end
    if kind == "enum" then return spec.options[1] end
    return nil
end

-- Called through normalize_properties: level 5 (past fail, this function,
-- normalize_properties and fox.script / fox.actor) is the calling script line.
local function normalize_property(spec, index, names, api)
    local function fail(message) error(message, 5) end
    local where = string.format("%s: property #%d", api, index)
    if type(spec) ~= "table" then fail(where .. " must be a table { name = ..., type = ... }") end
    for key in pairs(spec) do
        if not SPEC_KEYS[key] then
            fail(string.format("%s: unknown key '%s' (allowed: name, type, default, min, max, step, tooltip, options)",
                where, tostring(key)))
        end
    end
    local name = spec.name
    if type(name) ~= "string" or not is_identifier(name) then fail(where .. ": name must be an identifier") end
    where = string.format("%s: property '%s'", api, name)
    if RESERVED[name] or name:sub(1, 1) == "_" or name:sub(1, 3) == "on_" then
        fail(where .. ": the name is reserved (entity, transform, enabled, _* and on_* belong to the runtime)")
    end
    if names[name] then fail(where .. " is declared twice") end
    names[name] = true
    if not PROPERTY_TYPES[spec.type] then
        fail(string.format("%s: unknown type '%s' (number, integer, boolean, string, vec3, color, enum, entity, scene)",
            where, tostring(spec.type)))
    end

    local out = { name = name, type = spec.type, min = spec.min, max = spec.max, step = spec.step, tooltip = spec.tooltip }
    for _, key in ipairs({ "min", "max", "step" }) do
        if out[key] ~= nil and type(out[key]) ~= "number" then fail(string.format("%s: %s must be a number", where, key)) end
    end
    if out.tooltip ~= nil and type(out.tooltip) ~= "string" then fail(where .. ": tooltip must be a string") end
    if spec.type == "enum" then
        if type(spec.options) ~= "table" or #spec.options == 0 then
            fail(where .. ": enum properties need a non-empty options list")
        end
        out.options = {}
        for i, option in ipairs(spec.options) do
            if type(option) ~= "string" then fail(string.format("%s: option #%d must be a string", where, i)) end
            out.options[i] = option
        end
    elseif spec.options ~= nil then
        fail(where .. ": options only apply to enum properties")
    end

    if spec.default == nil then
        out.default = default_for(out)
    elseif spec.type == "entity" then
        fail(where .. ": entity properties cannot have a default")
    else
        local ok, value = coerce(out, spec.default)
        if not ok then fail(string.format("%s: invalid default (%s)", where, value)) end
        out.default = value
    end
    return out
end

-- Validates a fox.script / fox.actor properties array (error level 3 is the
-- caller of fox.script / fox.actor).
local function normalize_properties(list, api)
    list = list or {}
    if type(list) ~= "table" then error(api .. ": properties must be an array", 3) end
    for key in pairs(list) do
        if math.type(key) ~= "integer" then
            error(api .. ": properties must be an array of { name = ..., type = ... } (order = editor order)", 3)
        end
    end
    local properties, names = {}, {}
    for i, spec in ipairs(list) do
        properties[i] = normalize_property(spec, i, names, api)
    end
    return properties
end

function fox.script(definition)
    if definition == nil then definition = {} end
    if type(definition) ~= "table" then
        error("fox.script expects a table: fox.script { properties = { ... } }", 2)
    end
    for key in pairs(definition) do
        if key ~= "properties" then
            error(string.format("fox.script: unknown key '%s' (allowed: properties)", tostring(key)), 2)
        end
    end
    local class = { __fox_class = true, __properties = normalize_properties(definition.properties, "fox.script"),
                    __name = "Script" }
    class.__index = class
    return setmetatable(class, CLASS_MT)
end

---------------------------------------------------------------------------
-- Actor classes

local Actor = setmetatable({}, { __index = Script })
fox.Actor = Actor

local function copy_component(source)
    local out = {}
    for key, value in pairs(source) do out[key] = copy_value(value) end
    return out
end

-- Components: the base's list, then this definition's entries. An entry
-- named like a base component (same or no type) changes only the keys it
-- sets; with another type it replaces the base component in place.
local function merge_components(base, list)
    list = list or {}
    if type(list) ~= "table" then error("fox.actor: components must be an array", 3) end
    for key in pairs(list) do
        if math.type(key) ~= "integer" then
            error("fox.actor: components must be an array of { name = ..., type = ... }", 3)
        end
    end
    local components, index_of = {}, {}
    if base then
        for i, component in ipairs(base.__components) do
            components[i] = copy_component(component)
            index_of[component.name] = i
        end
    end
    local declared = {}
    for i, entry in ipairs(list) do
        local where = string.format("fox.actor: component #%d", i)
        if type(entry) ~= "table" then error(where .. " must be a table { name = ..., type = ... }", 3) end
        local name = entry.name
        if type(name) ~= "string" or not is_identifier(name) then
            error(where .. ": name must be an identifier (the component is self.<name>)", 3)
        end
        where = string.format("fox.actor: component '%s'", name)
        if RESERVED[name] or name:sub(1, 1) == "_" or name:sub(1, 3) == "on_" then
            error(where .. ": the name is reserved (entity, transform, enabled, _* and on_* belong to the runtime)", 3)
        end
        if declared[name] then error(where .. " is declared twice", 3) end
        declared[name] = true
        local index = index_of[name]
        local merged
        if index and (entry.type == nil or entry.type == components[index].type) then
            merged = components[index]
            for key, value in pairs(entry) do merged[key] = copy_value(value) end
        else
            merged = copy_component(entry)
            if not index then
                index = #components + 1
                index_of[name] = index
            end
            components[index] = merged
        end
        local ok, reason = host.validate_component(merged)
        if not ok then error("fox.actor: " .. reason, 3) end
    end
    return components
end

function fox.actor(definition)
    if definition == nil then definition = {} end
    if type(definition) ~= "table" then
        error("fox.actor expects a table: fox.actor { components = { ... }, properties = { ... } }", 2)
    end
    for key in pairs(definition) do
        if key ~= "base" and key ~= "components" and key ~= "properties" then
            error(string.format("fox.actor: unknown key '%s' (allowed: base, components, properties)", tostring(key)), 2)
        end
    end
    local base = definition.base
    if base ~= nil and not (type(base) == "table" and rawget(base, "__fox_actor")) then
        error("fox.actor: base must be another fox.actor class (base = require(\"Actors.Lamp\"))", 2)
    end

    -- Base properties first; redeclaring one replaces it (a new default).
    local properties, index_of = {}, {}
    if base then
        for i, spec in ipairs(base.__properties) do
            properties[i] = spec
            index_of[spec.name] = i
        end
    end
    for _, spec in ipairs(normalize_properties(definition.properties, "fox.actor")) do
        local index = index_of[spec.name] or #properties + 1
        properties[index] = spec
        index_of[spec.name] = index
    end

    local components = merge_components(base, definition.components)
    for _, component in ipairs(components) do
        if index_of[component.name] then
            error(string.format("fox.actor: component '%s' has the name of a property", component.name), 2)
        end
    end

    local class = { __fox_class = true, __fox_actor = true, __properties = properties,
                    __components = components, __base = base, __name = "Actor" }
    class.__index = class
    return setmetatable(class, { __index = base or Actor })
end

function Actor:destroy()
    fox.world.destroy(self)
end

function Actor:is_a(class)
    local current = getmetatable(self)
    while current do
        if current == class then return true end
        current = rawget(current, "__base")
    end
    return false
end

local function find_property(class, name)
    for _, spec in ipairs(class.__properties) do
        if spec.name == name then return spec end
    end
    return nil
end

-- C++: creates an instance from JSON-decoded property overrides. components
-- (actors only) maps component names to their fox.ActorComponent handles.
function rt.new_instance(class, overrides, path, owner, entity, transform, components)
    local self = setmetatable({}, class)
    self.enabled = true
    self.entity = entity
    self.transform = transform
    rawset(self, "_owner", owner)
    rawset(self, "_path", path)
    if components then
        for name, handle in pairs(components) do rawset(self, name, handle) end
    end
    for _, spec in ipairs(class.__properties) do
        local value = overrides and overrides[spec.name]
        if value ~= nil then
            local ok, coerced = coerce(spec, value)
            if ok then
                value = coerced
            else
                host.warn(path, string.format("property '%s': %s; using the default", spec.name, coerced))
                value = copy_value(spec.default)
            end
        else
            value = copy_value(spec.default)
        end
        self[spec.name] = value
    end
    if overrides then
        for key in pairs(overrides) do
            if not find_property(class, key) then
                host.warn(path, string.format("unknown property '%s' (the script does not declare it)", tostring(key)))
            end
        end
    end
    return self
end

-- C++: the property schema for Describe.
function rt.properties(class)
    return class.__properties
end

---------------------------------------------------------------------------
-- Hot reload

local CLASS_INTERNAL = { __index = true, __name = true, __path = true }

-- Patches a reloaded module table in place so existing references (instance
-- metatables, other modules' upvalues) see the new functions.
function rt.patch(old, new)
    local class = rawget(old, "__fox_class") and rawget(new, "__fox_class")
    for key in pairs(old) do
        if rawget(new, key) == nil and not (class and CLASS_INTERNAL[key]) then rawset(old, key, nil) end
    end
    for key, value in pairs(new) do
        if not (class and CLASS_INTERNAL[key]) then rawset(old, key, value) end
    end
    local mt = getmetatable(new)
    if getmetatable(old) ~= mt then setmetatable(old, mt) end
end

-- C++: gives a reloaded instance defaults for newly declared properties.
function rt.fill_defaults(self)
    local class = getmetatable(self)
    for _, spec in ipairs(class.__properties) do
        if rawget(self, spec.name) == nil and spec.default ~= nil then
            self[spec.name] = copy_value(spec.default)
        end
    end
end

---------------------------------------------------------------------------
-- Coroutines

local WAIT_SECONDS, WAIT_FRAMES = {}, {}

local function require_coroutine(name)
    if not coroutine.isyieldable() then
        error(name .. " only works inside a coroutine started with self:start_coroutine", 3)
    end
end

function fox.wait(seconds)
    if type(seconds) ~= "number" then error("fox.wait expects a number of seconds", 2) end
    require_coroutine("fox.wait")
    coroutine.yield(WAIT_SECONDS, seconds)
end

function fox.wait_frames(frames)
    if math.type(frames) ~= "integer" or frames < 0 then error("fox.wait_frames expects a non-negative integer", 2) end
    require_coroutine("fox.wait_frames")
    coroutine.yield(WAIT_FRAMES, frames)
end

local function step(record)
    local ok, kind, amount = coroutine.resume(record.co)
    if not ok then
        record.done = true
        error(debug.traceback(record.co, kind), 0)
    end
    if coroutine.status(record.co) == "dead" then
        record.done = true
        return
    end
    record.wake_time, record.wake_frame = nil, nil
    if kind == WAIT_SECONDS then
        record.wake_time = fox.time.time + amount
    elseif kind == WAIT_FRAMES then
        record.wake_frame = fox.time.frame + amount
    else
        record.wake_frame = fox.time.frame + 1
    end
end

function Script:start_coroutine(fn, ...)
    if type(fn) ~= "function" then error("start_coroutine expects a function", 2) end
    local args = table.pack(...)
    local record = { co = coroutine.create(function() return fn(table.unpack(args, 1, args.n)) end) }
    local list = rawget(self, "_coroutines")
    if not list then
        list = {}
        rawset(self, "_coroutines", list)
    end
    list[#list + 1] = record
    step(record)
    return record.co
end

function Script:stop_coroutines()
    local list = rawget(self, "_coroutines")
    if list then
        for _, record in ipairs(list) do record.done = true end
    end
    rawset(self, "_coroutines", nil)
end

function Script:get_script(path)
    if type(path) ~= "string" then error("get_script expects a script path", 2) end
    return host.find_script(rawget(self, "_owner") or 0, path)
end

-- C++: resumes this instance's due coroutines once per frame.
function rt.resume_coroutines(self)
    local list = rawget(self, "_coroutines")
    if not list then return end
    local now, frame = fox.time.time, fox.time.frame
    local i = 1
    while i <= #list do
        local record = list[i]
        if not record.done and (record.wake_time == nil or now >= record.wake_time)
            and (record.wake_frame == nil or frame >= record.wake_frame) then
            step(record)
        end
        if record.done then table.remove(list, i) else i = i + 1 end
    end
    if #list == 0 and rawget(self, "_coroutines") == list then rawset(self, "_coroutines", nil) end
end

---------------------------------------------------------------------------
-- fox.test

local cases = {}

local function deep_equal(a, b)
    if a == b then return true end
    if type(a) ~= "table" or type(b) ~= "table" then return false end
    for key, value in pairs(a) do
        if not deep_equal(value, b[key]) then return false end
    end
    for key in pairs(b) do
        if a[key] == nil then return false end
    end
    return true
end

function fox.test.case(name, fn)
    if type(name) ~= "string" or type(fn) ~= "function" then
        error("fox.test.case expects (name, function)", 2)
    end
    cases[#cases + 1] = { name = name, fn = fn }
end

function fox.test.expect(condition, message)
    if not condition then error(message or "expectation failed", 2) end
end

function fox.test.expect_eq(actual, expected)
    if not deep_equal(actual, expected) then
        error(string.format("expected %s, got %s", fox.inspect(expected), fox.inspect(actual)), 2)
    end
end

function fox.test.expect_near(actual, expected, epsilon)
    epsilon = epsilon or 1e-6
    local ok
    if type(actual) == "number" and type(expected) == "number" then
        ok = math.abs(actual - expected) <= epsilon
    elseif is_vec3(actual) and is_vec3(expected) then
        ok = math.abs(actual.x - expected.x) <= epsilon and math.abs(actual.y - expected.y) <= epsilon
            and math.abs(actual.z - expected.z) <= epsilon
    else
        error("expect_near compares two numbers or two fox.Vec3 values", 2)
    end
    if not ok then
        error(string.format("expected %s (within %g), got %s", fox.inspect(expected), epsilon, fox.inspect(actual)), 2)
    end
end

-- C++: cases registered by the test file that just ran.
function rt.take_test_cases()
    local taken = cases
    cases = {}
    return taken
end

---------------------------------------------------------------------------
-- Sandbox finish: accidental globals are errors (state belongs on self).

setmetatable(_G, {
    __newindex = function(_, name)
        error(string.format("assignment to undeclared global '%s' (use a local, or keep state on self)",
            tostring(name)), 2)
    end,
})

return rt
