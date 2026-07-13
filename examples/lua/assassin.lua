-- Example S.A.M behavior script — Lua.
--
-- Place a .lua file next to a class JSON with the SAME base name and S.A.M loads
-- it automatically:  classes/assassin.json  +  classes/assassin.lua
--
-- Define on_event(event); S.A.M calls it with a copied, primitive-only event
-- table (never engine pointers), host-authoritative only. This example logs every
-- hook and, on level-up, exercises every host API function.

local function on_level_up(event)
	local p = event.player
	sam_log("Assassin leveled up to " .. tostring(event.amount))

	-- Reward + feedback
	sam_message(p, "You reached level " .. tostring(event.amount) .. "!")
	sam_grant_item(p, "IRON_DAGGER")
	sam_grant_gold(p, 25)

	-- Read stats / world
	local hp, maxhp = sam_get_stat(p, "HP"), sam_get_stat(p, "MAXHP")
	sam_log("On floor " .. tostring(sam_get_floor()) .. " with HP " .. tostring(hp) .. "/" .. tostring(maxhp))

	-- Bounded stat write (full heal) + a temporary buff
	sam_set_stat(p, "HP", maxhp)
	sam_apply_effect(p, "LEVITATING", 250) -- 5 seconds (50 ticks/sec)
	sam_apply_effect(p, "FAST", 100)
	sam_remove_effect(p, "FAST")

	-- Sound + a look around
	sam_play_sound(153)
	local nearby = sam_get_nearby_entities(p, 20)
	sam_log("Creatures within 20 tiles: " .. tostring(#nearby))

	-- Drop a gem on the floor near the start of the level
	sam_spawn_item(10, 10, "GEM_RUBY")
end

function on_event(event)
	local n = event.name
	if n == "player.on_level_up" then
		on_level_up(event)
	elseif n == "player.on_hit" then
		sam_log("Hit target " .. tostring(event.target_uid) .. " for " .. tostring(event.damage) .. (event.lethal == 1 and " (kill)" or ""))
	elseif n == "player.on_kill" then
		sam_grant_gold(event.player, 10) -- bounty per kill
	elseif n == "player.on_equip" then
		sam_log("Equipped item " .. tostring(event.item_type) .. " in slot " .. tostring(event.slot))
	elseif n == "player.on_unequip" then
		sam_log("Unequipped item " .. tostring(event.item_type) .. " from slot " .. tostring(event.slot))
	elseif n == "player.on_item_use" then
		sam_log("Used consumable " .. tostring(event.item_type))
	elseif n == "player.on_death" then
		sam_log("Died: " .. tostring(event.obituary))
	elseif n == "player.on_spell_cast" then
		sam_log("Cast spell " .. tostring(event.spell_name))
	elseif n == "player.on_gold_collected" then
		sam_log("Picked up " .. tostring(event.amount) .. " gold (total " .. tostring(event.total_gold) .. ")")
	elseif n == "player.on_damage_taken" then
		if event.lethal == 1 then sam_log("Took a lethal hit!") end
	elseif n == "player.on_floor_change" then
		sam_message(event.player, "Descending to floor " .. tostring(event.new_floor))
	end
end
