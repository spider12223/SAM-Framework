-- Party Test: party members in a real game, unattended.
--
--   tools/run_mod_test.sh PartyTest        ->  PASS, exit code 0
--
-- What it does, and what would be different if the code were broken:
--   1. reads sam_party_choices and the character-screen rule;
--   2. spawns Alpha (respawns), Bravo (respawn off) and Charlie (a handler keeps it dead);
--   3. reads and changes them, and checks the refusals leave nothing behind;
--   4. kills all three: only Alpha should be waiting to come back;
--   5. takes the stairs, and checks that Alpha, and only Alpha, rejoined beside player 0.
-- Not here: the party carrying the run on after every player falls. That ends the run, so it is
-- played by hand (solo and online) rather than from a test that must finish.
-- The lobby half (cards, links, the 'SAML' exchange) needs a lobby, so it is checked by hand;
-- here only what a script sees in a game is checked: no card can be set, slots read back.

local pass, fail = 0, 0
local function check(name, ok, saw)
	if ok then pass = pass + 1 else fail = fail + 1 end
	sam_log((ok and "PASS  " or "FAIL  ") .. name .. "   (" .. tostring(saw) .. ")")
end

local actionAdded = sam_lobby_add_slot_action("probe", "+ Probe")

local spawned, died = {}, {}
local alpha, bravo, charlie
local phase, ticksLeft = nil, nil

local function count(t) local n = 0 for _ in pairs(t) do n = n + 1 end return n end
local function ours(list)
	local out = {}
	for _, m in ipairs(list or {}) do if m.mod == "partytest" then out[m.name] = m end end
	return out
end

local function firstFloor()
	check("a slot action registers at top level", actionAdded == true, actionAdded)

	local ch = sam_party_choices()
	local hasWarrior, hasHuman = false, false
	for _, c in ipairs(ch and ch.classes or {}) do if c.id == "warrior" then hasWarrior = true end end
	for _, r in ipairs(ch and ch.races or {}) do if r.id == "human" then hasHuman = true end end
	check("the choices list warrior", hasWarrior, ch and #ch.classes)
	check("and human", hasHuman, ch and #ch.races)
	check("and 18 looks", ch and ch.appearances == 18, ch and ch.appearances)
	check("a human warrior is allowed", sam_party_choice_allowed("human", "warrior") == true)
	check("an unknown race is not", sam_party_choice_allowed("nosuchrace", "warrior") == false)
	local n = sam_party_random_name("female")
	check("a random name is a name", type(n) == "string" and #n > 0 and #n <= 31, n)

	-- the refusals, first, so nothing they leave behind is counted later
	check("an unknown race is refused", sam_party_spawn(0, { race = "nosuchrace" }) == nil)
	check("an unknown class is refused", sam_party_spawn(0, { class = "nosuchclass" }) == nil)
	check("a 32-character tag is refused", sam_party_spawn(0, { tag = string.rep("t", 32) }) == nil)
	check("a leader who is not here is refused", sam_party_spawn(3, { name = "Nobody" }) == nil)
	check("the refusals made nobody", count(ours(sam_party_list())) == 0, count(ours(sam_party_list())))

	alpha = sam_party_spawn(0, { name = "Alpha", race = "human", class = "warrior", sex = "female", appearance = 3, tag = "partytest:a" })
	bravo = sam_party_spawn(0, { name = "Bravo", race = "goblin", class = "wizard", respawn = false })
	charlie = sam_party_spawn(0, { name = "Charlie", class = "healer", tag = "partytest:c" })
	check("three party members spawn", alpha and bravo and charlie, tostring(alpha) .. " " .. tostring(bravo) .. " " .. tostring(charlie))
	check("party.on_member_spawned fired for each", count(spawned) == 3, count(spawned))
	check("as fresh spawns", spawned.Alpha and spawned.Alpha.respawned == 0)

	check("a party member is a member", sam_party_is_member(alpha) == true)
	check("a player is not", sam_party_is_member(sam_get_player_uid(0)) == false)
	local a = sam_party_get_info(alpha) or {}
	check("info has its name, class and race", a.name == "Alpha" and a.class == "warrior" and a.race == "human", tostring(a.name) .. " " .. tostring(a.class) .. " " .. tostring(a.race))
	check("its look and sex", a.appearance == 3 and a.sex == "female", tostring(a.appearance) .. " " .. tostring(a.sex))
	check("its leader, tag and mod", a.leader == 0 and a.tag == "partytest:a" and a.mod == "partytest", tostring(a.leader) .. " " .. tostring(a.tag) .. " " .. tostring(a.mod))
	check("and is alive at level 1 or more", (a.hp or 0) > 0 and (a.level or 0) >= 1, tostring(a.hp) .. " hp, level " .. tostring(a.level))
	local b = sam_party_get_info(bravo) or {}
	check("the default tag is the namespace", b.tag == "partytest", b.tag)
	check("respawn = false is kept", b.respawn == false, b.respawn)
	local items = sam_get_container_items(alpha) or {}
	check("a warrior carries its kit's food", #items >= 2, #items .. " items")

	check("carrying the run on is off unless asked for", (sam_party_get_info(alpha) or {}).carry_on == false and b.carry_on == false, tostring(a.carry_on) .. " " .. tostring(b.carry_on))
	check("a carry_on that is not true or false is refused", sam_party_set_options(bravo, { carry_on = "yes" }) == false)
	check("and changed nothing", (sam_party_get_info(bravo) or {}).carry_on == false)
	check("carry_on can be turned on while a player stands", sam_party_set_options(bravo, { carry_on = true }) == true and (sam_party_get_info(bravo) or {}).carry_on == true)
	check("and off again", sam_party_set_options(bravo, { carry_on = false }) == true and (sam_party_get_info(bravo) or {}).carry_on == false)
	check("options change", sam_party_set_options(bravo, { wait_at_exit = false }) == true)
	check("and read back", (sam_party_get_info(bravo) or {}).wait_at_exit == false)
	check("a player takes no options", sam_party_set_options(sam_get_player_uid(0), { respawn = true }) == false)
	check("all three are listed", count(ours(sam_party_list())) == 3, count(ours(sam_party_list())))

	check("a difficulty count of 4 is refused", sam_party_set_difficulty_count(4) == false)
	check("a card cannot be set outside a lobby", sam_lobby_set_card(1, { title = "x" }) == false)
	local s0 = sam_lobby_get_slot(0)
	check("slot 0 reads as a player", s0 and s0.state == "player", s0 and s0.state)
	check("slot 9 is nothing", sam_lobby_get_slot(9) == nil)

	sam_kill_monster(alpha)
	sam_kill_monster(bravo)
	sam_kill_monster(charlie)
end

local function afterDeaths()
	check("party.on_member_died fired for all three", count(died) == 3, count(died))
	check("Alpha would come back, Bravo would not", died.Alpha and died.Alpha.will_respawn == 1 and died.Bravo and died.Bravo.will_respawn == 0)
	local fallen = sam_party_fallen()
	check("only Alpha is waiting to come back", #fallen == 1 and fallen[1].name == "Alpha", #fallen)
	check("nobody is left on this floor", count(ours(sam_party_list())) == 0, count(ours(sam_party_list())))
	spawned = {}
	sam_travel_to_level(1)
end

local function secondFloor()
	local here = ours(sam_party_list())
	check("Alpha rejoined on the next floor", here.Alpha ~= nil and count(here) == 1, count(here))
	check("as a respawn, beside player 0", spawned.Alpha and spawned.Alpha.respawned == 1 and spawned.Alpha.leader == 0)
	check("still a warrior", here.Alpha and here.Alpha.class == "warrior")
	check("and nobody is waiting any more", #sam_party_fallen() == 0, #sam_party_fallen())
	sam_log(string.format("PARTY RESULT: %d passed, %d FAILED", pass, fail))
	sam_test_done(pass, fail)
end

function on_event(e)
	if e.name == "game.on_game_start" and e.player == 0 and phase == nil then
		phase, ticksLeft = "first", 50
	elseif e.name == "game.on_level_entered" and e.player == 0 and phase == "travel" then
		phase, ticksLeft = "second", 50
	elseif e.name == "party.on_member_spawned" and e.mod == "partytest" then
		spawned[e.member_name] = { respawned = e.respawned, leader = e.leader }
	elseif e.name == "party.on_member_died" and e.mod == "partytest" then
		died[e.member_name] = { will_respawn = e.will_respawn }
		if e.tag == "partytest:c" then return false end   -- Charlie stays dead
	end
end

function on_tick()
	if ticksLeft == nil then return end
	ticksLeft = ticksLeft - 1
	if ticksLeft > 0 then return end
	ticksLeft = nil
	if phase == "first" then
		firstFloor()
		phase, ticksLeft = "deaths", 10
	elseif phase == "deaths" then
		phase = "travel"
		afterDeaths()
	elseif phase == "second" then
		phase = "done"
		secondFloor()
	end
end
