-- Game Speed Control: four rebindable preset keys, each with a speed slider, and a corner readout.
--
-- The community mod this reproduces shipped as a patched barony.exe with "/gamespeed 0.1-8" and
-- four keybind slots in the Bindings page. Here it is a folder: sam_register_action puts a row in
-- Settings > Controls > Bindings, sam_register_setting puts a slider in Settings > General under
-- MOD SETTINGS, and sam_set_game_speed changes how many game ticks a real second buys. Everything
-- is registered at the top level so the rows exist before a game starts.
--
-- Speed is singleplayer only. In a netgame the host's world would run at Nx while every joiner's
-- body, moved by their own machine, stayed at 1x, so the framework refuses and says so once.

local SLOTS = {
	{ id = "slot1", label = "Game speed slot 1", key = "Keypad 0", speed = 0.5 },
	{ id = "slot2", label = "Game speed slot 2", key = "Keypad 1", speed = 1 },
	{ id = "slot3", label = "Game speed slot 3", key = "Keypad 2", speed = 2 },
	{ id = "slot4", label = "Game speed slot 4", key = "Keypad 3", speed = 8 },
}

local sliderOf = {}   -- "gamespeed:slot1" -> "slot1_speed"
for _, s in ipairs(SLOTS) do
	sam_register_action(s.id, s.label, s.key)
	sam_register_setting(s.id .. "_speed", {
		type = "slider", label = s.label .. " speed",
		tooltip = "0.1x to 8x. Press the slot's key in game to switch to it.",
		min = 0.1, max = 8, step = 0.1, default = s.speed,
	})
	sliderOf["gamespeed:" .. s.id] = s.id .. "_speed"
end

-- Optional: bullet time on a kill. Off by default; a toggle in the same MOD SETTINGS section.
sam_register_setting("bullet_time", {
	type = "toggle", label = "Bullet time on a kill",
	tooltip = "Quarter speed for half a real second whenever you land a killing blow.",
	default = false,
})

function on_event(e)
	if e.name == "on_action_pressed" and sliderOf[e.action] then
		-- The slider, as the player left it. Setting the speed already set fires nothing, so a
		-- held key costs nothing.
		local mult = sam_get_setting(sliderOf[e.action])
		if not sam_set_game_speed(mult) then
			sam_message(e.player, "Game speed is singleplayer only.")
		end

	elseif e.name == "game.on_speed_changed" then
		-- The value is already stored when this fires, so the readout reads the exact multiplier.
		-- new_percent on the event is the rounded whole number, fine for a log line, not for 0.25x.
		local mult = sam_get_game_speed()
		if mult == 1 then
			sam_hud_clear("gamespeed_readout")
		else
			sam_hud_text("gamespeed_readout", 8, 8, string.format("%.2gx", mult), 0xFFFFFFFF, 0)
		end

	elseif e.name == "player.on_kill" and sam_get_setting("bullet_time") then
		-- A temporary window: after that many GAME ticks at the new speed it goes back by itself.
		-- Half a real second at 0.25x is 0.5 * 50 * 0.25 = 6 ticks. A window opened inside a window
		-- still returns to the original speed, so two quick kills do not strand you at 0.25x.
		sam_set_game_speed(0.25, 6)
	end
end
