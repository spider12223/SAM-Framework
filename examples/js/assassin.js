// Example S.A.M behavior script — JavaScript (v0.4.0+).
//
// Place a .js file next to a class JSON with the SAME base name and S.A.M loads
// it automatically:  classes/assassin.json  +  classes/assassin.js
// (.lua and .ts siblings, if present, load too — all receive every event.)
//
// Runs in a sandboxed quickjs-ng engine: no fs/network/os, a memory + time
// budget, and only copied primitives cross the boundary (never engine pointers).

function on_event(event) {
	if (event.name === "player.on_level_up") {
		sam_log("Assassin [JavaScript] leveled up to " + event.amount);
		sam_grant_item(event.player, "IRON_DAGGER");
	}
}
