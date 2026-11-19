-- Procedural city for RacingCar (standalone Luau, no Roblox APIs).
-- The C++ host exposes CityWorld.add(chunkId, modelId, x, y, z, yawDegrees)
-- and CityWorld.remove(chunkId). Model IDs correspond to cityNames in main.cpp.
local CHUNK_SIZE = 192
local VIEW_RADIUS = 1
local SEED = 24681357
local active = {}
local nextId = 1
local lastX, lastZ = nil, nil

local function hash(x, z, salt)
    local n = (x * 73856093 + z * 19349663 + salt * 83492791 + SEED) % 2147483647
    n = (n * 48271 + 1) % 2147483647
    return n
end

local function spawnChunk(cx, cz)
    local key = tostring(cx) .. ":" .. tostring(cz)
    if active[key] then return end
    local id = nextId
    nextId += 1
    active[key] = id
    local ox, oz = cx * CHUNK_SIZE, cz * CHUNK_SIZE
    -- Road corridors share the same world grid on adjacent chunks.
    -- The imported meshes need their real dimensions/pivots verified.
    -- 16m grid is an initial placement, not guaranteed seamless FBX tiling.
    for ix = 0, 11 do
        for iz = 0, 11 do
            local x, z = ox + ix * 16, oz + iz * 16
            local roadX = ix == 0 or ix == 6
            local roadZ = iz == 0 or iz == 6
            if roadX and roadZ then
                CityWorld.add(id, 2, x, 0, z, 0)
            elseif roadX or roadZ then
                CityWorld.add(id, 1, x, 0, z, if roadZ then 90 else 0)
            else
                -- Leave the original garage/start area clear.
                if math.abs(x) > 48 or math.abs(z) > 48 then
                    local n = hash(cx * 12 + ix, cz * 12 + iz, 1)
                    if n % 5 == 0 then
                        local building = 4 + (n % 3)
                        CityWorld.add(id, building, x, 0, z, (n % 4) * 90)
                    end
                end
            end
        end
    end
end

function cityUpdate(x, z)
    local cx = math.floor(x / CHUNK_SIZE)
    local cz = math.floor(z / CHUNK_SIZE)
    if cx == lastX and cz == lastZ then return end
    lastX, lastZ = cx, cz
    for dx = -VIEW_RADIUS, VIEW_RADIUS do
        for dz = -VIEW_RADIUS, VIEW_RADIUS do
            spawnChunk(cx + dx, cz + dz)
        end
    end
    for key, id in active do
        local sx, sz = string.match(key, "^(-?%d+):(-?%d+)$")
        local bx, bz = tonumber(sx), tonumber(sz)
        if math.abs(bx - cx) > VIEW_RADIUS or math.abs(bz - cz) > VIEW_RADIUS then
            CityWorld.remove(id)
            active[key] = nil
        end
    end
end
