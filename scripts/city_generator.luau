-- City road network calibrated against the original Downtown City MegaKit GLBs.
-- Street_2Lane: asphalt 6 x 12 (longitudinal Z).
-- Street_4WayIntersection: asphalt 18 x 18; outer markings ~24.67 x 24.67.
-- Connector centers: intersection at +/-9, straight at +/-6.
-- Thus the first straight center is 15m from intersection center.
local CHUNK_SIZE = 180
local BLOCK_SPACING = 90
local VIEW_RADIUS = 1
local SEED = 24681357
local active = {}
local nextId = 1
local lastX, lastZ = nil, nil
local metrics = CityAssets or {}

local function hash(x,z,salt)
    local n=(x*73856093+z*19349663+salt*83492791+SEED)%2147483647
    return (n*48271+1)%2147483647
end

local function buildingScale(model, maxWidth, maxDepth)
    local m=metrics[model]
    if not m or not m.width or not m.depth or m.width<=0 or m.depth<=0 then
        return 1
    end
    return math.clamp(math.min(maxWidth/m.width,maxDepth/m.depth),0.05,3)
end

local function spawnChunk(cx,cz)
    local key=tostring(cx)..":"..tostring(cz)
    if active[key] then return end
    local id=nextId
    nextId+=1
    active[key]=id
    local ox,oz=cx*CHUNK_SIZE,cz*CHUNK_SIZE

    -- Each 180m chunk owns 2x2 intersections, and road segments leading
    -- toward the next intersections. Adjacent chunks continue seamlessly.
    for ix=0,1 do
        for iz=0,1 do
            local x,z=ox+ix*BLOCK_SPACING,oz+iz*BLOCK_SPACING
            CityWorld.add(id,2,x,0,z,0,1)
            for segment=0,5 do
                local distance=15+12*segment
                -- Original straight GLB longitudinal axis is Z.
                CityWorld.add(id,1,x,0,z+distance,180,1)
                CityWorld.add(id,1,x+distance,0,z,270,1)
            end
        end
    end

    -- Each 90m square has an 80m paved interior, leaving the 18m\n    -- intersection footprint and a 2m setback from each 6m-wide road.\n    for ix=0,1 do\n        for iz=0,1 do\n            CityWorld.add(id,9,ox+ix*BLOCK_SPACING+45,0,\n                oz+iz*BLOCK_SPACING+45,0,1,80,80)\n        end\n    end\n\n    -- One building per interior lot. No buildings inside road corridors.
    for ix=0,1 do
        for iz=0,1 do
            local bx,bz=ox+ix*BLOCK_SPACING,oz+iz*BLOCK_SPACING
            for lx=0,1 do
                for lz=0,1 do
                    local x=bx+27+lx*36
                    local z=bz+27+lz*36
                    if math.abs(x)>56 or math.abs(z)>56 then
                        local n=hash(math.floor(x),math.floor(z),1)
                        if n%4~=0 then
                            local model=4+n%3
                            CityWorld.add(id,model,x,0,z,(n%4)*90,
                                buildingScale(model,27,27))
                        end
                    end
                end
            end
        end
    end
end

function cityUpdate(x,z)
    local cx=math.floor(x/CHUNK_SIZE)
    local cz=math.floor(z/CHUNK_SIZE)
    if cx==lastX and cz==lastZ then return end
    lastX,lastZ=cx,cz
    for key,id in active do
        local sx,sz=string.match(key,"^(-?%d+):(-?%d+)$")
        if math.abs(tonumber(sx)-cx)>VIEW_RADIUS or math.abs(tonumber(sz)-cz)>VIEW_RADIUS then
            CityWorld.remove(id)
            active[key]=nil
        end
    end
    for dx=-VIEW_RADIUS,VIEW_RADIUS do
        for dz=-VIEW_RADIUS,VIEW_RADIUS do
            spawnChunk(cx+dx,cz+dz)
        end
    end
end
