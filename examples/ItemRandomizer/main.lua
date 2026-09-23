-- Item Randomizer: everything joins the loot pool from floor 0, artifacts, orbs and class
-- spellbooks included, and the shops keep working.
--
-- The community mod this reproduces replaces items.json, which is why it breaks every mod that
-- adds an item and why its deep Arms & Armour stores sell only rocks: the store asks for a rare
-- tier, the edited sheet has nothing at that tier, and the engine's fallback is a rock. This
-- version touches the loot RULES instead of the sheet: sam_patch_item for the floor eligibility,
-- the loot tables for what may appear where, and two events: one for the store's minimum level,
-- one to keep the spellbook the item roll picked.
--
-- Steps 1 to 3 run at load, on every machine, and depend on nothing but the sheet. The table
-- writers are of kind "all", so a joiner with the mod builds the same pool the host does, and a
-- joiner without it still plays. The Lite rule is different: it is driven by a SETTING, and a
-- setting's value is per machine, so it is applied from game.on_game_start instead (step 4).

local ARTIFACTS = {
	"ARTIFACT_SWORD", "ARTIFACT_MACE", "ARTIFACT_SPEAR", "ARTIFACT_AXE", "ARTIFACT_BOW",
	"ARTIFACT_BREASTPIECE", "ARTIFACT_HELM", "ARTIFACT_BOOTS", "ARTIFACT_CLOAK", "ARTIFACT_GLOVES",
	"MASK_ARTIFACT_VISOR",
}
local ORBS = { "ARTIFACT_ORB_BLUE", "ARTIFACT_ORB_RED", "ARTIFACT_ORB_PURPLE", "ARTIFACT_ORB_GREEN" }

-- The Lite flavour of the original: artifacts and orbs still drop, but no shop stocks them.
sam_register_setting("lite", {
	type = "toggle", label = "Lite: keep artifacts and orbs out of shops",
	tooltip = "They still turn up on the floor and in chests.",
	default = false,
})

-- 1. Every sheet item vanilla never rolls (item level -1: artifacts, orbs, keys, class gear,
--    tinkering products, quivers, foci...) becomes eligible from floor 0. Spells are not items.
local opened = 0
for _, it in ipairs(sam_list_items()) do
	if it.level == -1 and it.category ~= "SPELL_CAT" and it.category ~= "TOME_SPELL" then
		if sam_patch_item(it.type, { level = 0 }) then opened = opened + 1 end
	end
end

-- 2. A rolled spellbook is re-drawn from the spell table, and vanilla only draws spells with a
--    drop table. Let every spell the catalog lists come out of that draw, from floor 0. This is
--    the rule the level generator applies to the floor's own books and chests (it runs on a
--    worker thread where no event can fire), and it can only name the spells sam_list_spells
--    returns: class, race and monster spells are hidden from that list, so their books are
--    handled by the world.on_before_spellbook_reroll handler in on_event instead.
local books = 0
for _, sp in ipairs(sam_list_spells()) do
	if sam_set_spell_droppable(sp.id, true, 0) then books = books + 1 end
end

-- 3. Nothing but potions becomes an alchemy recipe: the recipe picker uses the same loot curve,
--    and without this an artifact could be "learned" as a recipe on level up.
for _, it in ipairs(sam_list_items()) do
	if it.category ~= "POTION" then sam_set_loot_context(it.type, "recipe", false) end
end

-- 4. Lite: artifacts and orbs are never sold. The toggle is a per-machine setting and the shop
--    table is an `all` table, so it has to be the HOST's toggle that everyone applies. While mods
--    load sam_is_host() is true on every machine (the game has not started), so applying it here
--    would give a joiner its own copy of the table, built from its own toggle. game.on_game_start
--    fires on the host only, and an `all` call made there reaches every S.A.M client and is
--    replayed to a late joiner; the on_setting_changed branch below is guarded the same way.
local function applyLite(on)
	for _, list in ipairs({ ARTIFACTS, ORBS }) do
		for _, id in ipairs(list) do sam_set_loot_context(id, "shop", not on) end
	end
end

sam_log(string.format("Item Randomizer: %d items opened to every floor, %d spellbooks droppable.", opened, books))

function on_event(e)
	if e.name == "world.on_before_loot_roll" and e.context == "shop" then
		-- The Hamlet fix. A deep store asks for WEAPON 10+, ARMOR 5+, THROWN 8+; with every level
		-- now 0 that window is empty and the engine would hand it a rock. Open the window.
		e.min_level = 0

	elseif e.name == "world.on_before_spellbook_reroll" then
		-- Keep the book the item roll picked instead of re-drawing it from the spell table. Step 1
		-- opened the class and race spellbooks (item level -1 in the sheet) from floor 0, but the
		-- engine re-draws every rolled spellbook from the spells it lists, and those spells are
		-- hidden, so the book was thrown away again. keep = 1 lets it through from the first floor;
		-- allow_hidden = 1 would instead run the difficulty ladder over the hidden spells (a
		-- difficulty-100 class spell only from floor 20) and admit the engine's bookless foci
		-- spells, which come out as tomes of nothing. This fires for the rolls made during play (a
		-- shop stocking, a creature's pockets, a chest summoned by a spell); the floor's own books
		-- are rolled where no event fires and stay the listed spells' books.
		e.keep = 1

	elseif e.name == "game.on_game_start" and e.player == 0 then
		-- The host's toggle, once, as the game starts (step 4 says why not at load).
		applyLite(sam_get_setting("lite") == true)

	elseif e.name == "mod.on_setting_changed" and e.mod == "randomizer" and e.id == "lite" then
		-- The toggle takes effect at once, for the next shop the dungeon generates. The event's
		-- old/new are text (event fields are numbers and strings); the setting itself is the
		-- boolean, and it is already stored when this fires. On a joiner the row is its own and
		-- the table is the host's, so a joiner's flip changes nothing: an `all` call from a client
		-- is refused anyway, and the guard keeps that refusal out of the log.
		if sam_is_host() then applyLite(sam_get_setting("lite") == true) end
	end
end
