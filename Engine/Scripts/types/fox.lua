---@meta
-- FoxEngine Lua API reference.
-- GENERATED from Engine/src/Scripting/Bindings by `FoxProject api`; do not edit.
-- Script components, lifecycle and conventions: see AGENTS.md (Scripting).

---Root of the FoxEngine scripting API. Module tables are strict: reading or
---writing an unknown field raises an error instead of returning nil.
---@class fox
fox = {}

---Declares a script class. Return it from the module:
---```lua
---local Spinner = fox.script { properties = {
---    { name = "speed", type = "number", default = 90 },
---} }
---function Spinner:on_update(dt) self.transform:rotate(vec3(0, self.speed * dt, 0)) end
---return Spinner
---```
---@param definition? fox.ScriptDefinition
---@return fox.Script
function fox.script(definition) end

---Declares an actor class: a placeable, spawnable object built from components.
---Each component becomes self.<name> (a fox.ActorComponent). Return it from the module:
---```lua
---local Lamp = fox.actor {
---    components = {
---        { name = "mesh", type = "mesh", mesh = "Assets/Props/Lamp.fbx" },
---        { name = "light", type = "point_light", color = vec3(1, 0.8, 0.6), position = vec3(0, 2, 0) },
---    },
---    properties = { { name = "flicker", type = "boolean", default = false } },
---}
---function Lamp:on_update(dt) self.light.intensity = 1 + math.sin(fox.time.time * 9) * 0.2 end
---return Lamp
---```
---@param definition? fox.ActorDefinition
---@return fox.Actor
function fox.actor(definition) end

---Pretty-prints a value (tables sorted by key, cycles marked; default depth 3).
---@param value any
---@param depth? integer
---@return string
function fox.inspect(value, depth) end

---Inside a coroutine: suspends for the given scaled seconds.
---@param seconds number
---@return nil
function fox.wait(seconds) end

---Inside a coroutine: suspends for the given number of frames.
---@param frames integer
---@return nil
function fox.wait_frames(frames) end

---Engine log output (FoxEngine.log, the Debug-build console window and the editor Console).
---Lines are tagged with the calling script's file(line). The global print logs at info level.
---@class fox.log
fox.log = {}

---Logs the arguments, tab-separated, at info level.
---@param ... any
---@return nil
function fox.log.info(...) end

---Logs at warning level.
---@param ... any
---@return nil
function fox.log.warn(...) end

---Logs at error level without raising (use the Lua error() to abort).
---@param ... any
---@return nil
function fox.log.error(...) end

---Frame timing. Scaled values follow fox.time.scale; unscaled ones do not.
---@class fox.time
---@field dt number Scaled seconds since the previous frame (0 while paused).
---@field unscaled_dt number Real seconds since the previous frame.
---@field time number Scaled seconds since the session started.
---@field real_time number Real seconds since the session started.
---@field frame integer Frames simulated since the session started.
---@field fps number Frames per second from the last frame time.
---@field scale number Time scale (read/write). 0 pauses physics, particles, scaled time and fox.wait timers; on_update still runs with dt = 0 and on_gui is unaffected.
fox.time = {}

---Application state.
---@class fox.app
fox.app = {}

---Closes the game (the player exits; the editor stops Play mode).
---@return nil
function fox.app.quit() end

---True while playing inside the editor.
---@return boolean
function fox.app.is_editor() end

---True in a published build (no console, no hot reload).
---@return boolean
function fox.app.is_published() end

---The name from project.json.
---@return string
function fox.app.project_name() end

---The OS window hosting the game.
---@class fox.window
---@field fullscreen boolean Borderless fullscreen (read/write; F11 toggles it too).
fox.window = {}

---Client area width and height in pixels.
---@return integer
---@return integer
function fox.window.size() end

---Sets the window title (the player appends fps in development builds).
---@param title string
---@return nil
function fox.window.set_title(title) end

---Base of every script class. Instances are tables: store state on self
---(module-level locals reset on hot reload). Callbacks are optional methods.
---Order: on_enable, on_start, then every frame coroutines, on_update, physics
---steps (on_fixed_update), on_late_update, on_gui. A callback that raises an
---error stops the instance until its file is saved again (hot reload).
---@class fox.Script
---@field entity fox.Entity? Owning entity (nil for autoloads).
---@field transform fox.Transform? Shortcut for self.entity.transform.
---@field enabled boolean Disabled scripts receive no updates; toggling calls on_enable/on_disable.
---@field on_start fun(self)? Once, before the first on_update (after the first on_enable).
---@field on_update fun(self, dt: number)? Every frame with scaled dt.
---@field on_fixed_update fun(self, dt: number)? Before every physics step (dt = fox.physics.fixed_dt).
---@field on_late_update fun(self, dt: number)? After physics and every on_update.
---@field on_gui fun(self)? Inside the UI frame; the only place fox.ui works.
---@field on_enable fun(self)? When the script becomes active: on its first frame and after re-enabling.
---@field on_disable fun(self)? When the script or its entity becomes inactive, and before on_destroy.
---@field on_destroy fun(self)? When the entity is destroyed, the scene unloads or the session ends (after on_start ran).
---@field on_reload fun(self)? After hot reload patched the class (new properties got their defaults).
---@field on_scene_loaded fun(self, scene: fox.SceneInfo)? Autoloads only: after each scene becomes ready (after on_start).
fox.Script = {}

---Runs fn as a coroutine owned by this script: it starts immediately and is
---resumed every frame (use fox.wait/fox.wait_frames). Stops on destroy.
---@param fn fun(...)
---@param ... any
---@return thread
function fox.Script:start_coroutine(fn, ...) end

---Stops every coroutine started by this script.
---@return nil
function fox.Script:stop_coroutines() end

---Another script instance on the same entity (autoloads: another autoload).
---@param path string
---@return fox.Script?
function fox.Script:get_script(path) end

---Base of every actor class (fox.actor). An actor is a root entity (self.entity,
---self.transform) whose components are child entities. Actors get every fox.Script
---callback; on_destroy runs when the actor is destroyed or its scene unloads.
---@class fox.Actor : fox.Script
fox.Actor = {}

---Destroys the actor at the end of this frame (fox.world.destroy).
---@return nil
function fox.Actor:destroy() end

---True if the actor's class is class or derives from it.
---@param class fox.Actor
---@return boolean
function fox.Actor:is_a(class) end

---Argument of fox.actor.
---@class fox.ActorDefinition
---@field base fox.Actor? Base class: its components and properties come first; same-named entries override them.
---@field components fox.ComponentSpec[]? The actor's parts.
---@field properties fox.PropertySpec[]? Editable properties in editor order.

---One actor component. Levels override any key except name and type per
---placement. Transforms are relative to the actor.
---@class fox.ComponentSpec
---@field name string Becomes self.<name> (an identifier; not a property name).
---@field type "mesh"|"point_light"|"particle_emitter"|"box"|"sphere"
---@field position fox.Vec3?
---@field rotation fox.Vec3? Euler degrees.
---@field scale fox.Vec3?
---@field mesh string? mesh: model path (FBX, glTF, OBJ).
---@field cast_shadows boolean? mesh, box, sphere (default true).
---@field receive_shadows boolean? mesh (default true).
---@field alpha_cutoff number? mesh: alpha-test threshold for textures with alpha (0 = off).
---@field collision boolean? mesh: kinematic triangle collider that follows the component (default false); box/sphere: the shape's collider (default true; false = rendered only, e.g. view models).
---@field color fox.Vec3? point_light: linear RGB (default white); box/sphere: albedo 0..1 (default 0.8 grey).
---@field emissive fox.Vec3? box/sphere: linear HDR glow (default 0, 0, 0); above 1 blooms.
---@field intensity number? point_light (default 1).
---@field radius number? point_light range (default 10); sphere radius (default 0.5).
---@field preset string? particle_emitter: Assets/Particles/Presets/*.json.
---@field size fox.Vec3? box: full extents (default 1, 1, 1).
---@field visible boolean? box/sphere: render the shape (default true); false = collider only.
---@field body "kinematic"|"dynamic"? box/sphere: kinematic follows the actor (default); dynamic is simulated and moves the actor (one per actor; it is then the actor's only collision).
---@field mass number? Dynamic body mass in kg (default 1).
---@field friction number? Default 0.5.
---@field restitution number? Bounciness 0..1 (default 0.2).

---Argument of fox.script.
---@class fox.ScriptDefinition
---@field properties fox.PropertySpec[]? Editable properties in editor order.

---One editable property. Scenes store only overrides; the default comes from
---the script, so changing it updates every instance without an override.
---@class fox.PropertySpec
---@field name string Field name on self (not entity/transform/enabled, _* or on_*).
---@field type fox.PropertyType
---@field default any? Defaults: 0, false, "", vec3(0,0,0), color vec3(1,1,1), first option, nil entity.
---@field min number? Editor slider minimum.
---@field max number? Editor slider maximum.
---@field step number? Editor drag speed.
---@field tooltip string?
---@field options string[]? Allowed values of an enum property.

---Script property types and their Lua values.
---@alias fox.PropertyType
---| "number"
---| "integer"
---| "boolean"
---| "string"
---| "vec3"
---| "color" # fox.Vec3 with rgb in 0..1
---| "enum" # string from options
---| "entity" # fox.Entity or nil
---| "scene" # scene path string

---Unit tests for *_test.lua files, run headless by `FoxProject test`.
---```lua
---fox.test.case("clamps pitch", function()
---    fox.test.expect_eq(Look.clamp_pitch(120), 89)
---end)
---```
---@class fox.test
fox.test = {}

---Registers a test case.
---@param name string
---@param fn fun()
---@return nil
function fox.test.case(name, fn) end

---Fails the case when condition is falsy.
---@param condition any
---@param message? string
---@return nil
function fox.test.expect(condition, message) end

---Fails unless the values are equal (tables compared deeply).
---@param actual any
---@param expected any
---@return nil
function fox.test.expect_eq(actual, expected) end

---Fails unless the values differ by at most epsilon (default 1e-6).
---@param actual number|fox.Vec3
---@param expected number|fox.Vec3
---@param epsilon? number
---@return nil
function fox.test.expect_near(actual, expected, epsilon) end

---Creates a fox.Vec3; missing components are 0.
---@param x? number
---@param y? number
---@param z? number
---@return fox.Vec3
function vec3(x, y, z) end

---3D vector value. Operators return new vectors. Engine properties such as
---transform.position return copies: modify the copy, then assign it back
---(`local p = t.position; p.y = 2; t.position = p`).
---@class fox.Vec3
---@field x number
---@field y number
---@field z number
---@operator add(fox.Vec3): fox.Vec3
---@operator sub(fox.Vec3): fox.Vec3
---@operator mul(number): fox.Vec3
---@operator mul(fox.Vec3): fox.Vec3
---@operator div(number): fox.Vec3
---@operator unm: fox.Vec3
fox.Vec3 = {}

---Euclidean length.
---@return number
function fox.Vec3:length() end

---Unit-length copy; the zero vector stays zero.
---@return fox.Vec3
function fox.Vec3:normalized() end

---Dot product.
---@param other fox.Vec3
---@return number
function fox.Vec3:dot(other) end

---Cross product (left-handed engine axes).
---@param other fox.Vec3
---@return fox.Vec3
function fox.Vec3:cross(other) end

---Linear interpolation; t = 0 gives self.
---@param other fox.Vec3
---@param t number
---@return fox.Vec3
function fox.Vec3:lerp(other, t) end

---Distance between two points.
---@param other fox.Vec3
---@return number
function fox.Vec3:distance(other) end

---True if value is a fox.Vec3.
---@param value any
---@return boolean
function fox.Vec3.is(value) end

---Key, mouse button or gamepad button name (matching ignores case).
---Gamepad.* names match any connected pad.
---@alias fox.Key
---| "A"
---| "B"
---| "C"
---| "D"
---| "E"
---| "F"
---| "G"
---| "H"
---| "I"
---| "J"
---| "K"
---| "L"
---| "M"
---| "N"
---| "O"
---| "P"
---| "Q"
---| "R"
---| "S"
---| "T"
---| "U"
---| "V"
---| "W"
---| "X"
---| "Y"
---| "Z"
---| "0"
---| "1"
---| "2"
---| "3"
---| "4"
---| "5"
---| "6"
---| "7"
---| "8"
---| "9"
---| "F1"
---| "F2"
---| "F3"
---| "F4"
---| "F5"
---| "F6"
---| "F7"
---| "F8"
---| "F9"
---| "F10"
---| "F11"
---| "F12"
---| "Space"
---| "Enter"
---| "Escape"
---| "Tab"
---| "Backspace"
---| "Insert"
---| "Delete"
---| "Home"
---| "End"
---| "PageUp"
---| "PageDown"
---| "Up"
---| "Down"
---| "Left"
---| "Right"
---| "Shift"
---| "Ctrl"
---| "Alt"
---| "CapsLock"
---| "Backquote"
---| "Minus"
---| "Equals"
---| "LeftBracket"
---| "RightBracket"
---| "Backslash"
---| "Semicolon"
---| "Quote"
---| "Comma"
---| "Period"
---| "Slash"
---| "Numpad0"
---| "Numpad1"
---| "Numpad2"
---| "Numpad3"
---| "Numpad4"
---| "Numpad5"
---| "Numpad6"
---| "Numpad7"
---| "Numpad8"
---| "Numpad9"
---| "NumpadAdd"
---| "NumpadSubtract"
---| "NumpadMultiply"
---| "NumpadDivide"
---| "NumpadDecimal"
---| "MouseLeft"
---| "MouseRight"
---| "MouseMiddle"
---| "Gamepad.A"
---| "Gamepad.B"
---| "Gamepad.X"
---| "Gamepad.Y"
---| "Gamepad.LeftShoulder"
---| "Gamepad.RightShoulder"
---| "Gamepad.LeftThumb"
---| "Gamepad.RightThumb"
---| "Gamepad.Start"
---| "Gamepad.Back"
---| "Gamepad.DpadUp"
---| "Gamepad.DpadDown"
---| "Gamepad.DpadLeft"
---| "Gamepad.DpadRight"

---Gamepad button name for fox.input.gamepad_down/pressed.
---@alias fox.GamepadButton
---| "A"
---| "B"
---| "X"
---| "Y"
---| "LeftShoulder"
---| "RightShoulder"
---| "LeftThumb"
---| "RightThumb"
---| "Start"
---| "Back"
---| "DpadUp"
---| "DpadDown"
---| "DpadLeft"
---| "DpadRight"

---Snapshot returned by fox.input.gamepad.
---@class fox.Gamepad
---@field connected boolean
---@field left_x number Left stick, -1..1 with dead zone.
---@field left_y number
---@field right_x number
---@field right_y number
---@field left_trigger number 0..1
---@field right_trigger number 0..1

---Keyboard, mouse and gamepad state for this frame. Everything reads as
---neutral (nothing held, zero deltas) while the game has no input focus.
---@class fox.input
fox.input = {}

---True while the key is held.
---@param key fox.Key
---@return boolean
function fox.input.key_down(key) end

---True on the frame the key went down.
---@param key fox.Key
---@return boolean
function fox.input.key_pressed(key) end

---True on the frame the key went up.
---@param key fox.Key
---@return boolean
function fox.input.key_released(key) end

---True while any key bound to the project.json action is held.
---@param action string
---@return boolean
function fox.input.action_down(action) end

---True on the frame the action started.
---@param action string
---@return boolean
function fox.input.action_pressed(action) end

---True on the frame the action ended.
---@param action string
---@return boolean
function fox.input.action_released(action) end

---Mouse movement in pixels this frame (x right, y down).
---@return number
---@return number
function fox.input.mouse_delta() end

---Cursor position relative to the game view.
---@return number
---@return number
function fox.input.mouse_position() end

---Wheel notches this frame (positive = away from the user).
---@return number
function fox.input.mouse_wheel() end

---Stick and trigger state of pad 0..3 (default 0).
---@param index? integer
---@return fox.Gamepad
function fox.input.gamepad(index) end

---True while the button is held on pad index (default 0).
---@param button fox.GamepadButton
---@param index? integer
---@return boolean
function fox.input.gamepad_down(button, index) end

---True on the frame the button went down on pad index (default 0).
---@param button fox.GamepadButton
---@param index? integer
---@return boolean
function fox.input.gamepad_pressed(button, index) end

---Sets the pad's motor speeds (0..1).
---@param index integer
---@param left number
---@param right number
---@return nil
function fox.input.set_rumble(index, left, right) end

---Hides and locks the cursor for mouse-look (mouse_delta keeps working).
---Ignored without input focus; released automatically when focus is lost.
---@param capture boolean
---@return nil
function fox.input.capture_cursor(capture) end

---True while the cursor is captured.
---@return boolean
function fox.input.cursor_captured() end

---Handle to an entity of the loaded scene. Handles stay valid until the
---scene changes; using one afterwards raises an error (check is_valid).
---@class fox.Entity
---@field id integer Runtime id, unique within the loaded scene.
---@field source_id integer Id in the scene file (0 for entities created at runtime).
---@field name string
---@field kind string actor, actor_component, mesh, primitive, point_light, particle_emitter, camera, sun, rigid_body or empty.
---@field actor fox.Actor? The actor this entity is the root or a component of (its class instance), if any.
---@field active boolean Inactive entities are hidden and their scripts receive no updates (on_disable/on_enable run on the transitions). Not inherited by children.
---@field transform fox.Transform
---@field parent fox.Entity? Parent entity, if any.
fox.Entity = {}

---Direct children in scene order.
---@return fox.Entity[]
function fox.Entity:children() end

---False once the entity or its scene is gone.
---@return boolean
function fox.Entity:is_valid() end

---The instance of the script at path on this entity ("Assets/Scripts/Door.lua").
---@param path string
---@return fox.Script?
function fox.Entity:get_script(path) end

---Attaches a script component at runtime (not saved). Its on_start runs next frame.
---@param path string
---@param properties? table
---@return fox.Script
function fox.Entity:add_script(path, properties) end

---One component of an actor (self.<name> on the actor), declared in its
---fox.actor class. Each is a child entity of the actor: its transform is
---relative to the actor, and moving it moves what it renders and its
---kinematic collider. Type-specific fields raise on other types.
---@class fox.ActorComponent
---@field name string Component name from the class.
---@field type string mesh, point_light, particle_emitter, box or sphere.
---@field entity fox.Entity The component's child entity.
---@field transform fox.Transform Transform relative to the actor.
---@field actor fox.Actor The owning actor.
---@field enabled boolean Disabled components render nothing, emit nothing and have no collision.
---@field color fox.Vec3 point_light: linear RGB. box/sphere: albedo, 0..1 per channel.
---@field visible boolean box/sphere: false keeps the collider but renders nothing.
---@field emissive fox.Vec3 box/sphere: linear HDR glow added to the shading (above 1 blooms).
---@field intensity number point_light: multiplier on color.
---@field radius number point_light: range in m.
---@field velocity fox.Vec3 box/sphere with body = "dynamic": linear velocity of the actor in m/s.
fox.ActorComponent = {}

---particle_emitter: emits a burst of particles next frame.
---@param count integer
---@return nil
function fox.ActorComponent:emit(count) end

---Position, rotation and scale of an entity, relative to its parent.
---Rotation is euler degrees: x = pitch (positive looks down), y = yaw
---(positive turns +Z towards +X), z = roll. Values are copies: assign
---the modified vector back. Moving a light, emitter or primitive entity
---moves what it renders; the scene camera is driven by fox.camera.
---@class fox.Transform
---@field position fox.Vec3 Local position.
---@field rotation fox.Vec3 Local euler angles in degrees.
---@field scale fox.Vec3 Local scale.
---@field world_position fox.Vec3 World-space position.
---@field forward fox.Vec3 World-space +Z axis.
---@field right fox.Vec3 World-space +X axis.
---@field up fox.Vec3 World-space +Y axis.
---@field entity fox.Entity The owning entity.
fox.Transform = {}

---Moves by delta in parent space.
---@param delta fox.Vec3
---@return nil
function fox.Transform:translate(delta) end

---Adds euler degrees to the rotation.
---@param degrees fox.Vec3
---@return nil
function fox.Transform:rotate(degrees) end

---Turns +Z towards a world-space point (roll 0).
---@param target fox.Vec3
---@return nil
function fox.Transform:look_at(target) end

---Argument of on_scene_loaded.
---@class fox.SceneInfo
---@field name string Scene name from the level file.
---@field path string Project-relative scene path.

---The loaded scene. Loading is asynchronous: load() starts it, scripts keep
---running until the new scene is ready, then entity scripts of the old scene
---get on_destroy, the new scene's scripts are created, and autoloads receive
---on_scene_loaded. Scene loads reset per-scene settings (sun, fog, bloom,
---exposure), so re-apply player preferences in on_scene_loaded.
---@class fox.scene
fox.scene = {}

---Name of the loaded scene.
---@return string
function fox.scene.name() end

---Project-relative path of the loaded scene.
---@return string
function fox.scene.path() end

---Scenes of the project: project.json "scenes", or every scene in Assets/Scenes.
---@return string[]
function fox.scene.list() end

---Starts loading a scene ("Assets/Scenes/sponza.json").
---@param path string
---@return nil
function fox.scene.load(path) end

---Reloads the current scene from its start state.
---@return nil
function fox.scene.reload() end

---True while a scene load is in progress.
---@return boolean
function fox.scene.is_loading() end

---First entity with this name, in scene order.
---@param name string
---@return fox.Entity?
function fox.scene.find(name) end

---Every entity with this name (all entities without one).
---@param name? string
---@return fox.Entity[]
function fox.scene.find_all(name) end

---The entity with this id in the scene file.
---@param source_id integer
---@return fox.Entity?
function fox.scene.get(source_id) end

---Options of fox.world.spawn.
---@class fox.SpawnOptions
---@field position fox.Vec3? World position (default origin).
---@field rotation fox.Vec3? Euler degrees.
---@field scale fox.Vec3?
---@field name string? Entity name (default: the class file name).
---@field properties table? Property overrides, like a level placement's.
---@field components table<string, table>? Component overrides keyed by component name: { mesh = { mesh = "Assets/Props/Crate2.fbx" } }.

---Actors of the loaded scene: instances of fox.actor classes, placed in the
---level or spawned at runtime. Spawned actors are not saved and end with the scene.
---@class fox.world
fox.world = {}

---Creates an actor from its class (or script path) and returns its instance;
---on_start runs next frame. Meshes the level did not load are imported now
---(a hitch): place one instance in the level to preload a class's assets.
---@param class fox.Actor|string
---@param options? fox.SpawnOptions
---@return fox.Actor
function fox.world.spawn(class, options) end

---Destroys an actor at the end of this frame's update (its scripts get
---on_disable/on_destroy then). Same as actor:destroy().
---@param actor fox.Actor|fox.Entity
---@return nil
function fox.world.destroy(actor) end

---Every actor in creation order, or those of a class (or its script path) and its subclasses.
---@param class? fox.Actor|string
---@return fox.Actor[]
function fox.world.actors(class) end

---The first actor whose entity has this name.
---@param name string
---@return fox.Actor?
function fox.world.find(name) end

---The scene camera that renders the game view. Yaw turns clockwise from +Z
---seen from above; pitch is degrees above the horizon (clamped to +-89.9).
---@class fox.camera
---@field position fox.Vec3 Eye position; moving keeps the look direction.
---@field forward fox.Vec3 Unit look direction.
---@field right fox.Vec3 Unit right direction (horizontal).
---@field yaw number Degrees.
---@field pitch number Degrees above the horizon.
---@field fov number Vertical field of view in degrees (1..170).
---@field near number Near clip distance.
---@field far number Far clip distance.
fox.camera = {}

---Sets the look direction in degrees.
---@param yaw number
---@param pitch number
---@return nil
function fox.camera.set_rotation(yaw, pitch) end

---Turns the camera towards a world-space point.
---@param target fox.Vec3
---@return nil
function fox.camera.look_at(target) end

---World-space ray (origin, unit direction) through a game-view pixel, for picking.
---@param x number
---@param y number
---@return fox.Vec3
---@return fox.Vec3
function fox.camera.screen_ray(x, y) end

---Game-view pixel of a world-space point, and whether it is on screen.
---@param point fox.Vec3
---@return number
---@return number
---@return boolean
function fox.camera.world_to_screen(point) end

---The scene's walking capsule (feet at position): gravity, stair stepping and
---collision against every body; pushes dynamic bodies. Call move once per frame.
---@class fox.character
---@field position fox.Vec3 Feet position; assigning teleports.
---@field eye_position fox.Vec3 position + eye_height.
---@field grounded boolean Touching ground after the last move.
---@field vertical_velocity number Upward speed in m/s.
---@field move_speed number Suggested walking speed in m/s (scripts scale input by it).
---@field jump_speed number Take-off speed of jump() in m/s.
---@field eye_height number Eye height above the feet in m.
---@field gravity boolean Whether gravity pulls the character down.
---@field radius number Capsule radius in m.
---@field height number Capsule height in m, including the end caps.
---@field spawn_point fox.Vec3 The scene's character spawn position.
fox.character = {}

---Moves for dt seconds (default fox.time.dt) at the horizontal velocity in m/s;
---velocity.y is ignored (gravity and jumps drive vertical motion).
---@param velocity fox.Vec3
---@param dt? number
---@return nil
function fox.character.move(velocity, dt) end

---Jumps off the ground; false (no jump) while airborne.
---@return boolean
function fox.character.jump() end

---Result of fox.physics.raycast.
---@class fox.RaycastHit
---@field position fox.Vec3 World-space hit point.
---@field normal fox.Vec3 Surface normal at the hit.
---@field distance number Distance from the ray origin.
---@field dynamic boolean True for simulated bodies.
---@field entity fox.Entity? The hit entity, for simulated bodies.

---Rigid-body world (Jolt). Simulation runs at a fixed rate; scripts get on_fixed_update per step.
---@class fox.physics
---@field gravity fox.Vec3 Gravity acceleration in m/s^2.
---@field fixed_dt number Seconds per physics step (the dt of on_fixed_update).
fox.physics = {}

---Closest hit along the ray (ignores the character), or nil.
---@param origin fox.Vec3
---@param direction fox.Vec3
---@param max_distance? number
---@return fox.RaycastHit?
function fox.physics.raycast(origin, direction, max_distance) end

---Spawns a simulated sphere (default radius 0.5 m) that lives until remove_balls
---or the next scene load. Returns nil when the physics world is full.
---@param position fox.Vec3
---@param radius? number
---@param velocity? fox.Vec3
---@return fox.Entity?
function fox.physics.spawn_ball(position, radius, velocity) end

---Removes every ball spawned with spawn_ball.
---@return nil
function fox.physics.remove_balls() end

---Anti-aliasing / upscaling path (mutually exclusive).
---@alias fox.AAMode
---| "off"
---| "fxaa"
---| "taa"
---| "fsr_native" # FSR 3.1 native-resolution AA
---| "fsr_quality"
---| "fsr_balanced"
---| "fsr_performance"

---Rendering settings of the scene runtime (readable and writable).
---@class fox.graphics
---@field aa_mode fox.AAMode Anti-aliasing / upscaling mode.
---@field fsr_sharpening boolean FSR sharpening pass.
---@field fsr_sharpness number FSR sharpening strength (0..1).
---@field bloom boolean Bloom (reset by scene loads).
---@field ambient_occlusion boolean Screen-space ambient occlusion.
---@field volumetric_fog boolean Volumetric fog and light shafts (reset by scene loads).
---@field fog_density number Volumetric fog density (reset by scene loads).
---@field exposure number Tone-mapping exposure multiplier (reset by scene loads).
---@field saturation number Colour saturation (1 = neutral; reset by scene loads).
---@field vignette number Vignette strength (0..1; reset by scene loads).
fox.graphics = {}

---Sun and ambient light of the scene (reset by scene loads).
---@class fox.lighting
---@field sun_enabled boolean
---@field sun_elevation number Degrees above the horizon.
---@field sun_azimuth number Compass direction in degrees.
---@field sun_intensity number Multiplier on sun_color.
---@field sun_color fox.Vec3 Linear RGB.
---@field ambient_intensity number Image-based ambient light multiplier.
fox.lighting = {}

---Options for fox.ui.begin_window.
---@class fox.WindowOptions
---@field x number? Left edge relative to the game view (fixed every frame).
---@field y number? Top edge relative to the game view.
---@field width number? Fixed width (0 or nil: automatic).
---@field height number? Fixed height (0 or nil: automatic).
---@field center boolean? Centre the window in the game view.
---@field no_decoration boolean? No title bar, resize grip or scrollbars; not movable.
---@field no_background boolean? Transparent window background.
---@field auto_resize boolean? Resize to fit the contents every frame.
---@field no_inputs boolean? Click-through window (overlays).

---Immediate-mode UI (Dear ImGui), valid only inside on_gui. Positions are
---pixels relative to the game view. Widgets return their new value first:
---`self.volume = fox.ui.slider_float("Volume", self.volume, 0, 1)`.
---Colours are integers from fox.ui.rgba. Windows are never docked or saved;
---a script error or missing end_window is recovered and logged.
---@class fox.ui
fox.ui = {}

---Begins a top-level window; always pair with end_window, even when this
---returns false (collapsed). The id is also the title of decorated windows.
---@param id string
---@param options? fox.WindowOptions
---@return boolean
function fox.ui.begin_window(id, options) end

---Ends the window started by begin_window.
---@return nil
function fox.ui.end_window() end

---A line of text.
---@param text string
---@return nil
function fox.ui.text(text) end

---Text in a colour from fox.ui.rgba.
---@param color integer
---@param text string
---@return nil
function fox.ui.text_colored(color, text) end

---A button; true on the frame it is clicked. width -1 fills the line.
---@param label string
---@param width? number
---@param height? number
---@return boolean
function fox.ui.button(label, width, height) end

---Returns the new value and whether it changed this frame.
---@param label string
---@param value boolean
---@return boolean
---@return boolean
function fox.ui.checkbox(label, value) end

---Returns the new value and whether it changed. format is printf-style ("%.2f").
---@param label string
---@param value number
---@param min number
---@param max number
---@param format? string
---@return number
---@return boolean
function fox.ui.slider_float(label, value, min, max, format) end

---Returns the new value and whether it changed.
---@param label string
---@param value integer
---@param min integer
---@param max integer
---@return integer
---@return boolean
function fox.ui.slider_int(label, value, min, max) end

---Drop-down over items; index is 1-based. Returns the new index and whether it changed.
---@param label string
---@param index integer
---@param items string[]
---@return integer
---@return boolean
function fox.ui.combo(label, index, items) end

---A selectable row; true on the frame it is clicked.
---@param label string
---@param selected? boolean
---@return boolean
function fox.ui.selectable(label, selected) end

---Single-line text field (up to 1023 bytes). Returns the text and whether it changed.
---@param label string
---@param text string
---@return string
---@return boolean
function fox.ui.input_text(label, text) end

---A horizontal line.
---@return nil
function fox.ui.separator() end

---A horizontal line with a caption.
---@param text string
---@return nil
function fox.ui.separator_text(text) end

---Vertical spacing.
---@return nil
function fox.ui.spacing() end

---Places the next widget on the same line.
---@param offset? number
---@param spacing? number
---@return nil
function fox.ui.same_line(offset, spacing) end

---Remaining content width of the current window.
---@return number
function fox.ui.content_width() end

---Width of text in the current font.
---@param text string
---@return number
function fox.ui.calc_text_width(text) end

---Moves the layout cursor within the current window.
---@param x number
---@return nil
function fox.ui.set_cursor_x(x) end

---Draws text on the overlay behind script windows (default white).
---@param x number
---@param y number
---@param text string
---@param color? integer
---@return nil
function fox.ui.draw_text(x, y, text, color) end

---Draws a rectangle on the overlay.
---@param x number
---@param y number
---@param width number
---@param height number
---@param color integer
---@param filled? boolean
---@param rounding? number
---@return nil
function fox.ui.draw_rect(x, y, width, height, color, filled, rounding) end

---Draws a line on the overlay.
---@param x1 number
---@param y1 number
---@param x2 number
---@param y2 number
---@param color integer
---@param thickness? number
---@return nil
function fox.ui.draw_line(x1, y1, x2, y2, color, thickness) end

---Draws a circle on the overlay.
---@param x number
---@param y number
---@param radius number
---@param color integer
---@param filled? boolean
---@return nil
function fox.ui.draw_circle(x, y, radius, color, filled) end

---Darkens the whole game view (0..1), e.g. behind a pause menu.
---@param alpha number
---@return nil
function fox.ui.dim(alpha) end

---Packs a colour from 0..1 components (alpha defaults to 1).
---@param r number
---@param g number
---@param b number
---@param a? number
---@return integer
function fox.ui.rgba(r, g, b, a) end

---Width and height of the game view.
---@return number
---@return number
function fox.ui.screen_size() end

---True while a script window is hovered or in use (skip mouse-look clicks).
---@return boolean
function fox.ui.wants_mouse() end

---True while a script text field has keyboard focus.
---@return boolean
function fox.ui.wants_keyboard() end

