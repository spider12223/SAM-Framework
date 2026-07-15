-- Berserker — the closer to death, the deadlier. No armor, pure aggression.
--   below 50% HP  -> battle-frenzy (haste)
--   below 25% HP  -> RAGE: +3 STR (live) and last-stand toughness (halve damage)
--   after a kill  -> adrenaline rush: ~0.8s of damage immunity
--
-- Uses sam_set_stat("STR",...) for the live buff — sam_patch_class only edits the
-- class DEFINITION at character creation and would NOT touch a running player.
-- "Invincibility" is emulated in on_before_damage (no EFF_INVINCIBLE exists).

local HOST = 0
local raging = false
local base_str = nil
local adrenaline = 0   -- ticks of post-kill damage immunity remaining

function on_tick(event)
  if adrenaline > 0 then adrenaline = adrenaline - (event.delta_ticks or 1) end

  -- Re-evaluate HP state ~once per second (50 ticks) to stay cheap.
  if (event.tick_count or 0) % 50 ~= 0 then return end

  local hp = sam_get_stat(HOST, "HP")
  local maxhp = sam_get_stat(HOST, "MAXHP")
  if not hp or not maxhp or maxhp <= 0 then return end
  local pct = hp / maxhp

  if pct < 0.25 then
    if not raging then
      base_str = sam_get_stat(HOST, "STR")
      if base_str then sam_set_stat(HOST, "STR", base_str + 3) end
      raging = true
      sam_message(HOST, "RAGE! Blood pounds — your blows land like thunder.")
    end
    sam_apply_effect(HOST, "FAST", 120)
  else
    if raging then
      if base_str then sam_set_stat(HOST, "STR", base_str) end
      raging = false
      sam_message(HOST, "The red haze fades.")
    end
    if pct < 0.50 then
      sam_apply_effect(HOST, "FAST", 120)   -- battle-frenzy haste
    end
  end
end

function on_event(event)
  if event.name == "on_monster_died" then
    if event.killer_uid and event.killer_uid > 0 then
      adrenaline = 40   -- ~0.8s of damage immunity after a kill
    end

  elseif event.name == "on_before_damage" then
    if adrenaline > 0 then
      sam_modify_damage(event.player, 0)                              -- adrenaline: shrug it off
    else
      local hp = sam_get_stat(event.player, "HP")
      local maxhp = sam_get_stat(event.player, "MAXHP")
      if hp and maxhp and event.damage and hp <= maxhp * 0.25 then
        sam_modify_damage(event.player, math.floor(event.damage * 0.5))  -- last-stand toughness
      end
    end
  end
end
