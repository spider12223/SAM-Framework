// Example S.A.M behavior script — TypeScript.
//
// Place a .ts file next to a class JSON with the SAME base name and S.A.M loads
// it automatically:  classes/assassin.json  +  classes/assassin.ts
// The .ts is transpiled to JavaScript ONCE at mod-load (by the TypeScript compiler
// running under the embedded QuickJS) and cached by content hash, then run in the
// same sandbox as a .js script. Host-authoritative; only copied primitives cross in.

declare function sam_log(msg: string): void;
declare function sam_message(player: number, text: string): boolean;
declare function sam_grant_item(player: number, item: string): boolean;
declare function sam_grant_gold(player: number, amount: number): boolean;
declare function sam_apply_effect(player: number, effect: string, ticks: number): boolean;
declare function sam_remove_effect(player: number, effect: string): boolean;
declare function sam_get_stat(player: number, stat: string): number;
declare function sam_set_stat(player: number, stat: string, value: number): boolean;
declare function sam_get_floor(): number;
declare function sam_spawn_item(x: number, y: number, item: string): boolean;
declare function sam_play_sound(soundId: number, vol?: number): boolean;
declare function sam_get_nearby_entities(player: number, radius: number): number[];

// Types are erased at transpile time; this enum emits a real runtime object, so it
// proves the .ts is genuinely compiled (not just stripped) before it runs.
enum Effect {
	Levitating = "LEVITATING",
	Fast = "FAST",
}

interface SamEvent {
	name: string;
	player: number;
	amount?: number;
	target_uid?: number;
	damage?: number;
	lethal?: number;
	item_type?: number;
	slot?: string;
	obituary?: string;
	spell_name?: string;
	total_gold?: number;
	new_floor?: number;
}

function onLevelUp(p: number, level: number): void {
	sam_log("Assassin leveled up to " + level);

	sam_message(p, "You reached level " + level + "!");
	sam_grant_item(p, "IRON_DAGGER");
	sam_grant_gold(p, 25);

	const maxhp: number = sam_get_stat(p, "MAXHP");
	sam_log("On floor " + sam_get_floor() + " with HP " + sam_get_stat(p, "HP") + "/" + maxhp);
	sam_set_stat(p, "HP", maxhp);
	sam_apply_effect(p, Effect.Levitating, 250); // 5 seconds (50 ticks/sec)
	sam_apply_effect(p, Effect.Fast, 100);
	sam_remove_effect(p, Effect.Fast);

	sam_play_sound(153);
	sam_log("Creatures within 20 tiles: " + sam_get_nearby_entities(p, 20).length);
	sam_spawn_item(10, 10, "GEM_RUBY");
}

function on_event(event: SamEvent): void {
	const n: string = event.name;
	if (n === "player.on_level_up") {
		onLevelUp(event.player, event.amount ?? 0);
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
		sam_log("Picked up gold (total " + event.total_gold + ")");
	} else if (n === "player.on_damage_taken") {
		if (event.lethal === 1) { sam_log("Took a lethal hit!"); }
	} else if (n === "player.on_floor_change") {
		sam_message(event.player, "Descending to floor " + event.new_floor);
	}
}
