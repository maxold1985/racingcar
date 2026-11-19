-- Racing game script executed by the graphical host (Luau VM).
-- All positions and velocities are native Luau vectors.
local car = Physics.newVehicle({
    mass = 1200,
    inertia = vector.create(2500, 2100, 600),
    position = vector.create(0, 1.0, 0),
    engineForce = 6500,
    brakeForce = 9500,
    maxSteer = math.rad(18),
    -- Anti-roll bar stiffness in N/m: front and rear axle independently.
    -- The physics core transfers suspension force between left/right wheels.
    antirollFront = 9000,
    antirollRear = 7000,
})

local function addWheel(x, z, front)
    Physics.addWheel(car, {
        mount = vector.create(x, -0.2, z),
        radius = 0.34,
        restLength = 0.55,
        minLength = 0.15,
        maxLength = 0.82,
        spring = 27000,
        bumpDamping = 7000,
        reboundDamping = 8500,
        friction = 1.15,
        lateralStiffness = 6500,
        steering = front,
        driven = not front,
        axle = if front then 0 else 1,
    })
end
addWheel(-0.8,  1.3, true)
addWheel( 0.8,  1.3, true)
addWheel(-0.8, -1.3, false)
addWheel( 0.8, -1.3, false)

-- The host reads this ID to render the chassis and four wheels.
vehicleId = car

-- This table is updated in-place by the C++ host.
-- Capturing Input once keeps runtime reads dynamic and avoids stale imports.
local input = Input

local smoothSteer = 0

function fixedUpdate(dt)
    local throttle = input.throttle
    local brake = input.brake
    local steer = input.steer
    -- Rate-limit steering to avoid instantaneous lateral tire impulses.
    local maxChange = 3 * dt
    smoothSteer += math.clamp(steer - smoothSteer, -maxChange, maxChange)
    Physics.setControls(car, throttle, brake, smoothSteer)
    Physics.step(car, dt)
end
