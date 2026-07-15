-- Necromancer — death feeds the caster, and the reaper can be cheated once per floor.
--
-- The "commander of the dead" fantasy is the real SPELL_SUMMON this class starts with
-- (an actual controllable follower). We deliberately do NOT Lua-spawn a skeleton on
-- kill: sam_spawn_monsters creates a HOSTILE monster with no allegiance, so it would
-- just attack you. Instead, kills harvest souls (mana), and the grave gives you back
-- once per floor.

local HOST = 0

function on_event(event)
  if event.name == "on_monster_died" then
    -- Soul harvest: each slain foe restores a little mana.
    if event.killer_uid and event.killer_uid > 0 then
      local mp = sam_get_stat(HOST, "MP")
      local maxmp = sam_get_stat(HOST, "MAXMP")
      if mp and maxmp then sam_set_stat(HOST, "MP", math.min(maxmp, mp + 3)) end
    end

  elseif event.name == "on_before_damage" then
    -- Defy death: the first would-be-fatal blow each floor is negated, leaving 5 HP.
    -- (There is no invincibility effect exposed to Lua, so we cancel the hit itself.)
    local hp = sam_get_stat(event.player, "HP")
    local used = sam_load_data("defied") or 0
    if hp and event.damage and event.damage >= hp and used == 0 then
      sam_modify_damage(event.player, 0)
      sam_set_stat(event.player, "HP", 5)
      sam_save_data("defied", 1)
      sam_message(event.player, "You refuse to die — the grave gives you back, this once.")
    end

  elseif event.name == "player.on_floor_change" then
    sam_save_data("defied", 0)   -- recharge the once-per-floor cheat-death
  end
end
