-- Example S.A.M behavior script (v0.3.0).
--
-- Place a .lua file next to a class JSON with the SAME base name and S.A.M loads
-- it automatically:  classes/assassin.json  +  classes/assassin.lua
--
-- Define on_event(event); S.A.M calls it with a copied, primitive-only event
-- table (never engine pointers). This one gives the Assassin a free Iron Dagger
-- every time they level up.

function on_event(event)
	if event.name == "player.on_level_up" then
		sam_log("Assassin leveled up to " .. tostring(event.amount))
		sam_grant_item(event.player, "IRON_DAGGER")
	end
end
