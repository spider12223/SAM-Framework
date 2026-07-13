// Example S.A.M behavior script — JavaScript (quickjs-ng).
//
// Place a .js file next to a class JSON with the SAME base name and S.A.M loads
// it automatically:  classes/assassin.json  +  classes/assassin.js
// (.lua and .ts siblings, if present, load too — all receive every event.)
//
// Runs in a sandboxed engine: no fs/network/os, a memory + time budget, and only
// copied primitives cross the boundary (never engine pointers). Host-authoritative.

function onLevelUp(event) {
	var p = event.player;
	sam_log("Assassin leveled up to " + event.amount);

	// Reward + feedback
	sam_message(p, "You reached level " + event.amount + "!");
	sam_grant_item(p, "IRON_DAGGER");
	sam_grant_gold(p, 25);

	// Read stats / world, then a bounded write (full heal) + a temporary buff
	var maxhp = sam_get_stat(p, "MAXHP");
	sam_log("On floor " + sam_get_floor() + " with HP " + sam_get_stat(p, "HP") + "/" + maxhp);
	sam_set_stat(p, "HP", maxhp);
	sam_apply_effect(p, "LEVITATING", 250); // 5 seconds (50 ticks/sec)
	sam_apply_effect(p, "FAST", 100);
	sam_remove_effect(p, "FAST");

	// Sound + a look around + a dropped reward
	sam_play_sound(153);
	sam_log("Creatures within 20 tiles: " + sam_get_nearby_entities(p, 20).length);
	sam_spawn_item(10, 10, "GEM_RUBY");
}

function on_event(event) {
	var n = event.name;
	if (n === "player.on_level_up") {
		onLevelUp(event);
	} else if (n === "player.on_hit") {
		sam_log("Hit target " + event.target_uid + " for " + event.damage + (event.lethal === 1 ? " (kill)" : ""));
	} else if (n === "player.on_kill") {
		sam_grant_gold(event.player, 10); // bounty per kill
	} else if (n === "player.on_equip") {
		sam_log("Equipped item " + event.item_type + " in slot " + event.slot);
	} else if (n === "player.on_unequip") {
		sam_log("Unequipped item " + event.item_type + " from slot " + event.slot);
	} else if (n === "player.on_item_use") {
		sam_log("Used consumable " + event.item_type);
	} else if (n === "player.on_death") {
		sam_log("Died: " + event.obituary);
	} else if (n === "player.on_spell_cast") {
		sam_log("Cast spell " + event.spell_name);
	} else if (n === "player.on_gold_collected") {
		sam_log("Picked up " + event.amount + " gold (total " + event.total_gold + ")");
	} else if (n === "player.on_damage_taken") {
		if (event.lethal === 1) { sam_log("Took a lethal hit!"); }
	} else if (n === "player.on_floor_change") {
		sam_message(event.player, "Descending to floor " + event.new_floor);
	}
}
