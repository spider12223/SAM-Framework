-- Berserker — the closer to death, the deadlier. No armor, pure aggression.
--   below 50% HP  -> battle-frenzy (haste)
--   below 25% HP  -> RAGE: +3 STR (live) and last-stand toughness (halve damage)
--   after a kill  -> adrenaline rush: ~0.8s of damage immunity
--
-- Uses sam_set_stat("STR",...) for the live buff — sam_patch_class only edits the
-- class DEFINITION at character creation and would NOT touch a running player.
-- "Invincibility" is emulated in on_before_damage (no EFF_INVINCIBLE exists).

local CLASS = "vanillaplus:berserker"

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

-- One copy of this script serves the whole party, so anything that describes ONE player has
-- to be keyed by their number. As a plain file-local, `adrenaline` meant that any kill by
-- anybody, including one monster killing another, handed the entire party damage immunity.
local raging = {}
local base_str = {}
local adrenaline = {}

function on_tick(event)
  for p = 0, 3 do
    if (adrenaline[p] or 0) > 0 then
      adrenaline[p] = adrenaline[p] - (event.delta_ticks or 1)
    end
  end

  -- Re-evaluate HP state ~once per second (50 ticks) to stay cheap. The class check lives
  -- below this guard so it costs four calls a second, not four a frame.
  if (event.tick_count or 0) % 50 ~= 0 then return end

  for p = 0, 3 do
    if mine(p) then
      local hp = sam_get_stat(p, "HP")
      local maxhp = sam_get_stat(p, "MAXHP")
      if hp and maxhp and maxhp > 0 then
        local pct = hp / maxhp

        if pct < 0.25 then
          if not raging[p] then
            base_str[p] = sam_get_stat(p, "STR")
            if base_str[p] then sam_set_stat(p, "STR", base_str[p] + 3) end
            raging[p] = true
            sam_message(p, "RAGE! Blood pounds — your blows land like thunder.")
          end
          sam_apply_effect(p, "FAST", 120)
        else
          if raging[p] then
            if base_str[p] then sam_set_stat(p, "STR", base_str[p]) end
            raging[p] = false
            sam_message(p, "The red haze fades.")
          end
          if pct < 0.50 then
            sam_apply_effect(p, "FAST", 120)   -- battle-frenzy haste
          end
        end
      end
    end
  end
end

function on_event(event)
  if event.name == "on_monster_died" then
    local p = killer_player(event)
    if not mine(p) then return end
    adrenaline[p] = 40   -- ~0.8s of damage immunity after a kill

  elseif event.name == "on_before_damage" then
    local p = event.player
    if not mine(p) then return end
    if (adrenaline[p] or 0) > 0 then
      sam_modify_damage(p, 0)                                     -- adrenaline: shrug it off
    else
      local hp = sam_get_stat(p, "HP")
      local maxhp = sam_get_stat(p, "MAXHP")
      if hp and maxhp and event.damage and hp <= maxhp * 0.25 then
        sam_modify_damage(p, math.floor(event.damage * 0.5))      -- last-stand toughness
      end
    end
  end
end
