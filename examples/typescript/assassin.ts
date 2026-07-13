// Example S.A.M behavior script — TypeScript (v0.4.0+).
//
// Place a .ts file next to a class JSON with the SAME base name and S.A.M loads
// it automatically:  classes/assassin.json  +  classes/assassin.ts
// The .ts is transpiled to JavaScript ONCE at mod-load (by the TypeScript
// compiler running under the embedded QuickJS) and cached by content hash, then
// run in the same sandbox as a .js script.
//
// Types are erased; this file uses an enum + interface to show real transpilation
// (a type-erasure-only tool couldn't emit the enum's runtime object).

enum LootTable {
	LevelReward = "IRON_DAGGER",
}

interface LevelUpEvent {
	name: string;
	player: number;
	amount: number;
}

function on_event(event: LevelUpEvent): void {
	if (event.name === "player.on_level_up") {
		sam_log("Assassin [TypeScript] leveled up to " + event.amount);
		sam_grant_item(event.player, LootTable.LevelReward);
	}
}
