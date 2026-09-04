// TypeScript definitions for the S.A.M Framework scripting API.
// Generated from the API definition; do not edit by hand.
//
// Drop this beside your mod's .ts files, or reference it:
//   /// <reference path="sam.d.ts" />
//
// 184 functions, 72 events.

declare global {
  /**
   * Grant a class a permanent status effect at character creation (bakes at creation; run at mod-load).
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_add_class_passive(class_: string, effect: string): boolean;

  /**
   * Add to a player's move-speed multiplier (the result is clamped to [0.1, 3.0]). Additive counterpart to sam_set_move_speed — use it to stack a bonus onto whatever the multiplier already is (e.g. +0.1 on top of a 2.0 from another ability). Host-only; syncs to the owning client.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_add_move_speed(player: number, delta: number): number;

  /**
   * Apply a status effect to a player for N ticks (50 ticks = 1s). Optional strength sets the tier/magnitude for effects that carry one (e.g. GROWTH stacks) — omit it for the plain default. Targets the player, never a monster.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_apply_effect(player: number, effect: string, ticks: number, strength: number): boolean;

  /**
   * Apply a status effect to a monster by UID for N ticks.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_apply_monster_effect(uid: string, effect: string, ticks: number): boolean;

  /**
   * Attach one of your registered behaviours to a live monster. It runs AFTER vanilla AI each frame rather than replacing it, so the creature still fights and paths normally and your code layers on top.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_attach_behavior(uid: number, behavior: string): boolean;

  /**
   * Shake a player's camera. 1 is a nudge, ~10 a solid hit, 20+ violent. Feeds Barony's own shake channels so it decays naturally; for a remote client the host forwards it.
   */
  function sam_camera_shake(player: number, magnitude: number): boolean;

  /**
   * Cancel a pending timer by id (for the calling mod).
   */
  function sam_cancel_timer(id: string): void;

  /**
   * Immediately FIRE a spell/bolt from a player in the direction they face (free, no mana). Great for 'shoot on block'. Don't call from an on_spell_cast handler.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_cast_spell(player: number, spell: string): boolean;

  /**
   * Fire a spell AIMED at an entity (aims the bolt toward it) instead of straight ahead. Free cast, host-only.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_cast_spell_at(player: number, target_uid: string, spell: string): number;

  /**
   * Fire a spell aimed at a map tile. Free cast, host-only.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_cast_spell_pos(player: number, tile_x: number, tile_y: number, spell: string): number;

  /**
   * Strip EVERY active status effect from a player at once — buffs and debuffs, vanilla and custom.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_clear_effects(player: number): number;

  /**
   * Drop a script-set model and go back to whatever the entity would otherwise draw. Clients are told too, so a transformation can end cleanly.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_clear_model(uid: number): boolean;

  /**
   * Make a companion THRUST forward for a few ticks — the punch motion. Call it repeatedly on a fast repeating timer (e.g. every 3 ticks) during an ability to read as a continuous ORA-ORA flurry. Purely visual on the companion itself; combine with sam_cast_spell (forward projectile + real damage) and/or sam_get_nearby_entities + sam_deal_damage for the hits.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_companion_punch(uid: number): boolean;

  /**
   * The floating combat number the game shows on a hit. Lets a mod's custom damage read like real damage instead of being invisible.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_damage_number(uid: string, amount: number, type: number): boolean;

  /**
   * Deal `amount` damage to any entity by UID (positive = damage); existence-validated.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_deal_damage(entity_uid: string, amount: number): boolean;

  /**
   * Delete a persisted per-mod key.
   */
  function sam_delete_data(key: string): boolean;

  /**
   * Remove whatever behaviour a script attached to this entity. Local bookkeeping, so it is safe to call anywhere and on a uid that has none.
   */
  function sam_detach_behavior(uid: number): boolean;

  /**
   * Entities of a KIND near a tile. This is the gap sam_get_nearby_entities leaves: that one skips anything which is not a monster or a player, so doors, chests, levers, gold and dropped items were invisible to scripts.
   */
  function sam_find_entities(x: number, y: number, radiusTiles: number, kind: string): any[];

  /**
   * Fire a custom event to ALL Lua + JS/TS scripts cross-runtime. Only number/bool/string fields cross over; recursion capped at depth 8.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_fire_hook(name: string, event: any): number;

  /**
   * Armor class as the damage formula sees it, gear included.
   */
  function sam_get_ac(uid: string): number;

  /**
   * What the player actually has an action bound to — use it to print a correct prompt instead of guessing a key.
   */
  function sam_get_action_binding(player: number, action: string): string;

  /**
   * Get a player's class name (vanilla or custom).
   */
  function sam_get_class(player: number): string;

  /**
   * What is inside a chest, or what a creature is carrying.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_get_container_items(uid: string): any[];

  /**
   * How many ticks of an effect are left (50 = 1s) — so a debuff can scale or decay by time remaining. Readable on clients.
   */
  function sam_get_effect_duration(player: number, effect: string): number;

  /**
   * The effect's strength/magnitude for effects that store one (GROWTH tiers, potion STR). Readable on clients.
   */
  function sam_get_effect_strength(player: number, effect: string): number;

  /**
   * A stat as the game actually uses it — gear, effects and curses folded in — rather than the raw number on the sheet.
   */
  function sam_get_effective_stat(uid: string, stat: string): number;

  /**
   * Every active effect on a player at once — react to "any debuff" or strip all buffs without polling each effect by name. Custom pseudo-effect slots appear as "CUSTOM:<id>".
   */
  function sam_get_effects(player: number): any[];

  /**
   * Read which way an entity is pointing. sam_get_facing takes a PLAYER index and reads where that player looks; this takes an entity uid, which is what a behaviour is handed. Feed it straight to sam_spawn_projectile to fire where the thing is aiming.
   */
  function sam_get_entity_facing(uid: number): number;

  /**
   * Get the item NAME equipped in a slot (ARMOR==BREASTPLATE, BOOTS==SHOES). Vanilla items only — it can't name a custom item, so use sam_get_equipped_item_id to test for one.
   */
  function sam_get_equipped_item(player: number, slot: string): string;

  /**
   * Get the item ID equipped in a slot. Compare it against sam_item_id("namespace:item") to check whether YOUR custom item is equipped — the id is a number, so the name-returning version above can never match it.
   */
  function sam_get_equipped_item_id(player: number, slot: string): number;

  /**
   * Read which way a player is looking. 0 = +x (east), increasing toward +y — so the forward unit vector is (cos yaw, sin yaw) and 'behind' is yaw + π. Use it to place things relative to a player's facing (a marker in front, a follower behind) or to aim. Host-authoritative for remote players; a client always sees its own facing correctly.
   */
  function sam_get_facing(player: number): number;

  /**
   * Read a lobby setting the host chose at game start. Lets a mod adapt to the run it is actually in — skip a hunger mechanic when hunger is off, or scale difficulty when hardcore is on.
   */
  function sam_get_flag(flag: string): any;

  /**
   * Get the current floor/dungeon level.
   */
  function sam_get_floor(): number;

  /**
   * The picture's own pixel size, so a script can centre or scale it instead of hard-coding the numbers it was exported at. Also the cheapest way to check a picture actually resolves.
   */
  function sam_get_image_size(image: string): any[];

  /**
   * List a player's inventory. Use each item's uid with sam_remove_item. Empty list for an invalid player.
   */
  function sam_get_inventory(player: number): any;

  /**
   * Count how many of an item (vanilla or custom name) a player holds.
   */
  function sam_get_inventory_count(player: number, item_name: string): number;

  /**
   * The category of an item (WEAPON / ARMOR / GEM / POTION / SCROLL / SPELLBOOK / …). Pass an event's item_type to react by category — e.g. reward the player for identifying any GEM.
   */
  function sam_get_item_category(item: string): string;

  /**
   * Look up one item by type number or by name. The attributes sub-table is where a tooltip's numbers come from (ATK, AC and so on), so this is enough to render your own item description in a panel.
   */
  function sam_get_item_info(item: number): any;

  /**
   * Get the SAM-tracked per-player kill count for this session.
   */
  function sam_get_kills(player: number): number;

  /**
   * Everything about the current floor. sam_get_floor returns a bare number that cannot tell a secret branch from the main one, so location-gated content was impossible before this.
   */
  function sam_get_level_info(): any[];

  /**
   * How lit a tile is, computed exactly the way the engine computes it, so the number you get back is the number monster vision thresholds on rather than an approximation of it. Barony keeps one SHARED lightmap holding light that is there for everyone (a wall torch, a lit room) plus one per camera that also holds that player's own glow. This reads the shared one by default, because that is the one the AI reads. Pass a player index if you want what that player's screen actually shows instead.
   */
  function sam_get_light_at(x: number, y: number, player: number): any;

  /**
   * Read back the model ID a script set on this entity. Returns nil for an entity drawing its ordinary model.
   */
  function sam_get_model(uid: number): string;

  /**
   * Every S.A.M mod loaded right now. Cross-mod integration with zero engine work: soft-depend on another mod, avoid double registering, or light up extra content when a partner mod is present.
   */
  function sam_get_mods(): any[];

  /**
   * Read per-monster scratch data (boss phases, etc.); in-memory, cleared on shutdown.
   */
  function sam_get_monster_data(uid: string, key: string): any;

  /**
   * How many ticks of an effect a monster has left.
   */
  function sam_get_monster_effect_duration(uid: string, effect: string): number;

  /**
   * A monster effect's strength/magnitude.
   */
  function sam_get_monster_effect_strength(uid: string, effect: string): number;

  /**
   * Every active effect on a monster at once (custom slots appear as "CUSTOM:<id>").
   */
  function sam_get_monster_effects(uid: string): any[];

  /**
   * For a mod's custom monster this is the variant name it was given ("Rathalos"). A plain vanilla creature carries an empty variant name, so this falls back to the species name and never hands a script an empty string.
   */
  function sam_get_monster_name(uid: string): string;

  /**
   * Read a monster's stat by UID. DEX aliases SPEED.
   */
  function sam_get_monster_stat(uid: string, stat: string): number;

  /**
   * Get the player index a monster is currently targeting (if any).
   */
  function sam_get_monster_target(uid: string): number;

  /**
   * Identify a creature by name instead of the raw integer in an event payload. NOTE this is the BASE type: a custom monster is a variant of a vanilla species, so a mod's "Rathalos" built on a bat answers "bat". Use sam_get_monster_name for the variant's own name, or sam_monster_has_trait to tell modded creatures apart.
   */
  function sam_get_monster_type(uid: string): string;

  /**
   * Read a player's move-speed multiplier. Readable on clients.
   */
  function sam_get_move_speed(player: number): number;

  /**
   * List UIDs of monsters/players within `radius` tiles of a player (never raw pointers).
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_get_nearby_entities(player: number, radius: number): any[];

  /**
   * Read back a per-player in-memory value set by sam_set_player_data.
   */
  function sam_get_player_data(player: number, key: string): any;

  /**
   * Get a player's entity uid, so the uid-based world-ops (get/set position) can act on that player's body.
   */
  function sam_get_player_uid(player: number): number;

  /**
   * Read any entity's map-tile position (player, monster or ground item). Get a player's uid with sam_get_player_uid.
   */
  function sam_get_position(uid: number): any[];

  /**
   * Get a player's race: a custom race's "namespace:race" id, or the vanilla race name ("human", "skeleton", …). Use it in a race behavior script to gate logic to players of that race.
   */
  function sam_get_race(player: number): string;

  /**
   * Read the seed identifying this run. Pair it with sam_random when you want per-run variety that every player still agrees on.
   */
  function sam_get_seed(): number;

  /**
   * A proficiency rank. Accepts both spellings — "PRO_SWORD" (the class schema) and "sword" (what player.on_proficiency_increased hands you). effective (default true) includes the equipment bonus the game actually uses; pass false for the raw trained rank. Ranks were completely unreadable before this, even though the framework has always fired the event.
   */
  function sam_get_skill(uid: string, skill: string, effective: boolean): any;

  /**
   * List the spells a player currently knows.
   */
  function sam_get_spells(player: number): any[];

  /**
   * Read a live player stat. Refused on a multiplayer client.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_get_stat(player: number, stat: string): number;

  /**
   * Read one map tile. Liquid comes from the FLOOR tile, and the engine decides which tiles are liquid from their image filename — so a mod's own tile named "...lava..." reports as lava here too.
   */
  function sam_get_tile(x: number, y: number): any[];

  /**
   * Get elapsed game ticks for the current run.
   */
  function sam_get_time_played(): number;

  /**
   * Add gold to a player (clamped to >= 0), syncing the client HUD.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_grant_gold(player: number, amount: number): boolean;

  /**
   * Give a vanilla item (e.g. "IRON_DAGGER") to a player. Local player only for now.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_grant_item(player: number, item_name: string): boolean;

  /**
   * Grant a spell to a player: a vanilla SPELL_ name, or a custom "namespace:spell".
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_grant_spell(player: number, spell: string): boolean;

  /**
   * Check whether a player currently has a status effect.
   */
  function sam_has_effect(player: number, effect: string): boolean;

  /**
   * Take the overlay away early. No player clears every player's.
   */
  function sam_hide_image(player: number): boolean;

  /**
   * Briefly freeze enemy and projectile logic — a freeze-frame — for duration_ms (capped ~400). The player, HUD weapon and hand magic keep animating, so it reads as a punchy impact beat. SINGLEPLAYER ONLY: freezing host logic in a netgame would desync clients.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_hitstop(duration_ms: number): boolean;

  /**
   * Show or update a horizontal bar — a custom resource, a charge meter, a boss health track. frac is clamped to 0..1; 0 draws as empty rather than a sliver.
   */
  function sam_hud_bar(id: string, x: number, y: number, w: number, h: number, frac: number, color: number): boolean;

  /**
   * Remove one HUD element. No id removes the whole script HUD. The HUD is also dropped automatically when the mod unloads, so it can never outlive the mod that drew it.
   */
  function sam_hud_clear(id: string): boolean;

  /**
   * A PERSISTENT picture in the script HUD — a portrait, a custom gauge, a marker. Stays until sam_hud_clear(id) or the mod unloads, unlike the overlay. w/h of 0 means the picture's own pixel size. The colour is MIXED into the art, so white (the default) leaves it untouched and the alpha byte fades it.
   */
  function sam_hud_image(id: string, x: number, y: number, w: number, h: number, image: string, color: number): boolean;

  /**
   * Show or update a line of text on screen. Calling again with the same id moves/retitles the existing line rather than stacking a new one.
   */
  function sam_hud_text(id: string, x: number, y: number, text: string, color: number): boolean;

  /**
   * The EXAGGERATED version of the flash: a colour pop PLUS manga speed lines converging on screen centre PLUS a bright core flare. Pair it with sam_camera_shake and sam_hitstop for a full impact beat. lines is the speed-line count (0 = a plain flash).
   */
  function sam_impact_frame(player: number, r: number, g: number, b: number, intensity: number, duration_ms: number, lines: number): boolean;

  /**
   * Check whether a BOUND action is held. Reads Barony's own binding, so it follows whatever the player rebound it to (and works with mouse buttons, which raw keys can't see). Local player only — input never leaves its machine.
   */
  function sam_is_action_held(player: number, action: string): boolean;

  /**
   * Whether the player is actually blocking right now — the real engine state, not just the Defend button being down. Works for remote players in multiplayer.
   */
  function sam_is_defending(player: number): boolean;

  /**
   * Would these two fight? The engine's own allegiance answer, so charm, race and faction are all accounted for.
   */
  function sam_is_enemy(uid_a: string, uid_b: string): boolean;

  /**
   * The other side of sam_is_enemy — allies, followers and charmed creatures.
   */
  function sam_is_friend(uid_a: string, uid_b: string): boolean;

  /**
   * Whether a dead player is walking around as a ghost. Worth checking before granting items or applying effects, since a ghost is not an ordinary player.
   */
  function sam_is_ghost(player: number): boolean;

  /**
   * Whether this machine is the host. Most functions are host-only and warn on a client; check this first instead of letting a client fill the log with refusals.
   */
  function sam_is_host(): boolean;

  /**
   * Check whether a supported RAW key is currently held (A-Z, 0-9, F1-F12). Ignores the player's keybinds — prefer sam_is_action_held, which follows them.
   */
  function sam_is_key_held(key_name: string): boolean;

  /**
   * Is a given mod namespace loaded? The cheap form of sam_get_mods.
   */
  function sam_is_mod_loaded(namespace: string): boolean;

  /**
   * Is this a sane place to put something: in bounds, not inside a wall, not lava. Check before spawning instead of dropping a monster into rock.
   */
  function sam_is_spawnable(x: number, y: number): boolean;

  /**
   * The stricter ghost test: a spirit ghost specifically, rather than any ghost state.
   */
  function sam_is_spirit_ghost(player: number): boolean;

  /**
   * Resolve an item's numeric type id — compare it against event fields like on_block's shield_type to react only to a specific item. Accepts a vanilla name or a custom "namespace:item".
   */
  function sam_item_id(name: string): number;

  /**
   * Kill a monster by UID (runs its normal death + drops; fires on_monster_died).
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_kill_monster(uid: string): boolean;

  /**
   * Level a player up count times (default 1) through the real engine path: attribute rolls, HP/MP gain, the level-up screen and sound, and full client sync — the actual benefits, unlike bumping LVL with sam_set_stat. Host-only. Fires the player.on_level_up hook once per level.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_level_up(player: number, count: number): boolean;

  /**
   * Can a straight line get from A to B? This is the engine's own trace, so it agrees with what is drawn — unlike plain distance, which sees through solid rock.
   */
  function sam_line_of_sight(x1: number, y1: number, x2: number, y2: number, blockedByEntities: boolean): any;

  /**
   * List the keys sam_save_data has written for your mod, so you can iterate stored state without having to remember every key name. Returns an empty table when nothing has been saved yet.
   */
  function sam_list_data_keys(): any[];

  /**
   * List every item the game knows about, including items added by mods (those have custom = true). This is what a recipe browser, a shop's stock list or a bestiary of loot is built from — before it, a script could only ask about the item already in the player's hand.
   */
  function sam_list_items(category?: string): any[];

  /**
   * List the game's monster types. Note what this does NOT include: a S.A.M custom monster is a variant of a base species rather than a new entry in the engine's table, so it will not appear here as its own row — you will see the species it is built on. The NOTHING sentinel and the engine's reserved padding slots are filtered out. Pair with sam_spawn_monster for an arena mod, or with a panel for a bestiary.
   */
  function sam_list_monsters(): any[];

  /**
   * List the spells a player can actually be given, with their mana cost. Spells the game hides from its own UI are left out, so what you get back is the set that is meaningful to show a player.
   */
  function sam_list_spells(): any[];

  /**
   * Read back a persisted per-mod value.
   */
  function sam_load_data(key: string): any;

  /**
   * The player index THIS machine controls.
   */
  function sam_local_player(): number;

  /**
   * Write a line to sam_log.txt (the only output channel). Also exposed as sam.log(msg) in Lua.
   */
  function sam_log(msg: string): void;

  /**
   * Turn one entity to face another. This is the one a turret wants — it does the trigonometry so you do not have to. Because your behaviour owns the entity, the engine has no opinion about which way it points; you do. Refuses to turn a PLAYER: their facing belongs to whoever is holding the mouse, and a script fighting their input every frame would feel broken.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_look_at(uid: number, target_uid: number): boolean;

  /**
   * Show a line in a player's in-game message log.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_message(player: number, text: string): boolean;

  /**
   * Rewrite incoming damage (clamped to >= 0). ONLY valid inside an on_before_damage callback.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_modify_damage(player: number, new_value: number): void;

  /**
   * Rewrite the damage a MONSTER is about to take. Only valid inside an on_before_monster_damage callback. No subject argument: only one monster is ever mid-dispatch.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_modify_monster_damage(newValue: number): boolean;

  /**
   * Rewrite the number the engine is about to use, from inside any hook that offers one (XP gained, gold gained, and every future modifiable hook). Only valid inside such a callback — the error names the hook you ARE inside, so a wrong-place call says something useful.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_modify_value(newValue: number): boolean;

  /**
   * Make a monster swing immediately, using whatever attack pose its current weapon calls for.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_monster_attack(uid: string): boolean;

  /**
   * Make a monster (or a companion) cast a spell along its own facing. Free cast, host-only.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_monster_cast_spell(uid: string, spell: string): number;

  /**
   * Send a monster into a straight-line charge for N ticks (50 = 1 second, default 50, max 500). Aims at its target if it has line of sight, otherwise charges along its current facing.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_monster_charge(uid: string, ticks: number): boolean;

  /**
   * Put an item into a monster's equipment slot. Resolves a custom "ns:item" first and falls back to a vanilla item name. An unknown slot is refused and the valid list is logged.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_monster_equip(uid: number, slot: string, item: string): boolean;

  /**
   * Turn a monster to look at a tile. Aims at the tile centre. Pair it with sam_monster_charge to aim a charge.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_monster_face(uid: string, tileX: number, tileY: number): boolean;

  /**
   * The monster counterpart of sam_has_effect — e.g. react when a monster you just hit is POISONED. Pass a monster UID (from a monster event or sam_get_nearby_entities).
   */
  function sam_monster_has_effect(uid: string, effect: string): boolean;

  /**
   * Reads back what the mod declared in JSON. Without this a mod can SAY a monster is undead and the engine agrees, but the mod's own script cannot ask — so a "bonus vs undead" rule had no way to test for undead. False for every vanilla monster, so it is a no-op without a mod.
   */
  function sam_monster_has_trait(uid: string, trait: string): boolean;

  /**
   * Path a monster to a tile using the engine's real pathfinder, then put it in the hunt state so it walks there. Tile coordinates, matching sam_get_position. Returns false when the destination is unreachable.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_monster_path_to(uid: string, tileX: number, tileY: number): boolean;

  /**
   * Empty one of a monster's equipment slots. Pairs with sam_monster_equip for disarm effects and for swapping a creature's loadout mid-fight.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_monster_unequip(uid: number, slot: string): boolean;

  /**
   * Override a class's STARTING stats/skills (patch = { STR, DEX, ..., MAXHP, skills = {...} }). Per-machine — call on every peer in multiplayer; reverts on unload.
   */
  function sam_patch_class(class_: string, patch: any): boolean;

  /**
   * Override an item type's base fields live: { weight, value/gold_value, level, category, slot, tooltip, name/name_identified, name_unidentified, attributes = {...} }.
   */
  function sam_patch_item(item: string, patch: any): boolean;

  /**
   * Override a monster type's base stats (e.g. { HP, MAXHP, STR }) for future spawns; also zero RANDOM_* for exact values.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_patch_monster(monster: string, patch: any): boolean;

  /**
   * Play a sound for all connected players. sound_id is a vanilla numeric index OR the "namespace:sound" id of a custom sound bundled in the mod. vol 0-255 (default 128).
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_play_sound(sound_id: number, vol: number): boolean;

  /**
   * Positional audio: it attenuates with distance and pans, so a trap firing across the level is quiet, and in co-op each player hears it from where THEY are.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_play_sound_at(sound: number, tileX: number, tileY: number, volume: number): boolean;

  /**
   * The same, but the sound follows the entity as it moves.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_play_sound_entity(sound: number, uid: string, volume: number): boolean;

  /**
   * How many players are actually connected right now.
   */
  function sam_player_count(): number;

  /**
   * Check whether a player already knows a spell (vanilla or custom).
   */
  function sam_player_knows_spell(player: number, spell: string): boolean;

  /**
   * Power a mechanism on or off, as a switch wired to it would.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_power_entity(uid: string, on: boolean): boolean;

  /**
   * Deterministic random drawn from a named stream owned by your mod. Same run seed plus same stream plus same draw order gives the same number on every machine, which ordinary random() cannot promise. Use it for anything that must agree across a multiplayer party.
   */
  function sam_random(stream: string, min: number, max: number): number;

  /**
   * Give a name to a function that will BE an entity's brain. Barony runs every entity through a function pointer once per frame; this puts yours behind one. Your function is called with the entity's uid, once per frame, for every entity you spawned with that behaviour — and everything else in this reference is available inside it, so it can look around, move, shoot, damage, or open a window. Nothing about what it does comes from a list. Register at the top of your script rather than inside a handler, so the name exists before you spawn anything with it. Registering the same name twice replaces the function, and entities already in the world follow the new code. Behaviours are dropped when mods reload.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_register_behavior(name: string, fn: string): boolean;

  /**
   * Declare a namespaced custom hook. Name must contain a colon ("namespace:hook_name").
   */
  function sam_register_hook(name: string): void;

  /**
   * Remove a class passive effect previously added.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_remove_class_passive(class_: string, effect: string): boolean;

  /**
   * Clear a status effect from a player.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_remove_effect(player: number, effect: string): boolean;

  /**
   * Remove a non-player world entity by uid — a sam_spawn_portal marker, a spawned monster, a companion, a ground item, etc. Refuses players (use the normal death/teleport paths for those). Frees any light the entity owned.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_remove_entity(uid: number): boolean;

  /**
   * Remove a whole item stack from a player's inventory by its uid (from sam_get_inventory). Refuses an equipped item — unequip it first.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_remove_item(item_uid: number): boolean;

  /**
   * Clear a status effect from a monster by UID — the monster counterpart of sam_remove_effect.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_remove_monster_effect(uid: string, effect: string): boolean;

  /**
   * Un-learn a spell from a player's known list (local player). The counterpart to sam_grant_spell.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_remove_spell(player: number, spell: string): boolean;

  /**
   * Persist a value (number/string/bool/table) for the calling mod under savegames/sam_mod_data/<ns>/.
   */
  function sam_save_data(key: string, value: string): boolean;

  /**
   * Flash a player's whole screen in an RGB colour that fades to nothing — the anime "impact frame". intensity 0..1 is the peak opacity. Drawn on the machine the player lives on.
   */
  function sam_screen_flash(player: number, r: number, g: number, b: number, intensity: number, duration_ms: number): boolean;

  /**
   * Send a mod-defined message to another machine. Barony's packet ids are a fixed table, so before this a co-op mod had no way to tell the other side anything at all. On a client the target is ignored and the packet always goes to the host. The other side receives an "on_packet" event with .from, .tag and .payload. One datagram only — use sam_save_data for bulk state.
   */
  function sam_send_packet(target: number, tag: string, payload: string): boolean;

  /**
   * Turn an existing chest into permanent storage. Its contents then live in the player's savegame instead of on the floor, surviving descending, dying later, quitting and loading. This is the game's own void-chest storage, so the window, the networking and the save round-trip are all vanilla. Two limits worth designing around: every stash chest in a run shares ONE set of contents, and the chest window holds 12 stacks — so this is a stash, not a bank. Converting a chest that already holds loot hides that loot until you turn the stash back off; prefer converting an empty one.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_chest_stash(chest_uid: number, on?: boolean): boolean;

  /**
   * Open or close a door. Find one with sam_find_entities(x, y, r, "door").
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_door(uid: string, open: boolean): boolean;

  /**
   * Lock or unlock a door.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_door_locked(uid: string, locked: boolean): boolean;

  /**
   * Retime an ALREADY-ACTIVE effect in place, without re-triggering it. No-op if the effect isn't active (never spawns a fresh one). 50 ticks = 1s.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_effect_duration(player: number, effect: string, ticks: number): boolean;

  /**
   * Change the magnitude/tier of an ALREADY-ACTIVE effect while keeping its remaining duration. strength 1-255.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_effect_strength(player: number, effect: string, strength: number): boolean;

  /**
   * Point an entity at an angle. The primitive under sam_look_at, for when you are computing a direction yourself — a sweep, a spin, a lead on a moving target. The angle is normalised, so a behaviour that keeps adding to it will not drift out of range.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_entity_facing(uid: number, radians: number): boolean;

  /**
   * Swap any entity's model while the game is running. What crosses the wire is the model ID, never an index, so machines with different mod orders still agree. This is what makes transformations, boss phases and damage states possible; before it, a model was fixed at spawn.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_model(uid: number, model_id: string): boolean;

  /**
   * Store any primitive/table value in a monster's scratch store (JSON-marshaled).
   */
  function sam_set_monster_data(uid: string, key: string, value: string): boolean;

  /**
   * Rename a living monster. The name is what the player sees when targeting it and what appears in the obituary, so this is how a scripted boss or a named rare gets its title.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_monster_name(uid: number, name: string): boolean;

  /**
   * Set a monster's stat by UID (bounded).
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_monster_stat(uid: string, stat: string, value: number): boolean;

  /**
   * Make a monster acquire a player as its attack target.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_monster_target(uid: string, player: number): boolean;

  /**
   * Set a player's move-speed multiplier, clamped to [0.1, 3.0]. Host-only; syncs to the owning client. 1.0 is normal speed.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_move_speed(player: number, mult: number): boolean;

  /**
   * Store a per-player value (number/string/bool/table) in memory for THIS session — the right tool for cooldowns, ability flags and stack counters you read often. Unlike sam_save_data it never touches disk and is cleared on a new game.
   */
  function sam_set_player_data(player: number, key: string, value: string): void;

  /**
   * Move an entity to a map tile. Players go through the safe teleport path (can't tunnel into walls); other entities are relocated and re-broadcast to clients.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_position(uid: number, tile_x: number, tile_y: number): boolean;

  /**
   * Run `callback` (a function) every interval_ticks until cancelled. Ticks host-side.
   */
  function sam_set_repeating_timer(id: string, interval_ticks: number, callback: string): void;

  /**
   * Scale an entity. Clamped at 1.99 with a logged warning, because Barony quantises scale on the wire in 1/128 steps with a cap just under 2 — a larger value would look right to you and be invisible to everyone else.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_scale(uid: number, scale: number): boolean;

  /**
   * Set a live player stat, bounded (HP never exceeds MAXHP, stats clamped, etc.). Syncs the change to the owning client.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_stat(player: number, stat: string, value: number): boolean;

  /**
   * Write one map tile — dig a passage, wall something in, flood a room. Refuses out of bounds rather than corrupting the map array. Check sam_tiles_connected afterwards if the edit could seal the exit.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_tile(x: number, y: number, layer: number, tileId: number): boolean;

  /**
   * Run `callback` (a function) once after delay_ticks (50/sec). Replaces any timer with the same id. Ticks host-side.
   */
  function sam_set_timer(id: string, delay_ticks: number, callback: string): void;

  /**
   * Show or hide an entity. Refused, with a logged reason, on an entity that has a custom body: the draw pass deliberately keeps those visible, so hiding one this way would not work consistently. Clear the model first, or move it out of sight.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_set_visible(uid: number, visible?: boolean): boolean;

  /**
   * Cover a player's screen with one of the mod's pictures, over the world AND the HUD, for duration_ms (0 or omitted = until sam_hide_image). This is the jumpscare / title-card / death-splash layer: it removes itself, so there is nothing to clean up. alpha is 0..255 (default 255). "contain" keeps the picture's aspect ratio; "stretch" (default) fills the view. In multiplayer the host forwards the image NAME to the owning client, which draws it from its own copy of the mod.
   */
  function sam_show_image(player: number, image: string, duration_ms: number, alpha: number, fit: string): boolean;

  /**
   * The same overlay, placed rather than full-screen. Coordinates are virtual screen pixels (the space sam_hud_text uses), so a fixed layout survives any resolution. w or h of 0 means the picture's own size on that axis. Still drawn over the HUD — for a picture that sits IN the HUD, use sam_hud_image.
   */
  function sam_show_image_at(player: number, image: string, x: number, y: number, w: number, h: number, duration_ms: number, alpha: number): boolean;

  /**
   * Spawn a floating COMPANION (a JoJo-style "Stand" / familiar) that renders one of your custom .vox models and trails the player a short distance behind, with a gentle hover. Follows the player every frame and faces where they face. Optional scale (default 1.0, capped at 8) sizes the model. Drive the punch motion with sam_companion_punch, and clear it with sam_remove_entity. It's a decorative follower (PASSABLE, no AI, does no damage on its own — pair it with sam_cast_spell / sam_deal_damage for the actual attack). Host-only; not network-synced (host renders it). Re-spawn it on each new floor (entities are cleared on descent).
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_spawn_companion(player: number, model_id: string, scale: number): number;

  /**
   * Put something in the world that runs your behaviour. This is the other half of sam_register_behavior: that one supplies the code, this gives it a body. The entity starts passable with no collision of its own, because your behaviour decides what it collides with. Leave model empty and it is invisible, which is almost never what you want. Host-only.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_spawn_entity(tile_x: number, tile_y: number, behaviour: string, model?: string): number;

  /**
   * Spawn a ground item at a map tile. status, beatitude and count let you put an item back exactly as you found it — without them a stash could record that you owned a cursed, worn ring and then only ever hand back a pristine one. The uid comes back so you can move it (sam_set_position) or clear it (sam_remove_entity) later; a uid is never 0, so an older `if sam_spawn_item(...)` check still behaves as it did.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_spawn_item(x: number, y: number, item_name: string, status?: number, beatitude?: number, count?: number): number;

  /**
   * Summon a monster at a map tile. "shopkeeper" makes a working shop; the optional shop_type (0-14) picks the store kind.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_spawn_monster(tile_x: number, tile_y: number, monster_name: string, shop_type: number): number;

  /**
   * Spawn `count` (1-8) monsters of a type near an anchor entity's UID.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_spawn_monsters(near_uid: string, monster_type: string, count: number): number;

  /**
   * A vanilla particle burst at a tile, so a mod's own effect looks like part of the game.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_spawn_particle(kind: string, tileX: number, tileY: number, z: number, scale: number): boolean;

  /**
   * Spawn a purely-DECORATIVE portal (the swirling vortex) at a map tile — it animates and glows but is never interactive and never sends anyone to the next floor. Walkable, so a player can stand on it. Returns the uid so you can move it (sam_set_position) or clear it (sam_remove_entity) — e.g. a portal-gun marker. Host-only. Multiplayer: the portal is host-authoritative and NOT network-synced, so only the host renders it — connected clients won't see it (your teleport/logic still runs host-side).
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_spawn_portal(tile_x: number, tile_y: number): number;

  /**
   * Fire a moving projectile with its own speed, model, damage and lifetime. Until this the only thing a script could launch was a fixed vanilla spell, which ruled out ranged enemies with real attack patterns, telegraphed boss volleys, and weapons that fire anything but an arrow. It stops on the first thing it hits and fires an "on_projectile_hit" event with .projectile, .target, .x, .y and .damage — spawn a follow-up there for a burst or an explosion. Giving an owner stops the shot killing the player who fired it on its first frame. Leave model empty and the projectile is INVISIBLE, which is almost never what you want. Host-only, like every other world-mutating call. MULTIPLAYER: everything that matters is decided on the host, so damage, collisions and the hit event are correct for everyone — but a connected client has no behaviour for a custom projectile and only moves it when a position update arrives, about 8 times a second, so the flight looks stepped rather than smooth on their screen. Fine for a shot that crosses a room; noticeable on a slow, long-lived one.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_spawn_projectile(tile_x: number, tile_y: number, angle: number, speed: number, damage?: number, lifetime?: number, model?: string, owner?: number): number;

  /**
   * Can something WALK (or fly) from A to B at all? The softlock check: after a mod edits terrain, ask whether the exit is still reachable before committing.
   */
  function sam_tiles_connected(x1: number, y1: number, x2: number, y2: number, flying: boolean): boolean;

  /**
   * Flip a lever or switch, driving whatever it is wired to.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_toggle_switch(uid: string): boolean;

  /**
   * Send the party to any floor, including BACK UP, which the game otherwise never does — a ladder only ever counts upward, so before this no hub, home base or shop you walk back to was possible. The trip is deferred exactly as a ladder defers it, so it is safe to call from inside an event handler. Refused, with a logged reason, on a client, while another level change is already under way, or before a game has started. Nothing on the old floor is preserved: floors regenerate from the map seed, so put anything that must survive in a stash chest or in sam_world_save.
   *
   * Host-only: refused on a multiplayer client.
   */
  function sam_travel_to_level(floor: number, opts?: any): boolean;

  /**
   * Put a clickable button in a panel. Clicking it fires a "ui.on_click" event whose .panel and .widget match what you passed here, so one handler can serve every button by switching on .widget. The panel must have been opened with modal = true or the player will have no cursor to click with.
   */
  function sam_ui_button(panel: string, id: string, x: number, y: number, w: number, h: number, text: string): boolean;

  /**
   * Remove every widget from a panel but leave the panel itself open. This is how you rebuild a changing screen — clear, then re-declare the rows — without the window flickering shut and open again.
   */
  function sam_ui_clear(panel: string): boolean;

  /**
   * Close one panel, or every panel your mod has open if you pass nothing. Closing the last modal panel restores the player's camera control. Always close your panels on player.on_death and game.on_game_start so a leftover window cannot follow the player into the next run.
   */
  function sam_ui_close(panel?: string): boolean;

  /**
   * Change the font of one widget, or of an entire panel by passing an empty id -- which is the only way to restyle a panel's text in one call rather than widget by widget. Panels default to a small 16px face because the game's standard 32px font makes any list look enormous. The number after the first # is the pixel size — raise it for a heading, and raise the row height to match if it is a list.
   */
  function sam_ui_font(panel: string, id: string, font: string): boolean;

  /**
   * Put one of your mod's pictures in a panel, scaled to w by h. Resolves the same way sam_show_image does. The colour argument tints the picture and its alpha fades it, so the same file can be reused greyed-out for a locked entry.
   */
  function sam_ui_image(panel: string, id: string, x: number, y: number, w: number, h: number, image: string, color?: string): boolean;

  /**
   * Put an editable text box in a panel — a search field, a name entry, a price offer. Read what the player typed with sam_ui_input_text. Place the box clear of any label: a label wide enough to overlap the box will sit on top of it.
   */
  function sam_ui_input(panel: string, id: string, x: number, y: number, w: number, h: number, text?: string): boolean;

  /**
   * Read what the player has typed into one of your text boxes. Poll it from a button handler, or from on_tick if you want a search list to filter as they type. Pressing Enter in the box also fires "ui.on_submit" with the text in .value.
   */
  function sam_ui_input_text(panel: string, id: string): string;

  /**
   * Ask whether one of your panels is on screen. Useful to make a key or an item toggle a window instead of re-opening it, and to skip expensive refresh work while it is closed.
   */
  function sam_ui_is_open(panel: string): boolean;

  /**
   * Put a line of text in a panel. x/y are measured from the panel's top-left corner, not the screen. Give w enough room for the text or it will be cut off — sam_ui_text_size measures a string before you place it. Re-declaring the same id replaces the text, which is how you update a running total.
   */
  function sam_ui_label(panel: string, id: string, x: number, y: number, w: number, text: string, color?: string): boolean;

  /**
   * Create an empty scrolling list in a panel. Fill it with sam_ui_list_add. This is the widget for a shop's stock, a bestiary, a recipe index or a quest log — anything with more entries than fit on screen.
   */
  function sam_ui_list(panel: string, id: string, x: number, y: number, w: number, h: number): boolean;

  /**
   * Append one row to a list. Clicking a row fires "ui.on_select" with .panel, .widget set to the list and .value set to the row_id you chose here — so make row_id something you can act on, like an item id, rather than a display string.
   */
  function sam_ui_list_add(panel: string, id: string, row_id: string, text: string, color?: string): boolean;

  /**
   * Empty one list without touching the rest of the panel. Use this before re-filling a list from a search box or a filter, so the old results do not pile up under the new ones.
   */
  function sam_ui_list_clear(panel: string, id: string): boolean;

  /**
   * Set how tall each row of a list is. Raise it if you switched that list to a larger font, or rows will overlap.
   */
  function sam_ui_list_row_height(panel: string, id: string, pixels: number): boolean;

  /**
   * Open one of your mod's panels at a position and size given in VIRTUAL screen units (1280x720 at the default UI scale, not your monitor's pixels). modal = true frees the mouse cursor so the player can click your widgets, and hands camera control back when the panel closes — use it for anything with buttons. A non-modal panel is display-only and leaves the player in normal look-around mode. Opening a panel id that is already open re-positions it instead of opening a second one.
   */
  function sam_ui_open(panel: string, x: number, y: number, w: number, h: number, title?: string, modal?: boolean): boolean;

  /**
   * Recolour a panel's background and border. Nothing about a panel's look is fixed by the framework — set the background fully transparent for a bare overlay, or opaque for a solid window. Colours accept the same forms as the HUD calls.
   */
  function sam_ui_panel_style(panel: string, background: string, border: string, border_width?: number): boolean;

  /**
   * Measure a string before you place it. This is how you lay a panel out properly instead of guessing: size a label to its own text so it cannot overlap the widget beside it, right-align a column of numbers, or centre a heading in a panel of known width.
   */
  function sam_ui_text_size(text: string, font?: string): number;

  /**
   * Revert a class stat/skill patch.
   */
  function sam_unpatch_class(class_: string): boolean;

  /**
   * Forget one key for the current character. The whole store is dropped automatically when a run ends, so you only need this to reset something mid-run.
   */
  function sam_world_clear(key: string): boolean;

  /**
   * List the keys your mod has saved for this character. Handy for migrating an older save's data, or for showing the player what a mod is remembering about their run.
   */
  function sam_world_keys(): any[];

  /**
   * Read a value back from the current character's savegame. nil on a key you have never written is the signal that this is a fresh character — that is the natural place to run first-time setup, like anchoring a home floor.
   */
  function sam_world_load(key: string): any;

  /**
   * Save a value inside the CURRENT character's savegame. A brand new character starts with none of it, so a hub's unlock flags, a quest's progress or a bank balance cannot leak from one run into the next. Deliberately size-capped (8KB per value, 64KB across all mods) because oversized save data can produce a savegame that fails to load — keep flags and counters here and keep items in a stash chest, which the game persists properly on its own.
   */
  function sam_world_save(key: string, value: string): boolean;

  /** Every event name the engine fires. */
  type SamEventName =
    | "<namespace>:<hook_name>"
    | "game.on_game_end"
    | "game.on_game_start"
    | "game.on_level_entered"
    | "on_action_pressed"
    | "on_action_released"
    | "on_before_damage"
    | "on_before_monster_damage"
    | "on_key_pressed"
    | "on_key_released"
    | "on_monster_damaged"
    | "on_monster_died"
    | "on_projectile_hit"
    | "on_tick"
    | "player.on_attack_start"
    | "player.on_became_ghost"
    | "player.on_before_equip"
    | "player.on_before_item_pickup"
    | "player.on_before_revive"
    | "player.on_bleed_tick"
    | "player.on_block"
    | "player.on_callout"
    | "player.on_chest_opened"
    | "player.on_damage_taken"
    | "player.on_death"
    | "player.on_effect_applied"
    | "player.on_effect_expired"
    | "player.on_effect_removed"
    | "player.on_equip"
    | "player.on_floor_change"
    | "player.on_game_over"
    | "player.on_gold_collected"
    | "player.on_hit"
    | "player.on_hunger_change"
    | "player.on_item_bought"
    | "player.on_item_broken"
    | "player.on_item_dropped"
    | "player.on_item_identified"
    | "player.on_item_pickup"
    | "player.on_item_sold"
    | "player.on_item_use"
    | "player.on_kill"
    | "player.on_level_up"
    | "player.on_miss"
    | "player.on_player_joined"
    | "player.on_player_left"
    | "player.on_player_revived"
    | "player.on_poison_tick"
    | "player.on_proficiency_increased"
    | "player.on_shop_entered"
    | "player.on_spell_cast"
    | "player.on_spell_failed"
    | "player.on_spell_learned"
    | "player.on_status_effect_tick"
    | "player.on_unequip"
    | "player.on_xp_gained"
    | "ui.on_click"
    | "ui.on_select"
    | "ui.on_submit"
    | "world.on_before_chest_open"
    | "world.on_boulder_triggered"
    | "world.on_chest_found"
    | "world.on_door_opened"
    | "world.on_fountain_used"
    | "world.on_item_deployed"
    | "world.on_monster_spawned"
    | "world.on_orb_placed"
    | "world.on_projectile_hit"
    | "world.on_sink_used"
    | "world.on_switch_toggled"
    | "world.on_teleport"
    | "world.on_trap_triggered";

  interface SamEvent {
    name: SamEventName;
    [field: string]: any;
  }
}

export {};
