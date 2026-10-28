-- Item Move Test: gear on allies in a real game, unattended.
--
--   tools/run_mod_test.sh ItemMoveTest   ->  PASS, exit code 0
--
-- What it does, and what would be different if the code were broken:
--   1. makes a human and a kobold follow player 0, and leaves a goblin wild;
--   2. reads the human's (empty) pockets and refuses every call that names the wild goblin;
--   3. hands the human a helmet from player 0's backpack: the SAME uid must be in its pockets;
--   4. asks whether the kobold may wear it (no: race) and the human (yes);
--   5. a handler vetoes one equip (mod_veto, with its message), then lets the next through;
--   6. equips it: still the same uid, worn, not in the pockets' "pockets" list, nothing on the floor;
--   7. takes it off again, takes it back to player 0, and does the same with a cursed helmet,
--      which must stay on.
-- Not here: a player on another machine. That needs a second machine and is played by hand.

local pass, fail = 0, 0
local function check(name, ok, saw)
	if ok then pass = pass + 1 else fail = fail + 1 end
	sam_log((ok and "PASS  " or "FAIL  ") .. name .. "   (" .. tostring(saw) .. ")")
end

local human, kobold, wild
local done, failed = {}, {}   -- by ticket
local veto = nil              -- a message: the handler refuses every equip while it is set
local vetoSeen = 0
local stage, ticksLeft = 0, nil
local waiting = nil           -- a ticket this stage is waiting on
local helmUid, newUid, cursedUid

local function pocketItem(uid)
	local inv = sam_monster_get_inventory(human)
	for _, it in ipairs(inv and inv.pockets or {}) do if it.uid == uid then return it end end
	return nil
end
local function backpackItem(uid)
	for _, it in ipairs(sam_get_inventory(0) or {}) do if it.uid == uid then return it end end
	return nil
end
local function backpackHelm(beatitude)
	for _, it in ipairs(sam_get_inventory(0) or {}) do
		if it.name == "IRON_HELM" and not it.equipped and it.beatitude == beatitude then return it end
	end
	return nil
end
local function count(t) local n = 0 for _ in pairs(t) do n = n + 1 end return n end

local function spawnNear(name)
	local px, py = sam_get_position(sam_get_player_uid(0))
	for _, d in ipairs({ { 3, 0 }, { -3, 0 }, { 0, 3 }, { 0, -3 }, { 3, 3 }, { -3, -3 }, { 4, 0 }, { 0, 4 } }) do
		local uid = sam_spawn_monster(px + d[1], py + d[2], name)
		if uid then return uid end
	end
	return nil
end

local function setup()
	human = spawnNear("human")
	kobold = spawnNear("kobold")
	wild = spawnNear("goblin")
	check("three monsters spawn", human and kobold and wild, tostring(human) .. " " .. tostring(kobold) .. " " .. tostring(wild))
	check("a wild monster follows nobody", sam_get_follower_leader(wild) == nil, tostring(sam_get_follower_leader(wild)))
	check("the human becomes a follower", sam_make_follower(0, human) == true)
	check("and the kobold", sam_make_follower(0, kobold) == true)
	check("the human's leader is player 0", sam_get_follower_leader(human) == 0, tostring(sam_get_follower_leader(human)))
	check("a player follows nobody", sam_get_follower_leader(sam_get_player_uid(0)) == nil)

	local inv = sam_monster_get_inventory(human)
	check("a follower's inventory has pockets and worn", inv and type(inv.pockets) == "table" and type(inv.worn) == "table")
	check("a fresh human wears no helmet", inv and inv.worn.helmet == nil)
	check("a wild monster's inventory is nil", sam_monster_get_inventory(wild) == nil)

	local r = sam_monster_can_equip(wild, "IRON_HELM", "helmet")
	check("can_equip on a wild monster: no_ally", r and r.ok == false and r.reason == "no_ally", r and r.reason)
	r = sam_monster_can_equip(human, "IRON_HELM", "nosuchslot")
	check("a slot that does not exist: bad_slot", r and r.reason == "bad_slot", r and r.reason)
	r = sam_monster_can_equip(human, "NO_SUCH_ITEM", "helmet")
	check("an item that does not exist: unknown_item", r and r.reason == "unknown_item", r and r.reason)
	r = sam_monster_can_equip(human, "IRON_HELM", "shoes")
	check("a helmet in the shoes slot: wrong_slot", r and r.reason == "wrong_slot", r and r.reason)
	r = sam_monster_can_equip(kobold, "IRON_HELM", "helmet")
	check("a kobold may not wear a helmet: race", r and r.ok == false and r.reason == "race", r and r.reason)
	r = sam_monster_can_equip(human, "IRON_HELM", "helmet")
	check("a human may", r and r.ok == true, r and r.reason)
	r = sam_give_item_to_ally(0, wild, 1, 0)
	check("giving to a wild monster: no_ally", r and r.ok == false and r.reason == "no_ally", r and r.reason)
	r = sam_take_item_from_ally(0, wild, 1, 0)
	check("taking from one: no_ally", r and r.ok == false and r.reason == "no_ally", r and r.reason)
	r = sam_monster_equip_item(wild, "helmet", 1)
	check("equipping one: no_ally", r and r.ok == false and r.reason == "no_ally", r and r.reason)

	check("player 0 is given a helmet", sam_grant_item(0, "IRON_HELM") == true)
	local h = backpackHelm(0)
	helmUid = h and h.uid
	check("and it is in the backpack", helmUid ~= nil, tostring(helmUid))
	r = sam_give_item_to_ally(0, human, 987654, 0)
	check("an item that is not there: unknown_item", r and r.reason == "unknown_item", r and r.reason)
	r = sam_give_item_to_ally(0, human, helmUid, 5)
	check("more than the stack: bad_count", r and r.reason == "bad_count", r and r.reason)

	r = sam_give_item_to_ally(0, human, helmUid, 0)
	check("giving it is accepted with a ticket", r and r.ok == true and (r.ticket or 0) > 0, r and (r.reason or r.ticket))
	waiting = r and r.ticket
	local again = sam_give_item_to_ally(0, human, helmUid, 0)
	check("the same item again while it is moving: busy", again and again.reason == "busy", again and again.reason)
	check("nothing has moved before the tick", pocketItem(helmUid) == nil and backpackItem(helmUid) ~= nil)
end

local function afterGive()
	local d = done[waiting]
	check("done fired for the give, and only done", d ~= nil and failed[waiting] == nil, d and d.new_item_uid)
	check("the SAME uid is in the ally's pockets", pocketItem(helmUid) ~= nil, tostring(pocketItem(helmUid)))
	check("and the player no longer has it", backpackItem(helmUid) == nil)
	check("done says which item and how many", d and d.item_uid == helmUid and d.new_item_uid == helmUid and d.count == 1, d and (tostring(d.item_uid) .. " " .. tostring(d.count)))

	local r = sam_monster_can_equip(human, helmUid, "helmet")
	check("can_equip by item uid: yes", r and r.ok == true, r and r.reason)

	veto = "Only knights wear that."
	local before = vetoSeen
	r = sam_monster_can_equip(human, helmUid, "helmet")
	check("a handler's veto: mod_veto", r and r.ok == false and r.reason == "mod_veto", r and r.reason)
	check("with the handler's own message", r and r.text == "Only knights wear that.", r and r.text)
	check("the handler was asked", vetoSeen == before + 1, vetoSeen - before)
	r = sam_monster_equip_item(human, "helmet", helmUid)
	check("equip is refused the same way", r and r.reason == "mod_veto", r and r.reason)
	check("and the helmet stayed in the pockets", pocketItem(helmUid) ~= nil and (sam_monster_get_inventory(human).worn.helmet == nil))
	veto = nil

	r = sam_monster_equip_item(human, "helmet", 987654)
	check("equipping an item that is not in the pockets: not_in_pockets", r and r.reason == "not_in_pockets", r and r.reason)
	r = sam_monster_equip_item(human, "helmet", helmUid)
	check("equip works", r and r.ok == true, r and r.reason)
	local inv = sam_monster_get_inventory(human)
	check("the same uid is worn", inv.worn.helmet and inv.worn.helmet.uid == helmUid, inv.worn.helmet and inv.worn.helmet.uid)
	check("and it is listed once, under worn", pocketItem(helmUid) == nil)
	r = sam_give_item_to_ally(0, human, helmUid, 0)
	check("an item the player no longer holds cannot be given: unknown_item", r and r.ok == false and r.reason == "unknown_item", r and r.reason)
	r = sam_take_item_from_ally(0, human, helmUid, 0)
	check("nor taken while it is worn: equipped", r and r.reason == "equipped", r and r.reason)

	r = sam_monster_unequip_to_inventory(human, "helmet")
	check("unequip works", r and r.ok == true, r and r.reason)
	check("the helmet is back in the pockets under its uid", pocketItem(helmUid) ~= nil)
	r = sam_monster_unequip_to_inventory(human, "helmet")
	check("an empty slot: empty_slot", r and r.reason == "empty_slot", r and r.reason)

	r = sam_take_item_from_ally(0, human, helmUid, 0)
	check("taking it back is accepted", r and r.ok == true and (r.ticket or 0) > 0, r and r.reason)
	waiting = r and r.ticket
end

local function afterTake()
	local d = done[waiting]
	check("done fired for the take", d ~= nil and failed[waiting] == nil)
	newUid = d and d.new_item_uid
	check("the item is in the backpack under the uid done reported", newUid and backpackItem(newUid) ~= nil, tostring(newUid))
	check("and left the pockets", pocketItem(helmUid) == nil)

	-- a cursed helmet: it goes on, and then it will not come off
	check("player 0 is given a cursed helmet", sam_grant_item(0, "IRON_HELM", -2) == true)
	local c = backpackHelm(-2)
	cursedUid = c and c.uid
	check("and it is in the backpack", cursedUid ~= nil)
	local r = sam_give_item_to_ally(0, human, cursedUid, 0)
	waiting = r and r.ticket
	check("giving it is accepted", r and r.ok == true, r and r.reason)
end

local function afterCursedGive()
	check("the cursed helmet reached the pockets", pocketItem(cursedUid) ~= nil)
	local r = sam_monster_equip_item(human, "helmet", cursedUid)
	check("it goes on", r and r.ok == true, r and r.reason)
	r = sam_monster_unequip_to_inventory(human, "helmet")
	check("and will not come off: cursed", r and r.ok == false and r.reason == "cursed", r and r.reason)
	check("it is still worn, the same uid", (sam_monster_get_inventory(human).worn.helmet or {}).uid == cursedUid)
	r = sam_monster_can_equip(human, newUid or helmUid, "helmet")
	check("another helmet cannot replace it: cursed_in_slot", r and r.reason == "cursed_in_slot", r and r.reason)

	sam_log(string.format("ITEM MOVE RESULT: %d passed, %d FAILED", pass, fail))
	sam_test_done(pass, fail)
end

local stages = { setup, afterGive, afterTake, afterCursedGive }

function on_event(e)
	if e.name == "game.on_game_start" and e.player == 0 and stage == 0 then
		stage, ticksLeft = 1, 50
	elseif e.name == "item.on_ally_transfer_done" then
		done[e.ticket] = e
	elseif e.name == "item.on_ally_transfer_failed" then
		failed[e.ticket] = e
	elseif e.name == "item.on_before_monster_equip" and veto then
		vetoSeen = vetoSeen + 1
		e.message = veto
		return false
	end
end

local ticksRun = 0
function on_tick()
	ticksRun = ticksRun + 1
	if stage > 0 and ticksRun > 2000 and stages[stage] then
		check("the test finished in time", false, "stuck at stage " .. stage)
		stages = {}
		sam_log(string.format("ITEM MOVE RESULT: %d passed, %d FAILED", pass, fail))
		sam_test_done(pass, fail)
		return
	end
	if ticksLeft ~= nil then
		ticksLeft = ticksLeft - 1
		if ticksLeft > 0 then return end
		ticksLeft = nil
		stages[stage]()
		stage = stage + 1
		return
	end
	-- a stage that started a transfer goes on once its event has come
	if waiting and (done[waiting] or failed[waiting]) and stages[stage] then
		local run = stages[stage]
		stages[stage] = nil
		run()
		stage = stage + 1
	end
end
