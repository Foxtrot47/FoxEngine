-- Look and movement math shared by the player scripts. Pure functions only
-- (no scene or input access) so Game/CameraMath_test.lua can cover them.
--
-- Conventions match fox.camera: yaw in degrees turns +Z towards +X (clockwise
-- seen from above), pitch is degrees above the horizon.

local CameraMath = {}

-- Look limit of the old camera controller (fox.camera itself allows 89.9).
CameraMath.MAX_PITCH = 89

function CameraMath.clamp_pitch(pitch, limit)
    limit = limit or CameraMath.MAX_PITCH
    if pitch > limit then return limit end
    if pitch < -limit then return -limit end
    return pitch
end

-- Applies a mouse delta in pixels (x right, y down): moving right turns
-- right, moving down looks down. Returns the new yaw and clamped pitch.
function CameraMath.apply_look(yaw, pitch, dx, dy, sensitivity)
    return yaw + dx * sensitivity, CameraMath.clamp_pitch(pitch - dy * sensitivity)
end

-- Unit look direction.
function CameraMath.forward(yaw, pitch)
    local y, p = math.rad(yaw), math.rad(pitch)
    return vec3(math.sin(y) * math.cos(p), math.sin(p), math.cos(y) * math.cos(p))
end

-- Horizontal unit direction of the yaw (walking forward).
function CameraMath.flat_forward(yaw)
    local y = math.rad(yaw)
    return vec3(math.sin(y), 0, math.cos(y))
end

-- Horizontal unit right direction (no roll).
function CameraMath.right(yaw)
    local y = math.rad(yaw)
    return vec3(math.cos(y), 0, -math.sin(y))
end

-- -1, 0 or 1 from two held buttons.
function CameraMath.axis(positive, negative)
    local value = 0
    if positive then value = value + 1 end
    if negative then value = value - 1 end
    return value
end

-- Free-fly displacement for one frame. Forward follows the full look
-- direction; like the old controller, diagonals are not normalised.
function CameraMath.fly_delta(yaw, pitch, forward_axis, right_axis, up_axis, speed, dt)
    local step = speed * dt
    return CameraMath.forward(yaw, pitch) * (forward_axis * step)
        + CameraMath.right(yaw) * (right_axis * step)
        + vec3(0, up_axis * step, 0)
end

-- Horizontal walking velocity in m/s. Diagonals are normalised so they are
-- no faster than straight movement.
function CameraMath.walk_velocity(yaw, forward_axis, right_axis, speed)
    local wish = CameraMath.flat_forward(yaw) * forward_axis + CameraMath.right(yaw) * right_axis
    local length = wish:length()
    if length > 1 then
        wish = wish / length
    end
    return wish * speed
end

return CameraMath
