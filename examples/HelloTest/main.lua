-- Hello Test: the smallest mod `tools/run_mod_test.sh` can run, and a template for your own.
--
--   tools/run_mod_test.sh HelloTest        ->  PASS, exit code 0
--
-- A test mod does three things: it drives itself (nobody is at the keyboard), it logs every
-- check, and it ends with sam_test_done(pass, fail). Outside a -samtest run sam_test_done does
-- nothing, so this mod is also harmless in a normal game: start one and read sam_log.txt.
--
-- Check what would be DIFFERENT if the code were broken, not merely that a value has the right
-- type. "The speed is still 2 after a refused call" can fail; "the speed is a number" cannot.

local pass, fail = 0, 0

local function check(name, ok, saw)
	if ok then pass = pass + 1 else fail = fail + 1 end
	sam_log((ok and "PASS  " or "FAIL  ") .. name .. "   (" .. tostring(saw) .. ")")
end

-- Register at the top of the script, like any mod: the row exists before a game starts.
sam_register_setting("greeting", { type = "toggle", label = "Hello Test: say hello", default = true })

local function runChecks()
	-- Game speed: a set is kept, a refused set changes nothing, and 1 puts it back.
	check("the game starts at normal speed", sam_get_game_speed() == 1, sam_get_game_speed())
	check("double speed is accepted", sam_set_game_speed(2) == true, "set 2")
	check("and is what the game now reports", sam_get_game_speed() == 2, sam_get_game_speed())
	check("a speed of 0 is refused", sam_set_game_speed(0) == false, "set 0")
	check("and the refusal left the speed alone", sam_get_game_speed() == 2, sam_get_game_speed())
	sam_set_game_speed(1)
	check("1 puts it back", sam_get_game_speed() == 1, sam_get_game_speed())

	-- The loot pool: a roll comes out of the pool the engine says it would draw from.
	local pool = sam_get_loot_pool("WEAPON", 0, 5) or {}
	local inPool = {}
	for _, entry in ipairs(pool) do inPool[entry.type] = true end
	check("floors 0 to 5 have weapons to roll", #pool > 0, #pool .. " candidates")
	local strays = 0
	for _ = 1, 20 do
		local rolled = sam_roll_loot("WEAPON", 0, 5)
		if not inPool[rolled] then strays = strays + 1 end
	end
	check("20 rolls all land inside that pool", strays == 0, strays .. " outside it")

	-- A setting: a script change is kept, and so is the player's value afterwards.
	local before = sam_get_setting("greeting")
	check("the setting has a value", before ~= nil, before)
	check("a script can change it", sam_set_setting("greeting", not before) == true, "set " .. tostring(not before))
	check("and the change is what it reads back", sam_get_setting("greeting") == (not before), sam_get_setting("greeting"))
	sam_set_setting("greeting", before)   -- leave the player's value as we found it

	sam_log(string.format("HELLO RESULT: %d passed, %d FAILED", pass, fail))
	sam_test_done(pass, fail)
end

-- game.on_game_start fires as the first floor is built; give the world a second (50 game ticks)
-- to settle before checking anything.
local ticksLeft = nil

function on_event(e)
	if e.name == "game.on_game_start" and e.player == 0 then ticksLeft = 50 end
end

function on_tick(e)
	if ticksLeft == nil then return end
	ticksLeft = ticksLeft - 1
	if ticksLeft == 0 then
		ticksLeft = nil
		runChecks()
	end
end
