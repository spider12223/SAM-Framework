-- Boss Example: the fight logic.
--
-- The JSON gives you a custom-bodied creature with a boss bar and boss music. This file is
-- what makes it feel like a fight: phases, a telegraphed attack, adds, and a death drop.
--
-- The one thing to understand before you change anything: the boss MOVES and ATTACKS like
-- its base_type (a rat). You cannot replace that. What you can do is add set-pieces on top,
-- and that is what players actually read as "boss AI".

local BOSS_NAME  = "Ashen Wyvern"
local BREATH     = "bossexample:searing_breath"
local SCALE      = "bossexample:wyvern_scale"
local BLADE      = "bossexample:wyvern_blade"

-- Live fight state. Keyed by the boss's uid so two bosses never share a phase.
local fights = {}


-- =====================================================================================
-- Finding the boss
-- =====================================================================================
-- world.on_monster_spawned fires for EVERY monster, so filter by trait, not by type:
-- the wyvern is a rat as far as monster_type is concerned.

local function on_spawned(e)
  if not sam_monster_has_trait(e.monster_uid, "boss") then return end
  fights[e.monster_uid] = { phase = 1, breath_cooldown = 0 }
  sam_message(0, "The " .. BOSS_NAME .. " wakes.")
  sam_camera_shake(0, 12)
end


-- =====================================================================================
-- Phases: the part players read as "it got angry"
-- =====================================================================================
-- Nothing here is special engine support. It is just: watch the health, change what the
-- script does. Two phase changes is plenty for a fight to feel like it has structure.

local function check_phase(uid, f)
  local hp  = sam_get_monster_stat(uid, "HP")
  local max = sam_get_monster_stat(uid, "MAXHP")
  if not hp or not max or max <= 0 then return end
  local pct = (hp / max) * 100

  if f.phase == 1 and pct <= 66 then
    f.phase = 2
    sam_message(0, "The " .. BOSS_NAME .. " screams and takes to the air.")
    sam_screen_flash(0, 255, 120, 0)
    sam_camera_shake(0, 20)
    -- "Flight" in Barony is a hover, not a swoop: levitation plus a raised position reads
    -- as airborne. A real dive is not expressible (see the README).
    sam_apply_monster_effect(uid, "LEVITATING", -1)

  elseif f.phase == 2 and pct <= 33 then
    f.phase = 3
    sam_message(0, "The " .. BOSS_NAME .. " is enraged!")
    sam_screen_flash(0, 255, 40, 0)
    sam_camera_shake(0, 28)
    -- DEX is the movement-speed dial for a monster. Phase 3 = it comes at you faster.
    local dex = sam_get_monster_stat(uid, "DEX") or 6
    sam_set_monster_stat(uid, "DEX", dex + 8)
    -- And it brings friends.
    local x, y = sam_get_position(uid)
    if x then
      for _ = 1, 3 do sam_spawn_monster(x, y, "RAT") end
    end
  end
end


-- =====================================================================================
-- The breath attack: telegraph, then fire
-- =====================================================================================
-- The telegraph is the whole trick. An attack that just happens feels random; an attack
-- with half a second of warning feels designed. Players will call this "the boss AI".

local function try_breath(uid, f)
  if f.breath_cooldown > 0 then
    f.breath_cooldown = f.breath_cooldown - 1
    return
  end
  local target = sam_get_monster_target(uid)
  if not target then return end

  -- Telegraph: a roar and a shake, then the actual breath a beat later.
  sam_play_sound(173)
  sam_camera_shake(0, 10)
  sam_set_timer(function()
    -- Aimed at whoever it was hunting when the wind-up started.
    sam_monster_cast_spell(uid, BREATH)
  end, 30)  -- ~half a second at 60 ticks

  -- Faster in later phases: same attack, more pressure.
  f.breath_cooldown = (f.phase >= 3) and 90 or (f.phase == 2 and 150 or 240)
end


-- =====================================================================================
-- Death: the drop
-- =====================================================================================

local function on_died(e)
  local f = fights[e.monster_uid]
  if not f then return end
  fights[e.monster_uid] = nil

  sam_message(0, "The " .. BOSS_NAME .. " falls.")
  sam_screen_flash(0, 255, 255, 255)
  sam_camera_shake(0, 24)

  local x, y = sam_get_position(e.monster_uid)
  if not x then return end
  for _ = 1, 5 do sam_spawn_item(x, y, SCALE) end
  sam_spawn_item(x, y, BLADE)
end


-- =====================================================================================
-- Dispatch
-- =====================================================================================

function on_event(e)
  if e.name == "world.on_monster_spawned" then return on_spawned(e)
  elseif e.name == "on_monster_died"      then return on_died(e)
  end
end

-- on_tick drives the phases and the attack timing. Keep it cheap: this runs every frame,
-- and the loop below only touches bosses that are actually in a fight.
function on_tick(e)
  for uid, f in pairs(fights) do
    check_phase(uid, f)
    try_breath(uid, f)
  end
end

sam_log("Boss Example loaded.")
