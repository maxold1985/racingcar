-- Lua/Luau script running inside the native game engine, NOT Roblox Studio.
-- Coordinates: +X right, +Y up, +Z forward; metres, seconds, radians.
local car = Physics.newVehicle({
    mass = 1200,
    inertia = {x = 2500, y = 2100, z = 600},
    position = {x = 0, y = 0.98, z = 0},
    engineForce = 5200,
    brakeForce = 9500,
    maxSteer = math.rad(24),
    antirollFront = 9000,
    antirollRear = 7000,
})

local function wheel(x, z, front)
    return Physics.addWheel(car, {
        mount = {x = x, y = -0.20, z = z},
        radius = 0.34,
        restLength = 0.55,
        minLength = 0.15,
        maxLength = 0.82,
        spring = 27000,
        bumpDamping = 7000,
        reboundDamping = 8500,
        friction = 1.15,
        lateralStiffness = 9000,
        rollingResistance = 5,
        steering = front,
        driven = not front, -- rear wheel drive
        axle = front and 0 or 1,
    })
end

local FL = wheel(-0.80,  1.30, true)
local FR = wheel( 0.80,  1.30, true)
local RL = wheel(-0.80, -1.30, false)
local RR = wheel( 0.80, -1.30, false)
local time = 0
local ticks = 0

function fixedUpdate(dt)
    time += dt
    ticks += 1

    -- Demo: throttle -> slow steering -> brake -> stop.
    local throttle = if time < 4 then 1 else 0
    local steer = if time > 2.0 and time < 3.5 then 0.13 else 0
    local brake = if time >= 4.5 then 1 else 0

    Physics.setControls(car, throttle, brake, steer)
    Physics.step(car, dt)

    if ticks % 120 == 0 then
        local state = Physics.getState(car)
        local fl = Physics.getWheel(car, FL)
        print(string.format(
            "t=%.0fs | speed=%.1f km/h | x=%.2f z=%.2f y=%.2f | FL=%s, force=%.0fN",
            time, state.speedKmh, state.position.x, state.position.z,
            state.position.y, tostring(fl.grounded), fl.force
        ))
    end
end
