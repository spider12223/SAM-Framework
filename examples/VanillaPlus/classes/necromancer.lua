-- Necromancer — death feeds the caster, and the reaper can be cheated once per floor.
--
-- The "commander of the dead" fantasy is the real SPELL_SUMMON this class starts with
-- (an actual controllable follower). We deliberately do NOT Lua-spawn a skeleton on
-- kill: sam_spawn_monster creates a HOSTILE monster with no allegiance, so it would
-- just attack you. Instead, kills harvest souls (mana), and the grave gives you back
-- once per floor.

local CLASS = "vanillaplus:necromancer"

-- Once a script is loaded it receives EVERY event, whatever content it shipped beside:
-- dispatch is global, not scoped to this class. So every handler starts by asking whose
-- event this is and whether that player actually picked this class.
local function mine(player)
  return player ~= nil and player >= 0 and sam_get_class(player) == CLASS
end

-- on_monster_died names the killer by entity uid, not by player number, so match it against
-- each player's own body. nil means a trap, a hazard or another monster did the killing.
local function killer_player(event)
  local uid = event.killer_uid
  if not uid or uid <= 0 then return nil end
  for p = 0, 3 do
    if sam_get_player_uid(p) == uid then return p end
  end
  return nil
end

function on_event(event)
  if event.name == "on_monster_died" then
    -- Soul harvest: each foe YOU slay restores a little mana.
    local p = killer_player(event)
    if not mine(p) then return end
    local mp = sam_get_stat(p, "MP")
    local maxmp = sam_get_stat(p, "MAXMP")
    if mp and maxmp then sam_set_stat(p, "MP", math.min(maxmp, mp + 3)) end

  elseif event.name == "on_before_damage" then
    -- Defy death: the first would-be-fatal blow each floor is negated, leaving 5 HP.
    -- (There is no invincibility effect exposed to Lua, so we cancel the hit itself.)
    if not mine(event.player) then return end
    local hp = sam_get_stat(event.player, "HP")
    -- Per player and per run. sam_save_data would write one file for the whole mod, so in
    -- co-op two Necromancers would share the charge and it would survive into the next run.
    local used = sam_get_player_data(event.player, "defied") or 0
    if hp and event.damage and event.damage >= hp and used == 0 then
      sam_modify_damage(event.player, 0)
      sam_set_stat(event.player, "HP", 5)
      sam_set_player_data(event.player, "defied", 1)
      sam_message(event.player, "You refuse to die — the grave gives you back, this once.")
    end

  elseif event.name == "player.on_floor_change" then
    if not mine(event.player) then return end
    sam_set_player_data(event.player, "defied", 0)   -- recharge the once-per-floor cheat-death
  end
end
