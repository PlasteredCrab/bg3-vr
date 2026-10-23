-- The controlled character, steadied.
--
-- In combat on 2026-10-23, _C() alternated between two party members four times a second. VR start, the follow tick
-- and the head hiding all followed it, so the camera, the skeleton and the hidden head jumped back and forth. Here a
-- change only counts once the new character has held for kSteadyTicks ticks in a row. Every switch, and how often
-- _C() disagreed with the steady one, is logged.

local kSteadyTicks = 45 -- about a second at the game's tick rate

local steady = { character = nil, key = nil, candidate = nil, ticks = 0, disagreements = 0, total = 0 }

-- The core's acting=0x<handle> (the character the hotbar acts for, read natively; agent AF) when it has one: _C()
-- returns the first ClientControl character, which in combat isn't always the one being played. _C() otherwise.
local function Read()
    if BG3VR.ActingHex then
        local ok, character = pcall(function()
            return Ext.Entity.Get(Ext.Utils.IntegerToHandle(math.tointeger(tonumber(BG3VR.ActingHex))))
        end)
        if ok and character then
            return character, tostring(character)
        end
    end
    local ok, character = pcall(_C)
    if not ok then
        return nil, nil
    end
    return character, character and tostring(character) or nil
end

-- The controlled character VR follows (nil when there is none).
function BG3VR.Controlled()
    if not steady.key then
        local character, key = Read()
        steady.character, steady.key = character, key
    end
    return steady.character
end

Ext.Events.Tick:Subscribe(function()
    local character, key = Read()
    steady.total = steady.total + 1
    if key == steady.key then
        steady.candidate, steady.ticks = nil, 0
        if character then
            steady.character = character
        end
        return
    end
    steady.disagreements = steady.disagreements + 1
    if key ~= steady.candidate then
        steady.candidate, steady.ticks = key, 0
    end
    steady.ticks = steady.ticks + 1
    -- The native acting character is the game's own answer: only a few ticks to settle; _C() needs the long wait.
    if steady.ticks >= (BG3VR.ActingHex and 3 or kSteadyTicks) or not steady.key then
        BG3VR.Log("controlled character now %s (was %s; _C() disagreed with the steady one on %d of %d ticks so far)",
            tostring(key), tostring(steady.key), steady.disagreements, steady.total)
        steady.character, steady.key = character, key
        steady.candidate, steady.ticks = nil, 0
    end
end)
