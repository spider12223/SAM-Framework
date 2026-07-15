-- Ranger — the hunt rewards momentum.
--
-- Kill streak tracked with sam_save_data; every 5 kills grants a fresh quiver.
-- The streak resets each floor (announced) with a resupply.
--
-- NOTE: bow/thrown kills do NOT fire player.on_kill (that hook is melee-only), so
-- we count via on_monster_died. In single-player the local host (player 0) is you.

local HOST = 0

function on_event(event)
  if event.name == "on_monster_died" then
    -- Only count kills that actually had a killer (skip traps/starvation deaths).
    if event.killer_uid and event.killer_uid > 0 then
      local streak = (sam_load_data("streak") or 0) + 1
      sam_save_data("streak", streak)
      if streak % 5 == 0 then
        sam_grant_item(HOST, "QUIVER_HUNTING")
        sam_message(HOST, "Hunter's momentum (" .. streak .. " kills) — a fresh quiver!")
      end
    end
  elseif event.name == "player.on_floor_change" then
    sam_save_data("streak", 0)
    sam_grant_item(event.player, "QUIVER_HUNTING")
    sam_message(event.player, "A new floor — you scavenge arrows. The hunt begins anew.")
  end
end
