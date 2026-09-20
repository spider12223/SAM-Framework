-- Ranger — the hunt rewards momentum.
--
-- Kill streak tracked per player; every 5 kills grants a fresh quiver. The streak resets
-- each floor (announced) with a resupply.
--
-- NOTE: bow/thrown kills do NOT fire player.on_kill (that hook is melee-only), so
-- we count via on_monster_died.

local CLASS = "vanillaplus:ranger"

-- Once a script is loaded it receives EVERY event, whatever content it shipped beside:
-- dispatch is global, not scoped to this class. So every handler starts by asking whose
-- event this is and whether that player actually picked this class. Without this line a
-- Paladin would be handed the Ranger's quivers too.
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
    local p = killer_player(event)
    if not mine(p) then return end
    -- Per player and per run: sam_set_player_data is in memory and keyed by player, where
    -- sam_save_data writes one file for the whole mod and every Ranger would share a streak.
    local streak = (sam_get_player_data(p, "streak") or 0) + 1
    sam_set_player_data(p, "streak", streak)
    if streak % 5 == 0 then
      sam_grant_item(p, "QUIVER_HUNTING")
      sam_message(p, "Hunter's momentum (" .. streak .. " kills) — a fresh quiver!")
    end

  elseif event.name == "player.on_floor_change" then
    if not mine(event.player) then return end
    sam_set_player_data(event.player, "streak", 0)
    sam_grant_item(event.player, "QUIVER_HUNTING")
    sam_message(event.player, "A new floor — you scavenge arrows. The hunt begins anew.")
  end
end
