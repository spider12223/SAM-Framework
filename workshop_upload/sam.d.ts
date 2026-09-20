// TypeScript definitions for the S.A.M Framework scripting API.
// Generated from the API definition; do not edit by hand.
//
// Drop this beside your mod's .ts files, or reference it:
//   /// <reference path="sam.d.ts" />
//
// 316 functions, 76 events. Each function's "Multiplayer:" line starts with
// its kind (host, owner, screen, read, all, local, any); see docs/multiplayer.md.

declare global {
  /**
   * Grant a class a permanent status effect at character creation (bakes at creation; run at mod-load).
   *
   * Multiplayer: all. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.
   */
  function sam_add_class_passive(class_: any, effect: any): boolean;

  /**
   * Contribute to the damage multiplier for the hit currently being resolved. 0.25 is +25%, -0.5 is half.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_add_damage_multiplier(fraction: number): boolean;

  /**
   * The same for one creature.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_add_monster_stat_modifier(uid: number, stat: string, id: string, add?: number, multiply?: number): boolean;

  /**
   * Add to a player's move-speed multiplier (the result is clamped to [0.1, 3.0]). Additive counterpart to sam_set_move_speed: use it to stack a bonus onto whatever the multiplier already is (e.g. +0.1 on top of a 2.0 from another ability).
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript. Reaches every S.A.M player's machine in order, like sam_set_move_speed. A player whose game does not run S.A.M still walks at vanilla speed.
   */
  function sam_add_move_speed(player: number, delta: number): number | undefined;

  /**
   * Contribute to one of a player's computed stats. Adds are summed and multipliers multiplied ACROSS EVERY MOD, then applied as (base + adds) * multipliers — so two mods each giving +2 STR give +4, and two each halving give a quarter. Neither mod has to know the other exists. SPEED takes a multiplier only (add must be 0): a player's speed and a monster's are scaled from different bases, so an add would mean different things on each. Add is limited to +-10000 and multiply to 100.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_add_stat_modifier(player: number, stat: string, id: string, add?: number, multiply?: number): boolean;

  /**
   * Wake every ally near this monster onto an attacker, the way the engine does when something is hit in a room full of its friends. The attacker may be left out for "alerted by nothing in particular".
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_alert_allies(uid: number, attacker_uid?: number): boolean;

  /**
   * Apply a status effect to a player for N ticks (50 ticks = 1s). Optional strength sets the tier/magnitude for effects that carry one (e.g. GROWTH stacks) — omit it for the plain default. Targets the player, never a monster.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_apply_effect(player: number, effect: string, ticks: number, strength?: number): boolean;

  /**
   * Shove an entity, using the engine's own knockback. The angle is a Barony yaw in radians, the same number sam_get_facing gives you, so away-from-you is atan2(theirY - myY, theirX - myX). This is deliberately not a raw velocity write: both act functions throw velocity away unless the knockback effect is active, and a player takes the impulse in a completely different field from a monster, so a hand-written version does nothing at all to the two targets you would actually aim it at. The optional ticks is how long the stagger lasts and defaults to 30, which is what the engine uses. Force is capped at 7 because a single step bigger than that can jump clean over a wall instead of hitting it. Returns false, with a logged reason, for a creature that refuses knockback outright: liches, minotaurs, the devil and shopkeepers are immune, and the engine's own knockback does nothing to them either. It also returns false for anything whose behaviour never reads velocity at all, which includes the decorative portals sam_spawn_portal creates: use sam_move_entity on those. The angle is wrapped into 0 to 2 pi for you, because the network carries it as a fixed-point number that overflows past about 128 radians.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A resting gold bag moves too (it used to do nothing on any machine), and a shoved item or bag slides on every player's screen and ends where the host's copy comes to rest.
   */
  function sam_apply_force(uid: number, force: number, angle: number, ticks?: number): boolean;

  /**
   * Apply a status effect to a monster by UID for N ticks.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_apply_monster_effect(uid: number, effect: string, ticks: number): boolean;

  /**
   * Attach one of your registered behaviours to a live monster. It runs AFTER vanilla AI each frame rather than replacing it, so the creature still fights and paths normally and your code layers on top.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_attach_behavior(uid: number, behavior: string): boolean;

  /**
   * Degrade a worn piece of armour, possibly breaking it. With no slot named, the engine's own picker chooses, so the odds and the exclusions match a real hit. player.on_item_broken has existed as an event with no verb able to cause it.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_break_armor(entity_uid: number, slot?: string): boolean;

  /**
   * Shake a player's camera. 1 is a nudge, ~10 a solid hit, 20+ violent. Feeds Barony's own shake channels so it decays naturally; for a remote client the host forwards it. In JavaScript player and magnitude are required.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A shake for a player on another machine reaches their screen; nothing is sent to an empty player slot.
   */
  function sam_camera_shake(player: number, magnitude: number): boolean;

  /**
   * Whether two items would actually combine, using the game's own comparison rather than a guess at it, so things that never stack (readable books, for instance) correctly answer false. Passing the same item twice is false.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). Returns nil, not false, when either uid names no item this machine can see, because false here means the two would not merge.
   */
  function sam_can_items_stack(player: number, uid_a: number, uid_b: number): boolean | undefined;

  /**
   * Ask whether THIS entity could stand on that tile. Different from sam_is_spawnable, which only reads the map and cannot see other entities or the asker's own collision profile: levitation, body size and the pass-through set all change the answer. Check with this before sam_set_position instead of dropping a monster inside a wall. True is necessary but not sufficient for a player teleport, which applies extra rules of its own (no teleporting on the minotaur levels, and MFLAG_DISABLETELEPORT maps).
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). The entity grid this test reads exists only on the host, so a client's call is refused with a warning and returns nil ("cannot answer here") rather than a confident false.
   */
  function sam_can_stand(uid: number, tile_x: number, tile_y: number): boolean | undefined;

  /**
   * Check before promising the player a swap, so a cursed item does not silently refuse halfway through what your mod said it would do.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). Returns nil, not false, when the uid names no item this machine can see, because false here means cursed and cannot come off.
   */
  function sam_can_unequip(player: number, uid: number): boolean | undefined;

  /**
   * Cancel a pending timer by id (for the calling mod).
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_cancel_timer(id: string): void;

  /**
   * Immediately FIRE a spell/bolt from a player in the direction they face (free, no mana). Great for 'shoot on block'. Accepts the number sam_get_tome_spell returns, like sam_cast_spell_at and sam_cast_spell_pos. Don't call from an on_spell_cast handler.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_cast_spell(player: number, spell: string): boolean;

  /**
   * Fire a spell AIMED at an entity (aims the bolt toward it) instead of straight ahead. Free cast, host-only.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_cast_spell_at(player: number, target_uid: number, spell: string): number | undefined;

  /**
   * Fire a spell aimed at a map tile. Free cast, host-only.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_cast_spell_pos(player: number, tile_x: number, tile_y: number, spell: string): number | undefined;

  /**
   * Strip EVERY active status effect from a player at once — buffs and debuffs, vanilla and custom.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.
   */
  function sam_clear_effects(player: number): number;

  /**
   * Drop every immunity YOUR MOD declared, per player, per creature and per species.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.
   */
  function sam_clear_immunities(): number;

  /**
   * Drop a script-set model and go back to whatever the entity would otherwise draw. Clients are told too, so a transformation can end cleanly.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Reaches every S.A.M player in order, so the entity goes back to its original look on their screens too.
   */
  function sam_clear_model(uid: number): boolean;

  /**
   * Make a monster forget its current target.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_clear_monster_target(uid: number, force?: boolean): boolean;

  /**
   * Undo sam_set_species_damage_resist. No arguments clears everything, a species alone clears every type for it.
   *
   * Multiplayer: all. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns 0.
   */
  function sam_clear_species_damage_resist(species?: string, type?: string): number;

  /**
   * Take back every modifier YOUR MOD made, from one player, or with no argument from every player and every monster — which is what a mod's teardown wants. Other mods' contributions are left alone.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.
   */
  function sam_clear_stat_modifiers(player?: number): number;

  /**
   * Take back your mod's XP curve. Levels no other mod has set go back to a flat 100; a level another mod also set keeps that mod's threshold.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.
   */
  function sam_clear_xp_curve(): number;

  /**
   * Make a companion THRUST forward for a few ticks — the punch motion. Call it repeatedly on a fast repeating timer (e.g. every 3 ticks) during an ability to read as a continuous ORA-ORA flurry. Purely visual on the companion itself; combine with sam_cast_spell (forward projectile + real damage) and/or sam_get_nearby_entities + sam_deal_damage for the hits.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. The lunge is animated on every S.A.M player's machine.
   */
  function sam_companion_punch(uid: number): boolean;

  /**
   * Spend mana only if the creature has it. Nothing is taken when it cannot afford the cost, which makes this the right one for a custom ability's cost.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_consume_mp(entity_uid: number, amount: number): boolean;

  /**
   * The floating combat number the game shows on a hit. Lets a mod's custom damage read like real damage instead of being invisible.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_damage_number(uid: number, amount: number, type?: number): boolean;

  /**
   * Deal amount damage to any entity by UID (positive = damage); existence-validated.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_deal_damage(entity_uid: number, amount: number): boolean;

  /**
   * Deal damage of a particular weapon class, so the target's own resistance applies. 10 magic damage is 5 against something that halves magic and 20 against something that doubles it, without your script needing to know which. Both stages the engine applies are applied here, in its order: the species damage table, then live effects such as blood ward and sanctuary.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.
   */
  function sam_deal_damage_typed(entity_uid: number, amount: number, type: string): number;

  /**
   * Delete a persisted per-mod key.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_delete_data(key: string): boolean;

  /**
   * Remove whatever behaviour a script attached to this entity. Safe on a uid that has none.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_detach_behavior(uid: number): boolean;

  /**
   * Take mana, and take anything you cannot afford out of HEALTH instead. That overdraw is the point — it is how a blood-magic cost is expressed.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_drain_mp(entity_uid: number, amount: number, notify?: boolean): boolean;

  /**
   * Entities of a KIND near a tile. This is the gap sam_get_nearby_entities leaves: that one skips anything which is not a monster or a player, so doors, chests, levers, gold and dropped items were invisible to scripts. A kind you spell wrong is logged by name and returns nothing, rather than returning the empty list that looks exactly like "nothing nearby"; each distinct wrong word is reported once. It never returns the engine's shared marker uids (particles, flames, a client's own local effects), so every uid it returns works with the other functions.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Never returns the engine's shared marker uids (particles, flames, a client's own local effects), so every uid it returns can be used with the other functions. Each machine answers from its own copy, which on a client can lag the host.
   */
  function sam_find_entities(x: number, y: number, radiusTiles: number, kind?: string): any;

  /**
   * Fire a custom event to ALL Lua + JS/TS scripts cross-runtime. Only number/bool/string fields cross over; recursion capped at depth 8.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.
   */
  function sam_fire_hook(name: string, event?: any): number;

  /**
   * Armor class as the damage formula sees it, gear included.
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (uid), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_ac(uid: number): number;

  /**
   * What the player actually has an action bound to — use it to print a correct prompt instead of guessing a key.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). On the host, a player on another machine answers with the binding their game reported (nil until it has).
   */
  function sam_get_action_binding(player: number, action: string): string | undefined;

  /**
   * The melee attack figure the engine itself would use for this creature's next swing, weapon and stats included.
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (entity_uid), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_attack(entity_uid: number): number | undefined;

  /**
   * How much extra attack this creature gets against that particular target — slayer enchantments and the like.
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (entity_uid), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_bonus_attack_vs(entity_uid: number, target_uid: number): number | undefined;

  /**
   * Where the camera actually is, in the same tile units the setters take. mode is "vanilla", "orbit" or "absolute".
   *
   * Multiplayer: local. Answers for the machine running the script. Given a player on another machine (player), it is refused with a one-time warning and returns nil (undefined in JavaScript). Returns nil for a player on another machine, and logs it once.
   */
  function sam_get_camera(player: number): any;

  /**
   * Which class a player is, as an identifier you can act on: a custom class's "namespace:class" id, or the vanilla class's own name. Accepted by sam_patch_class, sam_add_class_passive and the rest, so you can read a player's class and then change it. It is not a display string; a custom class returns its id rather than its title.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_class(player: number): string | undefined;

  /**
   * What is inside a chest, or what a creature is carrying.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_container_items(uid: number): any;

  /**
   * How much of a given weapon class this creature actually takes: 1.0 normal, 0.5 half, 2.0 double. Equipment, effects and magic resistance are all included — it is the same call the character sheet makes to draw the number a player sees. Defaults to "magic".
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (entity_uid), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_damage_resist(entity_uid: number, type?: string): number | undefined;

  /**
   * Today's date on this machine, which is how you make content that only appears at Halloween or over Christmas. Per-machine, so treat it as decoration rather than as a rule.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_get_date(): any;

  /**
   * Distance between two entities on the FLOOR PLANE. Height is ignored, so a bat hovering directly overhead reads as zero tiles away. That matches how the game's own range checks work, which is why it is not corrected for here. Use this rather than working it out from sam_get_position, which rounds to whole tiles and so is wrong by up to a tile in each axis.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.
   */
  function sam_get_distance(uid_a: number, uid_b: number): number | undefined;

  /**
   * Distance from an entity to a tile, measured to the centre of that tile, which is where the game places things.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.
   */
  function sam_get_distance_to(uid: number, x: number, y: number): number | undefined;

  /**
   * How many ticks of an effect are left (50 = 1s), so a debuff can scale or decay by time remaining.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). A client does not count effect timers (it only knows whether an effect is on), so its call is refused and returns nil rather than a misleading 0.
   */
  function sam_get_effect_duration(player: number, effect: string): number | undefined;

  /**
   * The effect's strength/magnitude for effects that store one (GROWTH tiers, potion STR).
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_get_effect_strength(player: number, effect: string): number | undefined;

  /**
   * A stat as the game actually uses it — gear, effects and curses folded in — rather than the raw number on the sheet.
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (uid), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_effective_stat(uid: number, stat: string): number;

  /**
   * Every active effect on a player at once: react to "any debuff" or strip all buffs without polling each effect by name. Names match the effect events in Lua and JavaScript alike: the lowercase vanilla name ("poisoned"), a mod's "namespace:effect" id, or "CUSTOM:<id>" for an unnamed custom slot, so a list from here can be compared directly against an event's effect_name.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns an empty table (an empty array in JavaScript). A client does not count effect timers, so the ticks it could give would be wrong; its call is refused.
   */
  function sam_get_effects(player: number): any;

  /**
   * Read which way an entity is pointing. sam_get_facing takes a PLAYER index and reads where that player looks; this takes an entity uid, which is what a behaviour is handed. Feed it straight to sam_spawn_projectile to fire where the thing is aiming. Always in [0, 2π): an entity whose raw yaw was negative used to read as nil.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's copy of the entity. On a client it can lag the host, and a flag the host clears without telling clients can stay set there. Read on the host when the answer matters.
   */
  function sam_get_entity_facing(uid: number): number | undefined;

  /**
   * Read one of Barony's entity flags by name. An unknown name gives you nil, never false: false is a real answer to a question like "is this passable", so handing it back for a typo would send your script down the wrong branch without a word. Readable on a client, but a client's copy of a flag is only as fresh as the last update the host sent about that entity.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's copy of the entity. On a client it can lag the host, and a flag the host clears without telling clients can stay set there. Read on the host when the answer matters.
   */
  function sam_get_entity_flag(uid: number, flag: string): boolean | undefined;

  /**
   * The entity's collision box. Any overlap test or aim cone written in script needs this, and it was not readable before. In JavaScript this returns an array.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.
   */
  function sam_get_entity_size(uid: number): any;

  /**
   * Which model an entity is currently drawing. Pairs with sam_get_model, which reports only a model your own script set.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.
   */
  function sam_get_entity_sprite(uid: number): number | undefined;

  /**
   * Age of an entity in frames. Divide by sam_get_tick_rate for seconds. Useful for despawning your own spawns after a while without keeping a table of them.
   *
   * Multiplayer: local. Answers for the machine running the script. Each machine answers from its own copy. Each machine counts separately, and a client's count starts when that client first received the entity. Use the host's value for timing.
   */
  function sam_get_entity_ticks(uid: number): number | undefined;

  /**
   * What kind of thing a uid refers to. Lets one handler deal with a mixed list of uids without guessing from what other calls happen to succeed. Every word it returns is one sam_find_entities accepts, so you can read a kind and then go looking for more of the same.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.
   */
  function sam_get_entity_type(uid: number): string | undefined;

  /**
   * Get the item NAME equipped in a slot (ARMOR==BREASTPLATE, BOOTS==SHOES). Vanilla items only — it can't name a custom item, so use sam_get_equipped_item_id to test for one. In JavaScript player and slot are required.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). Ask on the host for anyone but yourself: the host's copy of every player's worn items is exact.
   */
  function sam_get_equipped_item(player: number, slot: string): string | undefined;

  /**
   * Get the item ID equipped in a slot. Compare it against sam_item_id("namespace:item") to check whether YOUR custom item is equipped — the id is a number, so the name-returning version above can never match it. In JavaScript player and slot are required.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). Ask on the host for anyone but yourself: the host's copy of every player's worn items is exact.
   */
  function sam_get_equipped_item_id(player: number, slot: string): number | undefined;

  /**
   * Where the ladder or portal off this floor is, found the same way the game's own dowsing does it. In JavaScript this returns an array.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_exit_position(): any;

  /**
   * Read which way a player is looking. 0 = +x (east), increasing toward +y — so the forward unit vector is (cos yaw, sin yaw) and 'behind' is yaw + π. Use it to place things relative to a player's facing (a marker in front, a follower behind) or to aim. Host-authoritative for remote players; a client always sees its own facing correctly.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.
   */
  function sam_get_facing(player: number): number | undefined;

  /**
   * Read a lobby setting the host chose at game start. Lets a mod adapt to the run it is actually in — skip a hunger mechanic when hunger is off, or scale difficulty when hardcore is on.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_flag(flag: string): any;

  /**
   * Get the current floor/dungeon level.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_floor(): number;

  /**
   * How filling a food is. Takes an item TYPE rather than a uid, so you can price food your mod has not spawned yet. Vanilla foods only: the engine's table has no entry for custom items, so your own food answers 0.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_food_satiation(item_type: number): number | undefined;

  /**
   * How fast this machine is drawing. Local to whoever asks, so never let it decide anything shared: two players will get different numbers and their games will disagree.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_get_fps(): number;

  /**
   * The regeneration bonus this creature carries. It is what makes sam_get_regen_interval shorter.
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (entity_uid), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_healring(entity_uid: number): number | undefined;

  /**
   * Read any creature's health by UID.
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (entity_uid), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_hp(entity_uid: number): number | undefined;

  /**
   * The picture's own pixel size, so a script can centre or scale it instead of hard-coding the numbers it was exported at. Also the cheapest way to check a picture actually resolves.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_image_size(image: string): any;

  /**
   * List a player's inventory. Use each item's uid with the item functions and sam_remove_item. name is the vanilla internal name, or a mod item's own "namespace:item" id (in Lua and JavaScript alike), so it can be told apart from other mod items and fed back into sam_grant_item. equipped is true only for the exact item being worn, so a spare identical ring in the bag does not count; it agrees with sam_is_item_equipped.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). On the host, a player on another machine is answered from a copy their own game sends a moment after joining and whenever it changes (at most five times a second). nil means this machine cannot see that player's things: the host before their game has reported, or a player whose game does not run S.A.M. That 'not reported yet' moment comes at the start of EVERY run, not only after a join: the host drops the last character's copy when a new run starts and that player's game sends the new one a moment later, so read another player's backpack or spells from a timer or a later event, never from game.on_game_start. A removal is no more visible than any other change until that next report, so never write 'if they still have it, give the reward' without a flag of your own. The uids a remote player's list gives you work with every item function: readers answer from the host's copy, writers are carried to that player's machine. A remote player's item crosses as a type NUMBER, and a mod's item numbers follow the order that machine's mods loaded, so the host names it from its own table: right when both machines load the same mods in the same order, wrong otherwise. An item whose type this host has no definition for is left out of that player's list entirely rather than guessed at.
   */
  function sam_get_inventory(player: number): any;

  /**
   * Count how many of an item (vanilla or custom name) a player holds.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). On the host, a player on another machine is answered from a copy their own game sends a moment after joining and whenever it changes (at most five times a second). nil means this machine cannot see that player's things: the host before their game has reported, or a player whose game does not run S.A.M. That 'not reported yet' moment comes at the start of EVERY run, not only after a join: the host drops the last character's copy when a new run starts and that player's game sends the new one a moment later, so read another player's backpack or spells from a timer or a later event, never from game.on_game_start. A removal is no more visible than any other change until that next report, so never write 'if they still have it, give the reward' without a flag of your own.
   */
  function sam_get_inventory_count(player: number, item_name: string): number | undefined;

  /**
   * Everything plain about one item in a single call, rather than a dozen separate getters. The computed values live in their own functions (sam_get_item_name, sam_get_item_value, sam_get_item_weight) because each runs real engine code instead of reading a field.
   *
   * Multiplayer: read. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own.
   */
  function sam_get_item(uid: number): any;

  /**
   * The armour value. Same caveat as sam_get_item_attack: the optional wearer only matters for cursed-item inversion, not for their skill or stats.
   *
   * Multiplayer: read. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own.
   */
  function sam_get_item_ac(uid: number, player?: number): number | undefined;

  /**
   * The weapon's attack value. The optional player is passed to the engine, but it only affects a few special cases such as shapeshifting and cursed-item inversion: it does NOT add that character's skill or strength, so two ordinary humans get the same number. For a real to-hit you still need the character's own stats.
   *
   * Multiplayer: read. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own.
   */
  function sam_get_item_attack(uid: number, player?: number): number | undefined;

  /**
   * The category of an item (WEAPON / ARMOR / GEM / POTION / SCROLL / SPELLBOOK / …). Pass an event's item_type to react by category — e.g. reward the player for identifying any GEM.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_item_category(item: any): string | undefined;

  /**
   * Look up one item by type number or by name. The attributes sub-table is where a tooltip's numbers come from (ATK, AC and so on), so this is enough to render your own item description in a panel.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_item_info(item: number): any;

  /**
   * The item's name, using the alias an unidentified item shows rather than its true name. Note this is the bare name: the blessed, cursed and condition wording the player sees in the tooltip is added separately by the game and is not included here.
   *
   * Multiplayer: read. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own.
   */
  function sam_get_item_name(uid: number): string | undefined;

  /**
   * Who this item belongs to. It is what the shopkeeper's theft rules read, so it is also how a mod knows whether something was taken rather than bought. An item nobody owns returns nil rather than zero.
   *
   * Multiplayer: read. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own. Returns false, not nil, when the uid names no item this machine can see, because nil already means nobody owns it. On the host it reads a remote player's items from the copy their game reports.
   */
  function sam_get_item_owner(uid: number): number | undefined;

  /**
   * Where this kind of item is worn. Works for custom items too, so an auto-equip mod does not need its own table.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_item_slot(item_type: number): any;

  /**
   * What this pile is worth: the engine's per-item value multiplied by how many you have. Blessing and condition are not part of it, because the engine's own gold value ignores them too; the shop applies those separately when it quotes a price.
   *
   * Multiplayer: read. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own.
   */
  function sam_get_item_value(uid: number): number | undefined;

  /**
   * Weight of the whole stack, with the engine's quiver rule applied. Add these up across sam_get_inventory for a carried total.
   *
   * Multiplayer: read. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own.
   */
  function sam_get_item_weight(uid: number): number | undefined;

  /**
   * Get the SAM-tracked per-player kill count for this session.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_get_kills(player: number): number | undefined;

  /**
   * Everything about the current floor. sam_get_floor returns a bare number that cannot tell a secret branch from the main one, so location-gated content was impossible before this.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_level_info(): any;

  /**
   * How lit a tile is, computed exactly the way the engine computes it, so the number you get back is the number monster vision thresholds on rather than an approximation of it. Barony keeps one SHARED lightmap holding light that is there for everyone (a wall torch, a lit room) plus one per camera that also holds that player's own glow. This reads the shared one by default, because that is the one the AI reads. Pass a player index if you want what that player's screen actually shows instead.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript. The shared lightmap the monster AI reads exists only on the host, so a client's call is refused with a warning. It answers with nothing at all rather than 0, because 0 is a real light level: pitch darkness.
   */
  function sam_get_light_at(x: number, y: number, player?: number): number | undefined;

  /**
   * The raw magic-resistance point count. Each point is a separate reduction: this is the input, sam_get_damage_resist(uid, "magic") is the result.
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (entity_uid), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_magic_resist(entity_uid: number): number | undefined;

  /**
   * The rules this particular map sets. Worth checking before a mod grants levitation or teleports someone, because a map that forbids it will simply undo your effect and the player will not know why.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_map_flags(): any;

  /**
   * The per-floor generation seed, which is the same number on the host and on every client because a client rebuilds the floor from it. Distinct from sam_get_seed, which identifies the whole run.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_map_seed(): number;

  /**
   * Read any creature's maximum health by UID.
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (entity_uid), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_max_hp(entity_uid: number): number | undefined;

  /**
   * Read any creature's maximum mana by UID.
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (entity_uid), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_max_mp(entity_uid: number): number | undefined;

  /**
   * The ceiling for this item and this player, which differs for arrows, thrown gems and scrap. Read it before writing a count.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_max_stack(player: number, uid: number): number | undefined;

  /**
   * Read back the model ID a script set on this entity, or the mod model a spawned entity was created with. Returns nil for an entity drawing its ordinary model.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Answers the same on every machine, because the model crosses the network by name. One exception: a player whose game never received the announcement -- it joined while the host already had more than 512 calls waiting for it, or it is not running S.A.M -- answers nil there. The host writes a line to the log when it has to refuse an announcement, so this is visible rather than silent.
   */
  function sam_get_model(uid: number): string | undefined;

  /**
   * Every S.A.M mod loaded right now. Cross-mod integration with zero engine work: soft-depend on another mod, avoid double registering, or light up extra content when a partner mod is present.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_get_mods(): any;

  /**
   * Read per-monster scratch data (boss phases, etc.); in-memory, cleared on shutdown.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_monster_data(uid: number, key: string): any;

  /**
   * How many ticks of an effect a monster has left.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_monster_effect_duration(uid: number, effect: string): number | undefined;

  /**
   * A monster effect's strength/magnitude.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_get_monster_effect_strength(uid: number, effect: string): number | undefined;

  /**
   * Every active effect on a monster at once. Custom slots appear under the id you declared them with, like "mymod:frostbite", and vanilla ones under the lowercase name the effect events use.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns an empty table (an empty array in JavaScript). In Lua the names now match JavaScript and the effect events: lowercase for a vanilla effect, the mod's 'ns:effect' for a custom one, and 'CUSTOM:<id>' for an unnamed slot. A Lua script comparing against 'POISONED' must change.
   */
  function sam_get_monster_effects(uid: number): any;

  /**
   * For a mod's custom monster this is the variant name it was given ("Rathalos"). A plain vanilla creature carries an empty variant name, so this falls back to the species name and never hands a script an empty string. DISPLAY only: a named creature answers with its own epithet (a shopkeeper is "Adrian"), and no call takes that back. To clone or patch what you are looking at, use sam_get_monster_type, which returns the species name that sam_spawn_monster and sam_patch_monster accept.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_monster_name(uid: number): string | undefined;

  /**
   * Read a monster's stat by UID. DEX aliases SPEED.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_get_monster_stat(uid: number, stat: string): number | undefined;

  /**
   * Get the player index a monster is currently targeting (if any).
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). A client's call returns nil, never 0, because 0 would read as "hunting the host".
   */
  function sam_get_monster_target(uid: number): number | undefined;

  /**
   * Read back what a monster is currently hunting, as a uid, whether that is a player, another monster or anything else with a body.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_get_monster_target_uid(uid: number): number | undefined;

  /**
   * Identify a creature by name instead of the raw integer in an event payload. NOTE this is the BASE type: a custom monster is a variant of a vanilla species, so a mod's "Rathalos" built on a bat answers "bat". Use sam_get_monster_name for the variant's own name, or sam_monster_has_trait to tell modded creatures apart.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_monster_type(uid: number): string | undefined;

  /**
   * Read a player's move-speed multiplier.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Every S.A.M machine holds every player's multiplier, so a client can read anyone's and gets the host's answer.
   */
  function sam_get_move_speed(player: number): number;

  /**
   * Read any creature's mana by UID.
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (entity_uid), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_mp(entity_uid: number): number | undefined;

  /**
   * What is playing right now: one of a mod's tracks ("mymod:boss") or a vanilla name ("mines02", "shop").
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_get_music(): string | undefined;

  /**
   * List UIDs of monsters/players within radius tiles of a player (never raw pointers). In JavaScript both arguments are required.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Works on a client too (it used to return an empty table there). Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.
   */
  function sam_get_nearby_entities(player: number, radius: number): any;

  /**
   * The rectangle the level generator is actually allowed to use, which excludes the perimeter gap some maps reserve. A spawner that ignores this can place things inside the outer wall. In JavaScript this returns an array.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_playable_bounds(): any;

  /**
   * Read back a per-player in-memory value set by sam_set_player_data.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet.
   */
  function sam_get_player_data(player: number, key: string): any;

  /**
   * Get a player's entity uid, so the uid-based world-ops (get/set position) can act on that player's body.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_player_uid(player: number): number | undefined;

  /**
   * Read any entity's map-tile position (player, monster or ground item). Get a player's uid with sam_get_player_uid.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.
   */
  function sam_get_position(uid: number): any;

  /**
   * Exact position, including the sub-tile fraction and the z axis. sam_get_position rounds to a whole tile and drops z entirely, so nothing in script could tell a flying bat from a rat standing underneath it, or two creatures sharing one tile. In JavaScript this returns an array [x, y, z].
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.
   */
  function sam_get_position_precise(uid: number): any;

  /**
   * Get a player's race: a custom race's "namespace:race" id, or the vanilla race name ("human", "skeleton", …). Use it in a race behavior script to gate logic to players of that race.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_race(player: number): string | undefined;

  /**
   * The ranged attack figure for this creature, optionally including a quiver bonus.
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (entity_uid), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_ranged_attack(entity_uid: number, quiver_bonus?: number): number | undefined;

  /**
   * This machine's wall clock. Same warning as sam_get_fps: two players' clocks differ, so this must not feed a dice roll or anything you save.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_get_real_time(): number;

  /**
   * How often this creature regenerates health naturally. SMALLER is faster.
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (entity_uid), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript). For a client's own player it can miss the vampiric-aura bonus while more than 5 seconds of it remain (clients do not count effect timers); read it on the host for an exact value.
   */
  function sam_get_regen_interval(entity_uid: number): number | undefined;

  /**
   * The run clock the game itself displays. It stops during the intro, while you are dead, and while THIS machine has the game paused. Not the same as sam_get_time_played, which counts wall time since the program started, menus included. In multiplayer each machine counts its own, and pausing is local, so a player who spent a minute in their menu is a minute behind everyone else: read it on the host if a rule depends on it.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_get_run_time(): number;

  /**
   * Read an entity's scale. The counterpart to sam_set_scale, which shipped without a reader. In JavaScript this returns an array.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's copy; on a client the scale is rounded to steps of 1/128.
   */
  function sam_get_scale(uid: number): any;

  /**
   * Read the seed identifying this run. Pair it with sam_random when you want per-run variety.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_seed(): number;

  /**
   * A proficiency rank. Accepts both spellings — "PRO_SWORD" (the class schema) and "sword" (what player.on_proficiency_increased hands you). effective (default true) includes the equipment bonus the game actually uses; pass false for the raw trained rank. Ranks were completely unreadable before this, even though the framework has always fired the event.
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (uid), and anything else is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_get_skill(uid: number, skill: string, effective?: boolean): number | undefined;

  /**
   * List the spells a player currently knows.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). On the host, a player on another machine is answered from a copy their own game sends a moment after joining and whenever it changes (at most five times a second). nil means this machine cannot see that player's things: the host before their game has reported, or a player whose game does not run S.A.M. That 'not reported yet' moment comes at the start of EVERY run, not only after a join: the host drops the last character's copy when a new run starts and that player's game sends the new one a moment later, so read another player's backpack or spells from a timer or a later event, never from game.on_game_start. A removal is no more visible than any other change until that next report, so never write 'if they still have it, give the reward' without a flag of your own. A remote player's spells are reported by NAME, so they are right whatever order the mods loaded in.
   */
  function sam_get_spells(player: number): any;

  /**
   * Read a live player stat.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript. A client's own HUNGER is at most 5 seconds behind the host's.
   */
  function sam_get_stat(player: number, stat: string): number | undefined;

  /**
   * Read back what your own id currently contributes, so a mod does not have to remember. Only your mod's ids are visible to you.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_stat_modifier(player: number, stat: string, id: string): any;

  /**
   * The attack figure this creature would apply to a thrown weapon.
   *
   * Multiplayer: read. The host can read every creature; a client can read only its own player's uid (entity_uid), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_get_thrown_attack(entity_uid: number): number | undefined;

  /**
   * Logic frames per second. Barony's step is fixed, so there is no delta time to ask for; this is the constant every per-second conversion needs.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_tick_rate(): number;

  /**
   * Read one map tile. Liquid comes from the FLOOR tile, and the engine decides which tiles are liquid from their image filename — so a mod's own tile named "...lava..." reports as lava here too.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_tile(x: number, y: number): any;

  /**
   * Get elapsed game ticks for the current run.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_get_time_played(): number;

  /**
   * Which spell a spellbook or a spell tome contains. This is the bridge from an item to the spell functions: pair it with sam_grant_spell to teach whatever a book holds without hardcoding the pairing. Anything that is not a book returns nil.
   *
   * Multiplayer: read. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own. Returns false, not nil, when the uid names no item this machine can see, because nil already means the item teaches no spell. On the host it reads a remote player's items from the copy their game reports.
   */
  function sam_get_tome_spell(uid: number): number | undefined;

  /**
   * How fast something is moving and in what direction. Enough to lead a moving target, or to tell a charging monster from a standing one. In JavaScript this returns an array.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.
   */
  function sam_get_velocity(uid: number): any;

  /**
   * Read the current threshold. Answers 100 unless a mod said otherwise, so it is also how to read vanilla's number.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_get_xp_threshold(level: number): number;

  /**
   * Throw a chunk of gore off a creature. The optional sprite overrides the model.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. The chunks are seen by every player. A custom gib sprite crosses as a model number, so use vanilla model numbers for gibs in multiplayer.
   */
  function sam_gib(entity_uid: number, sprite?: number): boolean;

  /**
   * Add gold to a player (clamped to >= 0), syncing the client HUD.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_grant_gold(player: number, amount: number): boolean;

  /**
   * Give an item to a player: a vanilla name (e.g. "IRON_DAGGER") or a custom "namespace:item". The optional beatitude, status and count shape the item. A grant never asks player.on_before_item_pickup, so it cannot be vetoed.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A player on another machine receives it through the game's own item packet, so this works even for a player whose game does not run S.A.M; a mod item arrives as a rock for a player whose game does not have that mod. In singleplayer, slots 1 to 3 are not players and are refused.
   */
  function sam_grant_item(player: number, item_name: string, beatitude?: number, status?: number, count?: number): boolean;

  /**
   * Teach a player a spell: a vanilla SPELL_ name, a custom "namespace:spell", or the number sam_get_tome_spell returns. player.on_spell_learned fires once, on the host, when the spell is actually learned.
   *
   * Multiplayer: owner. The state lives on the player's own machine: on the host, a call about a player on another machine (named by player) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Carried to the player's own machine, where their spell list lives. Works for every player whose game runs S.A.M; a player without S.A.M is refused with a warning. When granting to another player prefer the NAME, because a mod's spell numbers follow mod load order. If their game refuses (the spell is already known), the reason shows in the host's log.
   */
  function sam_grant_spell(player: number, spell: string): boolean;

  /**
   * Give a player experience. Crossing the threshold levels them up naturally on the next tick, firing player.on_level_up, and respects any curve set with sam_set_xp_curve.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_grant_xp(player: number, amount: number): number | undefined;

  /**
   * Whether a key exists, without loading it. This genuinely distinguishes stored-but-empty from never-stored: saving nil writes a real entry, so sam_has_data is true while sam_load_data gives you nothing back. Use sam_delete_data when you want a key to actually be gone.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_has_data(key: string): boolean;

  /**
   * Check whether a player currently has a status effect.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_has_effect(player: number, effect: string): boolean | undefined;

  /**
   * Restore health to any player or monster by UID. Returns what actually landed, not what you asked for: health is clamped to the maximum, so a 50-point heal on something three short of full restores three.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_heal(entity_uid: number, amount: number): number | undefined;

  /**
   * Take the overlay away early. With no player it hides the overlay of the player the current event is about (outside an event: your own, and in splitscreen every local player's); -1 hides every player's in multiplayer.
   *
   * Multiplayer: screen. Shows on the screen of the player player names (left out: the player the current event is about, else this machine's own); -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.
   */
  function sam_hide_image(player?: number): boolean;

  /**
   * Singleplayer only. Briefly freeze enemy and projectile logic, a freeze-frame, for duration_ms (capped ~400). The player, HUD weapon and hand magic keep animating, so it reads as a punchy impact beat.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. In multiplayer it does nothing and says so once in the log: freezing the host's logic would desync the clients.
   */
  function sam_hitstop(duration_ms: number): boolean;

  /**
   * Show or update a horizontal bar — a custom resource, a charge meter, a boss health track. frac is clamped to 0..1; 0 draws as empty rather than a sliver.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_hud_bar(id: string, x: number, y: number, w: number, h: number, frac: number, color?: number, player?: number): boolean;

  /**
   * Remove one HUD element. No id removes the whole script HUD: sam_hud_clear(nil, p) clears everything on one player's screen. The HUD is also dropped automatically when the mod unloads, so it can never outlive the mod that drew it.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_hud_clear(id?: string, player?: number): boolean;

  /**
   * A PERSISTENT picture in the script HUD — a portrait, a custom gauge, a marker. Stays until sam_hud_clear(id) or the mod unloads, unlike the overlay. w/h of 0 means the picture's own pixel size. The colour is MIXED into the art, so white (the default) leaves it untouched and the alpha byte fades it.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_hud_image(id: string, x: number, y: number, w: number, h: number, image: string, color?: number, player?: number): boolean;

  /**
   * Show or update a line of text on screen. Calling again with the same id moves/retitles the existing line rather than stacking a new one.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message. To name a player without a colour, pass nil (null in JS) for the colour.
   */
  function sam_hud_text(id: string, x: number, y: number, text: string, color?: number, player?: number): boolean;

  /**
   * Identify an item the way a scroll does, through the engine's own path, so player.on_item_identified fires (once, on the host) and the owning player's screen updates. Calling it on an already-identified item succeeds quietly. An item on the floor, in a chest or in a shop belongs to nobody and is identified for the player you name; only an item in ANOTHER player's backpack is refused.
   *
   * Multiplayer: owner. The state lives on the player's own machine: on the host, a call about a player on another machine (named by uid) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning.
   */
  function sam_identify_item(player: number, uid: number): boolean;

  /**
   * The EXAGGERATED version of the flash: a colour pop PLUS manga speed lines converging on screen centre PLUS a bright core flare. Pair it with sam_camera_shake and sam_hitstop for a full impact beat. lines is the speed-line count (0 = a plain flash). In JavaScript player, r, g and b are required: a missing colour is refused instead of flashing black.
   *
   * Multiplayer: screen. Shows on the screen of the player player names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.
   */
  function sam_impact_frame(player: number, r: number, g: number, b: number, intensity?: number, duration_ms?: number, lines?: number): boolean;

  /**
   * Whether the bag has room. This one is genuinely local-only: the inventory grid exists on the machine drawing it, so asking about a remote player would answer about the wrong bag. It returns nil and logs why rather than lying.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). Answers only for a player on this machine, because it reads that machine's inventory grid: nil for anyone else, on the host too.
   */
  function sam_inventory_has_space(player: number): boolean | undefined;

  /**
   * Check whether a BOUND action is held. Reads Barony's own binding, so it follows whatever the player rebound it to (and works with mouse buttons, which raw keys can't see).
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript. On the host it works for every player: a joiner's game reports its buttons (false for a player whose game does not run S.A.M).
   */
  function sam_is_action_held(player: number, action: string): boolean | undefined;

  /**
   * The armour counterpart of sam_is_better_weapon, covering shields, helmets, breastplates, cloaks, boots, gloves and masks. Omit the second item to ask whether it is worth wearing at all, which is only ever true for something that actually goes in one of those slots.
   *
   * Multiplayer: read. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own. Returns nil, not false, when either uid names no item this machine can see, including a second uid that was given but names nothing, so it never silently compares against nothing.
   */
  function sam_is_better_armor(uid_new: number, uid_current?: number): boolean | undefined;

  /**
   * The same comparison monsters use when deciding what to pick up. Omit the second item to ask whether it is worth taking at all, which is only ever true for an actual weapon.
   *
   * Multiplayer: read. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own. Returns nil, not false, when either uid names no item this machine can see, including a second uid that was given but names nothing, so it never silently compares against nothing.
   */
  function sam_is_better_weapon(uid_new: number, uid_current?: number): boolean | undefined;

  /**
   * Whether a script has made this entity immune to damage with sam_set_damage_immune.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). The immunity set exists only on the host, so a client's call returns nil rather than a confident false.
   */
  function sam_is_damage_immune(uid: number): boolean | undefined;

  /**
   * Whether this floor is one of the unlit ones.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_is_dark_level(): boolean;

  /**
   * Whether the player is actually blocking right now: the real engine state, not just the Defend button being down.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_is_defending(player: number): boolean | undefined;

  /**
   * Would these two fight? The engine's own allegiance answer, so charm, race and faction are all accounted for.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_is_enemy(uid_a: number, uid_b: number): boolean | undefined;

  /**
   * The other side of sam_is_enemy — allies, followers and charmed creatures.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_is_friend(uid_a: number, uid_b: number): boolean | undefined;

  /**
   * Whether a dead player is walking around as a ghost. Worth checking before granting items or applying effects, since a ghost is not an ordinary player.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_is_ghost(player: number): boolean | undefined;

  /**
   * Whether this machine is the host. Functions of kind host warn once on a client; check this first to skip a whole block of host work.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_is_host(): boolean;

  /**
   * Ask before wasting a cast, which is impossible in vanilla.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_is_immune(uid: number, effect: string): boolean | undefined;

  /**
   * Whether a run is actually in progress. Worth checking at the top of a timer callback, which can otherwise fire while nobody is playing.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_is_in_game(): boolean;

  /**
   * Whether this specific item is currently equipped, as opposed to merely being in the bag. A spare identical ring in the bag does not count.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). On the host it answers for a remote player's items too, from the copy their game reports. It returns nil when the uid names no item this machine can see, and it is true only for the exact item being worn, so a spare identical ring does not read as equipped. It agrees with the equipped flag in sam_get_inventory.
   */
  function sam_is_item_equipped(player: number, uid: number): boolean | undefined;

  /**
   * Check whether a RAW key is currently held. Takes any key name the game itself uses, so what sam_get_action_binding hands back works here: single letters and digits, F1 to F12, and the spelled-out keys such as "Space", "Return", "Escape" and "Left Shift". A MOUSE binding has no key behind it and cannot be answered here; it says so in the log rather than returning false forever. Ignores the player's keybinds: prefer sam_is_action_held, which follows them and handles every binding kind.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript. Left out, player is the player the current event is about, else this machine's own. On the host, a player on another machine reads the keys their game reported: A-Z, 0-9 and F1-F12 only, and false for a player whose game does not run S.A.M.
   */
  function sam_is_key_held(key_name: string, player?: number): boolean | undefined;

  /**
   * Whether the game is mid-level-change. Entities are being destroyed and rebuilt during this, so it is the wrong moment to touch uids you were holding.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_is_loading(): boolean;

  /**
   * The engine's own test, so it agrees with what the game counts as melee for skill and damage purposes.
   *
   * Multiplayer: read. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own. On the host this answers for every player's items, including the uids sam_get_inventory hands back for a remote player. When the uid names nothing this machine can see it returns nil rather than false, and a warning names the function once.
   */
  function sam_is_melee_weapon(uid: number): boolean | undefined;

  /**
   * Is a given mod namespace loaded? The cheap form of sam_get_mods.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_is_mod_loaded(namespace: string): boolean;

  /**
   * Whether a player's parry window is currently open. The engine consumes this in melee resolution to produce parried damage; nothing exposed it before.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_is_parrying(player: number): boolean | undefined;

  /**
   * Whether the player has the game paused. Each machine has its own answer in multiplayer, where the world keeps running for everyone else.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_is_paused(): boolean;

  /**
   * The same judgement the game's own AI uses when deciding whether to throw a potion at you rather than drink it.
   *
   * Multiplayer: read. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own. On the host this answers for every player's items, including the uids sam_get_inventory hands back for a remote player. When the uid names nothing this machine can see it returns nil rather than false, and a warning names the function once.
   */
  function sam_is_potion_bad(uid: number): boolean | undefined;

  /**
   * Takes a TYPE, so it answers for an item you have not spawned.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_is_ranged_weapon(item_type: number): boolean;

  /**
   * Covers everything worn in the shield hand, not only shields: lanterns, torches, quivers, spellbooks and crystal shards all count, as does any custom item your mod marks for the shield slot.
   *
   * Multiplayer: read. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own. On the host this answers for every player's items, including the uids sam_get_inventory hands back for a remote player. When the uid names nothing this machine can see it returns nil rather than false, and a warning names the function once.
   */
  function sam_is_shield(uid: number): boolean | undefined;

  /**
   * Is this a sane place to put something: in bounds, not inside a wall, not lava. Check before spawning instead of dropping a monster into rock.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_is_spawnable(x: number, y: number): boolean;

  /**
   * The stricter ghost test: a spirit ghost specifically, rather than any ghost state.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_is_spirit_ghost(player: number): boolean | undefined;

  /**
   * Whether the map's rules allow digging here: it returns false for water and lava, the Hell and fortress edges, and any tile marked no-dig. It does NOT check that there is a wall to dig, so it is true on ordinary open floor too. Pair it with sam_get_tile if you need something solid to be there. Out-of-bounds coordinates return false rather than reading past the map.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_is_tile_diggable(x: number, y: number): boolean;

  /**
   * Whether an entity is visible. The counterpart to sam_set_visible.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's copy of the entity. On a client it can lag the host, and a flag the host clears without telling clients can stay set there. Read on the host when the answer matters. When a monster's invisibility effect ends, every player now sees it again (while scripts are loaded).
   */
  function sam_is_visible(uid: number): boolean | undefined;

  /**
   * The item counterpart of sam_monster_has_trait. Takes a TYPE. Answers for all eleven traits a mod can declare, checking what the item declared and then the game's own rule for a vanilla one. An unknown trait name is refused with the valid list logged rather than silently returning false.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_item_has_trait(item_type: number, trait: string): boolean;

  /**
   * Resolve an item's numeric type id — compare it against event fields like on_block's shield_type to react only to a specific item. Accepts a vanilla name or a custom "namespace:item".
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_item_id(name: string): number | undefined;

  /**
   * Kill a monster by UID (runs its normal death + drops; fires on_monster_died).
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_kill_monster(uid: number): boolean;

  /**
   * Level a player up count times (default 1) through the real engine path: attribute rolls, HP/MP gain, the level-up screen and sound, and full client sync. These are the actual benefits, unlike bumping LVL with sam_set_stat. Fires the player.on_level_up hook once per level.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_level_up(player: number, count?: number): boolean;

  /**
   * Can a straight line get from A to B? This is the engine's own trace, so it agrees with what is drawn — unlike plain distance, which sees through solid rock.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_line_of_sight(x1: number, y1: number, x2: number, y2: number, blockedByEntities?: boolean): any;

  /**
   * List the keys sam_save_data has written for your mod, so you can iterate stored state without having to remember every key name. Returns an empty table when nothing has been saved yet.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_list_data_keys(): any;

  /**
   * List every item the game knows about, including items added by mods (those have custom = true). This is what a recipe browser, a shop's stock list or a bestiary of loot is built from. The name it gives you is accepted by sam_grant_item, sam_spawn_item, sam_item_id and the rest, so listing and then granting works; if two items happen to share a displayed name the call refuses and names both rather than guessing.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_list_items(category?: string): any;

  /**
   * List the game's monster types. Note what this does NOT include: a S.A.M custom monster is a variant of a base species rather than a new entry in the engine's table, so it will not appear here as its own row — you will see the species it is built on. The NOTHING sentinel and the engine's reserved padding slots are filtered out. Pair with sam_spawn_monster for an arena mod, or with a panel for a bestiary.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_list_monsters(): any;

  /**
   * The ids sam_play_music and a monster's "music" accept. /sam_music in the console prints the same, plus the vanilla names a replacement can target.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_list_music(): string[];

  /**
   * The ids sam_play_sound accepts. /sam_sounds in the console prints the same; /sam_sounds vanilla <text> searches the game's own.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_list_sounds(): string[];

  /**
   * List the spells a player can actually be given, with their mana cost. Spells the game hides from its own UI are left out, so what you get back is the set that is meaningful to show a player.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_list_spells(): any;

  /**
   * Read back a persisted per-mod value.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_load_data(key: string): any;

  /**
   * The player index THIS machine controls.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_local_player(): number;

  /**
   * Write a line to sam_log.txt (the only output channel). Also exposed as sam.log(msg) in Lua.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_log(msg: string): void;

  /**
   * Turn one entity to face another. This is the one a turret wants — it does the trigonometry so you do not have to. Because your behaviour owns the entity, the engine has no opinion about which way it points; you do. Refuses to turn a PLAYER: their facing belongs to whoever is holding the mouse, and a script fighting their input every frame would feel broken.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A spawned entity turns smoothly on every player's screen; a pinned prop turns for S.A.M players.
   */
  function sam_look_at(uid: number, target_uid: number): boolean;

  /**
   * Show a line in a player's in-game message log.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A line sent to a player on another machine is shown with one leading space, so that player's game can never mistake it for one of Barony's own messages that it acts on.
   */
  function sam_message(player: number, text: string): boolean;

  /**
   * Change a creature's mana by a relative amount. Negative takes it away.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_mod_mp(entity_uid: number, amount: number): number | undefined;

  /**
   * Rewrite incoming damage (clamped to >= 0). ONLY valid inside an on_before_damage callback.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_modify_damage(player: number, new_value: number): void;

  /**
   * Rewrite the damage a MONSTER is about to take. Only valid inside an on_before_monster_damage callback. No subject argument: only one monster is ever mid-dispatch.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_modify_monster_damage(newValue: number): boolean;

  /**
   * Rewrite the number the engine is about to use, from inside any hook that offers one (player.on_xp_gained today, and every future modifiable hook; player.on_gold_collected offers no value to rewrite). Only valid inside such a callback: the error names the hook you ARE inside, so a wrong-place call says something useful.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_modify_value(newValue: number): boolean;

  /**
   * Make a monster swing immediately, using whatever attack pose its current weapon calls for.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_monster_attack(uid: number): boolean;

  /**
   * Whether a creature's AI knows how to use a kind of item. Only five species have that behaviour at all: goblins, humans, goatmen, automatons and shadows. Everything else answers false, including custom races, even though sam_monster_equip will happily put the item in their hand. It runs on the host, because it reads the monster's stats.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_monster_can_wield(uid: number, item_type: number): boolean | undefined;

  /**
   * Make a monster (or a companion) cast a spell along its own facing. Free cast, host-only.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_monster_cast_spell(uid: number, spell: string): number | undefined;

  /**
   * Send a monster into a straight-line charge for N ticks (50 = 1 second, default 50, max 500). Aims at its target if it has line of sight, otherwise charges along its current facing.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_monster_charge(uid: number, ticks?: number): boolean;

  /**
   * Put an item into a monster's equipment slot. Resolves a custom "ns:item" first and falls back to a vanilla item name. An unknown slot is refused and the valid list is logged. The last three arguments are the same three sam_grant_item takes, so a monster can be given a cursed, a broken or a stacked item.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_monster_equip(uid: number, slot: string, item: string, beatitude?: number, status?: number, count?: number): boolean;

  /**
   * Turn a monster to look at a tile. Aims at the tile centre. Pair it with sam_monster_charge to aim a charge.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_monster_face(uid: number, tileX: number, tileY: number): boolean;

  /**
   * The monster counterpart of sam_has_effect — e.g. react when a monster you just hit is POISONED. Pass a monster UID (from a monster event or sam_get_nearby_entities).
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_monster_has_effect(uid: number, effect: string): boolean | undefined;

  /**
   * Reads back what the mod declared in JSON. Without this a mod can SAY a monster is undead and the engine agrees, but the mod's own script cannot ask — so a "bonus vs undead" rule had no way to test for undead. False for every vanilla monster, so it is a no-op without a mod.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript.
   */
  function sam_monster_has_trait(uid: number, trait: string): boolean | undefined;

  /**
   * Path a monster to a tile using the engine's real pathfinder, then put it in the hunt state so it walks there. Tile coordinates, matching sam_get_position. Returns false when the destination is unreachable.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_monster_path_to(uid: number, tileX: number, tileY: number): boolean;

  /**
   * Empty one of a monster's equipment slots. Pairs with sam_monster_equip for disarm effects and for swapping a creature's loadout mid-fight.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_monster_unequip(uid: number, slot: string): boolean;

  /**
   * Nudge an entity by a relative distance, sliding along whatever it runs into rather than stopping dead or passing through. The answer is the distance it MANAGED, so 0 means something is right there and 0.3 out of a requested 2 means it hit a corner: a plain true or false would have hidden the difference. Distances are in tiles, like every other spatial call here, and a long move is walked in short steps so it cannot skip over a wall: the engine's collision test only looks at where you land, not at the path, so a single two-tile step used to step clean over a one-tile wall and report the whole distance as clear. A ground item or gold bag is woken, so it settles exactly like a dropped item (drops to the floor, falls into a pit, floats on water), in singleplayer too. Use sam_set_position to teleport, or sam_apply_force to shove.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). On a connected player it really moves them: their own machine applies the move through its own collision, and the returned distance is what the host computed. A player whose game does not run S.A.M is corrected with the engine's own position packet, which can occasionally be lost. A ground item or gold bag moves for every player. Moving a prop Barony never updates on a client (a gate, a torch) reaches players who run S.A.M; a player without S.A.M keeps seeing the old spot, and the log says so once.
   */
  function sam_move_entity(uid: number, dx: number, dy: number): number | undefined;

  /**
   * Give a scripted kill a proper death message and credit the killer. sam_kill_monster just sets health to 0, so today a scripted kill produces the generic message and nobody gets credit.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_obituary(killer_uid: number, victim_uid: number, from_spell?: boolean): boolean;

  /**
   * Override a class's STARTING stats/skills (patch = { STR, DEX, ..., MAXHP, skills = {...} }). Reverts on unload. Tables are read the same way in Lua and JavaScript: keys are case-insensitive, number fields take only numbers and text fields only strings.
   *
   * Multiplayer: all. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table. A patch left over from an earlier singleplayer game can still shape a client's FIRST character in a co-op run, because that character is built before the host's table arrives: patch classes when your mod loads, or reload mods before hosting.
   */
  function sam_patch_class(class_: any, patch: any): boolean;

  /**
   * Override an item type's base fields live: { weight, value/gold_value, level, category, slot, tooltip, name/name_identified, name_unidentified, attributes = {...} }. An unrecognised category or slot name is refused rather than being read as WEAPON or NO_EQUIP, which are real values that would have applied silently. The whole patch is checked before any of it is written, so a false really does mean the item is untouched. Tables are read the same way in Lua and JavaScript: keys and category and slot names are matched without regard to case, number fields take only numbers and text fields only strings, and value beats gold_value and name_identified beats name.
   *
   * Multiplayer: all. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.
   */
  function sam_patch_item(item: any, patch: any): boolean;

  /**
   * Override a monster type's base stats (e.g. { HP, MAXHP, STR }) for future spawns; also zero RANDOM_* for exact values.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_patch_monster(monster: any, patch: any): boolean;

  /**
   * Take over the music for every player -- a boss fight, a cutscene, a victory sting. track is a file in your mod's music/ folder: "boss" from your own scripts, or "mymod:boss". It plays until sam_stop_music(), or until the floor changes unless persist is true.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Every player hears it, including a player who joins while it plays, and it is dropped when a game ends or a new one starts. "Until the floor changes" really means the floor; it used to last until the area name changed.
   */
  function sam_play_music(track: string, fade_seconds?: number, loop?: boolean, persist?: boolean): boolean;

  /**
   * Play a sound for every player. sound_id is a vanilla numeric index OR one of your mod's sounds: a file sounds/boom.ogg is "mymod:boom", or just "boom" from your own scripts. vol 0-255 (default 128).
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Plays once per computer, however many splitscreen players share it. A mod sound reaches the other players by NAME, so everyone hears the same sound whatever order their mods loaded in; a player without the mod hears nothing for it. A client's call is refused with one log line, so an event that fires on every machine never plays it twice and needs no sam_is_host() guard.
   */
  function sam_play_sound(sound_id: number | string, vol?: number): boolean;

  /**
   * Positional audio: it attenuates with distance and pans, so a trap firing across the level is quiet, and in co-op each player hears it from where THEY are.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Every player hears it from where they stand, and a mod sound goes by name. A client's call is refused with one log line, so an event that fires on every machine never plays it twice.
   */
  function sam_play_sound_at(sound: number | string, tileX: number, tileY: number, volume?: number): boolean;

  /**
   * The same, at an entity's position -- and if that entity is a monster or wears an item with its own "sounds" map, that map applies.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Every player hears it. A sound map's sound reaches a player without the mod as the game sound it replaces. A client's call is refused with one log line.
   */
  function sam_play_sound_entity(sound: number | string, uid: number, volume?: number): boolean;

  /**
   * How many players are actually connected right now.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Correct on every machine after a player times out or is kicked with /kick (while a mod is loaded).
   */
  function sam_player_count(): number;

  /**
   * Check whether a player already knows a spell (vanilla or custom).
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). On the host, a player on another machine is answered from a copy their own game sends a moment after joining and whenever it changes (at most five times a second). nil means this machine cannot see that player's things: the host before their game has reported, or a player whose game does not run S.A.M. That 'not reported yet' moment comes at the start of EVERY run, not only after a join: the host drops the last character's copy when a new run starts and that player's game sends the new one a moment later, so read another player's backpack or spells from a timer or a later event, never from game.on_game_start. A removal is no more visible than any other change until that next report, so never write 'if they still have it, give the reward' without a flag of your own.
   */
  function sam_player_knows_spell(player: number, spell: string): boolean | undefined;

  /**
   * Power a mechanism on or off, as a switch wired to it would.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_power_entity(uid: number, on: boolean): boolean;

  /**
   * Work out what one creature's melee swing would do to another, dealing nothing. Built from the same three terms the real melee path combines: attack, the target's AC effectiveness, and its AC.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_preview_damage(attacker_uid: number, target_uid: number): number | undefined;

  /**
   * Deterministic random drawn from a named stream owned by your mod. The same run seed, the same stream and the same order of calls give the same number, which ordinary random() cannot promise.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Draws happen on the host only and are not synchronized between machines: roll on the host and send the result with sam_send_packet if a client needs it.
   */
  function sam_random(stream: string, min: number, max: number): number;

  /**
   * Rolls a percentage chance. 0 or less is always false and 100 or more is always true, so you never have to clamp. Drawn from one of your mod's named streams, so the same run and the same call order give the same rolls.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript. Draws happen on the host only and are not synchronized between machines: roll on the host and send the result with sam_send_packet if a client needs it.
   */
  function sam_random_chance(stream: string, percent: number): any;

  /**
   * A deterministic fraction from one of your mod's named streams. The same run, the same stream and the same call order give the same number.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Draws happen on the host only and are not synchronized between machines: roll on the host and send the result with sam_send_packet if a client needs it.
   */
  function sam_random_float(stream: string): number;

  /**
   * Picks one entry at random, deterministically. Note that Lua lists start at 1 and JavaScript arrays start at 0; each version follows its own language, so the same list gives the same element in both.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Draws happen on the host only and are not synchronized between machines: roll on the host and send the result with sam_send_packet if a client needs it.
   */
  function sam_random_from_list(stream: string, list: any): any;

  /**
   * Picks a key with probability proportional to its weight, for loot tables and spawn tables. Weights need not add up to anything in particular, and a weight of zero or less can never come up. Number keys count by their text in Lua and JavaScript alike ({[1]=5} and {1:5} both mean the key "1"), and the key comes back as a string.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Draws happen on the host only and are not synchronized between machines: roll on the host and send the result with sam_send_packet if a client needs it.
   */
  function sam_random_weighted(stream: string, weights: any): any;

  /**
   * Give a name to a function that will BE an entity's brain. Barony runs every entity through a function pointer once per frame; this puts yours behind one. Your function is called with the entity's uid, once per frame, for every entity you spawned with that behaviour — and everything else in this reference is available inside it, so it can look around, move, shoot, damage, or open a window. Nothing about what it does comes from a list. Register at the top of your script rather than inside a handler, so the name exists before you spawn anything with it. Registering the same name twice replaces the function, and entities already in the world follow the new code. Behaviours are dropped when mods reload.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_register_behavior(name: string, fn: (...args: any[]) => any): boolean;

  /**
   * Declare a namespaced custom hook. Name must contain a colon ("namespace:hook_name").
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_register_hook(name: string): void;

  /**
   * Remove a class passive effect previously added.
   *
   * Multiplayer: all. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.
   */
  function sam_remove_class_passive(class_: any, effect: any): boolean;

  /**
   * Clear a status effect from a player.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_remove_effect(player: number, effect: string): boolean;

  /**
   * Remove a non-player world entity by uid: a sam_spawn_portal marker, a spawned monster, a companion, a ground item, etc. Refuses players (use the normal death/teleport paths for those). Frees any light the entity owned, and closes the chest UI first if it is a chest somebody has open. A follower is also taken off its leader's follower list and ally HUD, on the leader's own machine too. The removal is QUEUED and happens on the next frame, so the uid still resolves for the rest of the current event. That is deliberate: your handler was called from inside the engine, which is still holding a pointer to that entity, so freeing it immediately corrupted memory.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Removing a follower also takes it out of its leader's follower list and ally HUD, on the leader's own machine too.
   */
  function sam_remove_entity(uid: number): boolean;

  /**
   * Remove a whole item stack from a player's inventory by its uid (from sam_get_inventory). Refuses an equipped item; unequip it first. The removal is queued and happens at the end of the tick, so it is safe inside an item event.
   *
   * Multiplayer: owner. The state lives on the player's own machine: on the host, a call about a player on another machine (named by item_uid) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning.
   */
  function sam_remove_item(item_uid: number): boolean;

  /**
   * Clear a status effect from a monster by UID — the monster counterpart of sam_remove_effect.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_remove_monster_effect(uid: number, effect: string): boolean;

  /**
   * The monster twin of sam_remove_stat_modifier.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.
   */
  function sam_remove_monster_stat_modifier(uid: number, id: string): number;

  /**
   * Un-learn a spell. Also takes the spell's item out of the backpack and hotbar (and a vanilla spell's shapeshift twin), and drops it from the selected, hotbar-alternate and quick-cast slots. It happens at the end of the tick, so removing a spell inside a cast or item handler is safe. Until then sam_player_knows_spell still answers true, and a sam_grant_spell of the same spell CANCELS the queued removal -- so "remove the vanilla spell, then grant my own version of it" in one handler leaves the player with the spell rather than with neither. The counterpart to sam_grant_spell.
   *
   * Multiplayer: owner. The state lives on the player's own machine: on the host, a call about a player on another machine (named by player) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Carried to the player's own machine. Works for every player whose game runs S.A.M; a player without S.A.M is refused with a warning.
   */
  function sam_remove_spell(player: number, spell: string): boolean;

  /**
   * Take back everything your mod contributed under one id, across every stat, leaving other mods' contributions alone.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.
   */
  function sam_remove_stat_modifier(player: number, id: string): number;

  /**
   * Hand the camera back to the engine. Call it when your mod unloads, or the player keeps your camera.
   *
   * Multiplayer: screen. Shows on the screen of the player player names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.
   */
  function sam_reset_camera(player: number): boolean;

  /**
   * Bring a dead player back with half their health, at a tile you name or at their ghost's own position.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Revives any player in co-op whose game runs S.A.M; a player without S.A.M is refused with a warning, because only their own machine can take their ghost down. The gear the death dropped or bagged is removed from the revived player, so nothing is duplicated.
   */
  function sam_revive_player(player: number, x?: number, y?: number): boolean;

  /**
   * Persist a value (number/string/bool/table) for the calling mod under savegames/sam_mod_data/<ns>/.
   *
   * Multiplayer: local. Answers for the machine running the script.
   */
  function sam_save_data(key: string, value: any): boolean;

  /**
   * Flash a player's whole screen in an RGB colour that fades to nothing — the anime "impact frame". intensity 0..1 is the peak opacity. Drawn on the machine the player lives on. In JavaScript player, r, g and b are required: a missing colour is refused instead of flashing black.
   *
   * Multiplayer: screen. Shows on the screen of the player player names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.
   */
  function sam_screen_flash(player: number, r: number, g: number, b: number, intensity?: number, duration_ms?: number): boolean;

  /**
   * Send a mod-defined message to another machine. Barony's packet ids are a fixed table, so before this a co-op mod had no way to tell the other side anything at all. On a client the target is ignored and the packet always goes to the host. The other side receives an "on_packet" event with .from, .tag and .payload. The tag is 1 to 32 characters and the payload at most 400 bytes: one datagram, so split bulk data into several packets.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Works only once the game has started: in the lobby it is refused with a warning, and in singleplayer it returns false. Delivery is reliable but NOT ordered, so number your packets if order matters. A payload may contain zero bytes: Lua handlers receive the exact bytes, JavaScript handlers receive text (invalid UTF-8 is replaced), so send text or JSON from mods meant for both runtimes. The host ignores a packet that claims to come from slot 0 or from an empty slot. This is how a client's own script (in on_packet) tells the host something.
   */
  function sam_send_packet(target: number, tag: string, payload: string): boolean;

  /**
   * Point the camera somewhere fixed. Yaw is radians, the same way sam_get_facing reports them. Called with no angle it goes back to following the player's own look, which is what the mouse drives.
   *
   * Multiplayer: screen. Shows on the screen of the player player names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.
   */
  function sam_set_camera_angle(player: number, yaw?: number, pitch?: number): boolean;

  /**
   * Whether the boom shortens when a wall is between the player and the camera. On by default. Turn it off for a camera meant to pass through geometry.
   *
   * Multiplayer: screen. Shows on the screen of the player player names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.
   */
  function sam_set_camera_collision(player: number, on: boolean): boolean;

  /**
   * Put the camera behind the player and keep it there: back tiles behind, up tiles above, right tiles to their right. It follows their yaw and pitch, so looking down swings the camera up and over rather than sliding along the floor, and the boom shortens when a wall is in the way. This is the whole of a third-person camera.
   *
   * Multiplayer: screen. Shows on the screen of the player player names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.
   */
  function sam_set_camera_offset(player: number, back: number, up?: number, right?: number): boolean;

  /**
   * Pin the camera in the world instead of on the player: a security camera, a cutscene, a fixed view of a room. x and y are tiles, height is tiles above the floor.
   *
   * Multiplayer: screen. Shows on the screen of the player player names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.
   */
  function sam_set_camera_position(player: number, x: number, y: number, height?: number): boolean;

  /**
   * Keep the camera pointed at something, recomputed every frame so it tracks a moving subject. Pass 0 to stop.
   *
   * Multiplayer: screen. Shows on the screen of the player player names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.
   */
  function sam_set_camera_target(player: number, uid: number): boolean;

  /**
   * Turn an existing chest into permanent storage. Its contents then live in the player's savegame instead of on the floor, surviving descending, dying later, quitting and loading. This is the game's own void-chest storage, so the window, the networking and the save round-trip are all vanilla. Two limits worth designing around: every stash chest in a run shares ONE set of contents, and the chest window holds 12 stacks — so this is a stash, not a bank. Converting a chest that already holds loot hides that loot until you turn the stash back off; prefer converting an empty one.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_chest_stash(chest_uid: number, on?: boolean): boolean;

  /**
   * Make a player or a monster stop taking damage, or let them take damage again. It works at the one place every point of damage to a CREATURE is applied, right beside the engine's own invulnerabilities, so for a creature nothing gets through by another route. Anything else is refused and the refusal says why: chests, doors, furniture and breakable decorations do not have health in that sense, they carry their own separate pools that a hit decrements directly, and no immunity here could reach them. The hit still lands, the sound still plays and the knockback still happens; only the loss of health is stopped, which is what invulnerable means everywhere else in Barony. You could already do this from an on_before_damage handler, and still can; this costs nothing per hit and needs no bookkeeping. It is session state: never saved, and cleared on every floor and at the start of a run, because entity uids restart from 1 on each level and a leftover entry would hand your boss's invulnerability to a rat downstairs.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_damage_immune(uid: number, on: boolean): boolean;

  /**
   * Put a player into or out of the blocking stance.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. On a player on another machine the override lasts exactly one host logic pass. It only changes the host's hit resolution: the player's own screen and the other players never show the raised shield.
   */
  function sam_set_defending(player: number, on: boolean): boolean;

  /**
   * Open or close a door. Find one with sam_find_entities(x, y, r, "door").
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_door(uid: number, open: boolean): boolean;

  /**
   * Lock or unlock a door.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_door_locked(uid: number, locked: boolean): boolean;

  /**
   * Retime an ALREADY-ACTIVE effect in place, without re-triggering it. No-op if the effect isn't active (never spawns a fresh one). 50 ticks = 1s.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_effect_duration(player: number, effect: string, ticks: number): boolean;

  /**
   * Change the magnitude/tier of an ALREADY-ACTIVE effect while keeping its remaining duration. strength 1-255.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_effect_strength(player: number, effect: string, strength: number): boolean;

  /**
   * Set how high an entity floats. This is the engine's raw z, the third value sam_get_position_precise gives you, so reading and writing it round-trips. Barony's z axis points DOWN (gravity adds to it), so negative numbers are up and 0 is the floor. Clamped to -1023..1023, which is what the network can carry. REFUSED on players and monsters: their height is rewritten from scratch by their own species code on every single frame, so the call would report success and be erased before the next frame drew. Lift a creature with a levitation effect instead. Also refused on a companion, whose own hover curve rewrites its height every tick for the same reason. Use this on props, ground items, spawned portals, and entities your script owns through sam_register_behavior (though if your own handler writes the height, it wins).
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A ground item's height reaches S.A.M players. On a ground item this sets the height WITHOUT waking it, so an item you have already placed stays at the height you give it and it no longer matters whether you call this before or after sam_set_position. An item that is still falling or sliding is under the engine's physics and pulls itself back down, so lift items that are at rest.
   */
  function sam_set_elevation(uid: number, z: number): boolean;

  /**
   * Point an entity at an angle. The primitive under sam_look_at, for when you are computing a direction yourself — a sweep, a spin, a lead on a moving target. The angle is normalised, so a behaviour that keeps adding to it will not drift out of range.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A spawned entity turns smoothly on every player's screen; a pinned prop turns for S.A.M players.
   */
  function sam_set_entity_facing(uid: number, radians: number): boolean;

  /**
   * Turn one of Barony's entity flags on or off, and tell the other players about it. PASSABLE for a decoration nobody should bump into, BLOCKSIGHT for a prop that should cast a shadow, UNCLICKABLE for scenery, BRIGHT for something that glows, BURNABLE to make a prop able to catch fire. Four flags are read-only here and the refusal tells you why: INVISIBLE belongs to sam_set_visible and BURNING to sam_set_on_fire, both of which know extra rules this one does not, while NOUPDATE and UPDATENEEDED are how the network sweep decides who to tell about what, and STASIS_DITHER is rewritten from the stasis effect on every frame so setting it would be undone before you saw it.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Also refused where the game rewrites the flag every frame: BLOCKSIGHT on players and monsters, INVISIBLE_DITHER on players, and BURNABLE on ground items.
   */
  function sam_set_entity_flag(uid: number, flag: string, on: boolean): boolean;

  /**
   * Set an entity's collision box. The number is a half-extent in world units, 16 to a tile, so 4 is the usual monster and 0 means nothing collides with it. size_y defaults to the same value. Clamped to 0..127 in every mode: the network carries the size as one signed byte, so anything larger arrives negative on the other machines and turns the hitbox inside-out there while looking correct to you. Pair it with sam_set_scale when you grow a model and want the swing to match. Unlike sam_set_elevation this sticks, because the engine only writes sizes when an entity is created. In JavaScript a size that is not a whole number is refused, as in Lua.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. On a player on another machine the new size reaches their own machine if it runs S.A.M; a player whose game does not run S.A.M is refused, because their machine could not be told and they would stick in doorways. A pinned prop's size reaches S.A.M players.
   */
  function sam_set_entity_size(uid: number, size: number, size_y?: number): boolean;

  /**
   * Make a player immune to a named effect — poison, curse, polymorph, anything the game has.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_immunity(player: number, effect: string, on?: boolean): boolean;

  /**
   * Set the appearance number, which chooses a readable book's contents and which potion or scroll look an unidentified item shows. REFUSED on the types where this field is not decoration: a spell tome stores its spell here, a loot bag its contents, the robots their health and a scepter its charges, and all of that is written to the save, so changing it would permanently alter what the item is.
   *
   * Multiplayer: owner. The state lives on the player's own machine: on the host, a call about a player on another machine (named by uid) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning.
   */
  function sam_set_item_appearance(uid: number, appearance: number): boolean;

  /**
   * Bless or curse an item. Clamped to a range the tooltips and damage maths can actually represent. A worn item's new blessing reaches the host's copy used for combat.
   *
   * Multiplayer: owner. The state lives on the player's own machine: on the host, a call about a player on another machine (named by uid) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning.
   */
  function sam_set_item_beatitude(uid: number, value: number): boolean;

  /**
   * Set how many are in a stack, up to the item's own limit; a larger number is refused rather than silently wrapped, and sam_get_max_stack tells you the ceiling. A count of zero or less DESTROYS the item, which happens a moment later rather than instantly: the game is still using that item at the point your script runs, so the removal is queued and carried out on the next frame. Destroying an equipped item is refused, because the engine's cleanup identifies items by their contents and cannot tell two identical ones apart.
   *
   * Multiplayer: owner. The state lives on the player's own machine: on the host, a call about a player on another machine (named by uid) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning.
   */
  function sam_set_item_count(uid: number, count: number): boolean;

  /**
   * Set an item's droppable flag. Know what the game reads it for: a monster's items when it dies (and no item uid can reach a monster's inventory today), and worn armour a thief steals, which copies the flag. On a player's item it therefore matters only if a thief steals it while it is worn. The true or false is required: leaving it out is refused rather than treated as false.
   *
   * Multiplayer: owner. The state lives on the player's own machine: on the host, a call about a player on another machine (named by uid) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning.
   */
  function sam_set_item_droppable(uid: number, droppable: boolean): boolean;

  /**
   * Reassign ownership. This is how a soulbound or stolen-goods mod says what it means using the game's own bookkeeping instead of inventing a parallel one.
   *
   * Multiplayer: owner. The state lives on the player's own machine: on the host, a call about a player on another machine (named by uid) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning. An owner set on a client player's item stays while it is in their backpack, but is lost if they drop it: the game's drop packet carries no owner.
   */
  function sam_set_item_owner(uid: number, owner_uid: number): boolean;

  /**
   * Set an item's condition, as a name or as a number from 0 (BROKEN) to 4 (EXCELLENT). Anything outside that range is refused rather than quietly rounded to the nearest end, so a wrong number tells you instead of half working. A string is always read as a name, so pass 3 and not "3". Note that dropping a worn item to BROKEN does not take it off: the game refuses to USE a broken item but leaves it equipped, and that is vanilla behaviour rather than something this call gets wrong.
   *
   * Multiplayer: owner. The state lives on the player's own machine: on the host, a call about a player on another machine (named by uid) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning.
   */
  function sam_set_item_status(uid: number, status: string): boolean;

  /**
   * Swap any entity's model while the game is running. What crosses the wire is the model ID, never an index, so machines with different mod orders still agree. This is what makes transformations, boss phases and damage states possible; before it, a model was fixed at spawn.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. The model crosses BY NAME and in order, and a player who joins later is told as well, so every machine that has the mod draws the same model whatever order their mods loaded in. A player whose game does not run S.A.M keeps seeing the entity's original look.
   */
  function sam_set_model(uid: number, model_id: string): boolean;

  /**
   * Store any primitive/table value in a monster's scratch store (JSON-marshaled). Returns true in Lua and JavaScript alike.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_monster_data(uid: number, key: string, value: any): boolean;

  /**
   * Make one creature immune to a named effect.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_monster_immunity(uid: number, effect: string, on?: boolean): boolean;

  /**
   * Rename a living monster. The name is what the player sees when targeting it and what appears in the obituary, so this is how a scripted boss or a named rare gets its title.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A follower's new name reaches its owner's ally panel and nametag, if the owner's game runs S.A.M.
   */
  function sam_set_monster_name(uid: number, name: string): boolean;

  /**
   * Set a monster's stat by UID (bounded). MAXHP is capped at 32767, the enemy HP bar's wire size.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A remote player's follower shows its new MAX HP and level on its owner's ally panel.
   */
  function sam_set_monster_stat(uid: number, stat: string, value: number): boolean;

  /**
   * Make a monster acquire a player as its attack target.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_monster_target(uid: number, player: number): boolean;

  /**
   * Point a monster at ANY entity, not just a player: another monster, a companion, anything with a body.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_monster_target_uid(uid: number, target_uid: number, was_hit?: boolean): boolean;

  /**
   * Set a player's move-speed multiplier, clamped to [0.1, 3.0]. 1.0 is normal speed.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Reaches every S.A.M player's machine in order, and is caught up at the start of every run. A player whose game does not run S.A.M still walks at vanilla speed, because their own machine computes their movement.
   */
  function sam_set_move_speed(player: number, mult: number): boolean;

  /**
   * Set something alight, or put it out with sam_set_on_fire(uid, false). The answer is whether it is burning NOW, which is deliberately not what the engine's own function returns: that one answers false for an entity that was already on fire, so a script retrying on false would retry for ever. Two things stop a fire starting and each logs its reason. An entity that is not BURNABLE will never light (turn the flag on with sam_set_entity_flag first), and skeletons, automatons, anyone in a machinist apron and anyone wearing an amulet of burning resistance are immune. One thing to know about props: the burn timer only runs for players and monsters, so a chest or a decoration you light stays lit for the rest of the level and hurts nothing. That is useful for a brazier, and the second argument is how you undo it.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_on_fire(uid: number, on?: boolean): boolean;

  /**
   * Open a parry window for a number of ticks. 0 closes it.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_parry(player: number, ticks: number): boolean;

  /**
   * Store a per-player value (number/string/bool/table) in memory for THIS session: the right tool for cooldowns, ability flags and stack counters you read often. Unlike sam_save_data it never touches disk, and it is cleared when a game starts.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript. Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet. A value written in player.on_player_joined (the lobby) is cleared when the game starts: set per-player state up in game.on_game_start or game.on_level_entered.
   */
  function sam_set_player_data(player: number, key: string, value: any): void;

  /**
   * Move an entity to a map tile. Players go through the safe teleport path and cannot tunnel into walls; everything else is relocated and re-broadcast to clients. Anything that is not a player is placed where you asked even when the tile is blocked, because putting a decoration inside a wall alcove is a real thing mods do, but you get a logged warning: a monster dropped into a wall is stuck there for good. Call sam_can_stand first when the answer matters. A shared engine marker uid (0 or a negative number) and a limb uid are both refused with a reason.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Items, gold bags and pinned props reach every player that can be told: S.A.M players see props Barony never updates on a client (a gate, a torch) move too, while a player without S.A.M keeps seeing the old spot and the log says so once. A teleported player on another machine does not snap back. A ground item or a bag of gold that has already come to REST stays at rest: it is placed exactly where you ask -- in mid-air, over a pit, inside a wall alcove -- and it does not fall, in any mode. Use sam_push_entity when you want it to fall or slide; that call wakes the item on purpose. An item still in flight is simply moved mid-fall and lands where it lands.
   */
  function sam_set_position(uid: number, tile_x: number, tile_y: number): boolean;

  /**
   * Run callback (a function) every interval_ticks until cancelled.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript. Timers run on the host only. Every machine drops all timers when a run starts, so set them from game.on_game_start or later, never from top-level script code. A timer callback is not an event: a screen call inside it with no player shows on the host's screen.
   */
  function sam_set_repeating_timer(id: string, interval_ticks: number, callback: any): void;

  /**
   * Scale an entity. A scale of 0 or less is REFUSED, not quietly treated as 1.0 the way it used to be, which turned a script easing a model down to nothing into a model that popped back to full size on the last frame. To make something disappear use sam_set_visible. Clamped at the bottom to 1/128 with a warning as well, because the wire packs scale into one byte as scale times 128, so anything smaller arrives as 0 and vanishes on every other machine. Clamped at 1.99 with a logged warning, in EVERY mode including singleplayer: Barony quantises scale on the wire in 1/128 steps with a cap just under 2, so a larger value looks right to you and wrong to everyone else. The clamp used to be skipped in singleplayer, which meant a mod authored at 3.0 worked for its author and was broken the moment anyone hosted it.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Refused for players and slimes, whose scale the game rewrites every frame on every machine. A pinned prop's exact scale reaches S.A.M players.
   */
  function sam_set_scale(uid: number, scale: number): boolean;

  /**
   * Change how much of a weapon class an entire species takes. 1.0 normal, 0.5 halves it, 2.0 doubles it. Every creature of that species, now and later.
   *
   * Multiplayer: all. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_species_damage_resist(species: string, type: string, multiplier: number): boolean;

  /**
   * Make every creature of a kind immune to an effect, now and later. Skeletons that cannot be poisoned, automatons that cannot be charmed.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_species_immunity(species: string, effect: string, on?: boolean): boolean;

  /**
   * Set a live player stat, bounded (HP never exceeds MAXHP, stats clamped, etc.). The change reaches the player's own machine, and a LVL write updates every player's party display at once.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_set_stat(player: number, stat: string, value: number): boolean;

  /**
   * Write one map tile — dig a passage, wall something in, flood a room. Refuses out of bounds rather than corrupting the map array. Check sam_tiles_connected afterwards if the edit could seal the exit.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Edits reach every S.A.M player in the order they were made, including a player who finishes loading late. A player whose game does not run S.A.M sees only dug walls (a wall set to 0), not placed walls or floor and ceiling changes. Tile ids only mean the same tile when every player has the same mods.
   */
  function sam_set_tile(x: number, y: number, layer: number, tileId: number): boolean;

  /**
   * Run callback (a function) once after delay_ticks (50/sec). Replaces any timer with the same id.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript. Timers run on the host only. Every machine drops all timers when a run starts, so set them from game.on_game_start or later, never from top-level script code. A timer callback is not an event: a screen call inside it with no player shows on the host's screen, so keep the player in a local and pass it.
   */
  function sam_set_timer(id: string, delay_ticks: number, callback: any): void;

  /**
   * Show or hide an entity. The flag is REQUIRED: leaving it out is refused rather than guessed. A numeric 0 counts as false in both runtimes. Refused, with a logged reason, for players, monsters, ground items and a creature's LIMB (its weapon, shield, helmet or an arm), whose visibility the game rewrites every frame: use sam_apply_effect(player, "INVISIBLE", ticks) for a player or sam_apply_monster_effect(uid, "INVISIBLE", ticks) for a monster. On a limb INVISIBLE is how the game says the slot is EMPTY, so hiding one lasted a single tick; take the item off the creature instead. sam_set_model on a limb still works, which is how a cosmetic is floated in an empty hand. On props, spawned entities and companions it works even when they have a custom model.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Players, monsters, ground items and a creature's limb are refused, because the game rewrites their visibility every frame; use sam_apply_effect(uid, 'INVISIBLE', ticks) for a creature. On props, spawned entities and companions it works even when they have a custom model, and the change reaches every player.
   */
  function sam_set_visible(uid: number, visible: boolean): boolean;

  /**
   * How much experience the next level costs. Barony's is a flat 100 at every level — the vanilla scaling is literally commented out beside it in the engine — so every progression mod has had to fake it by writing EXP directly. The XP bar follows the curve, on clients too, and sam_level_up grants what the curve charges.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A curve declared when your mod loads reaches clients on the first floor. The XP bar and a client's own EXP stay correct above 255 on S.A.M clients; a player whose game does not run S.A.M sees EXP modulo 256 on the bar.
   */
  function sam_set_xp_curve(level: number, threshold: number): boolean;

  /**
   * Cover a player's screen with one of the mod's pictures, over the world AND the HUD, for duration_ms (0 or omitted = until sam_hide_image). This is the jumpscare / title-card / death-splash layer: it removes itself, so there is nothing to clean up. alpha is 0..255 (default 255). "contain" keeps the picture's aspect ratio; "stretch" (default) fills the view. In multiplayer it is drawn on that player's own machine, from its own copy of the mod.
   *
   * Multiplayer: screen. Shows on the screen of the player player names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.
   */
  function sam_show_image(player: number, image: string, duration_ms?: number, alpha?: number, fit?: string): boolean;

  /**
   * The same overlay, placed rather than full-screen. Coordinates are virtual screen pixels (the space sam_hud_text uses), so a fixed layout survives any resolution. w or h of 0 means the picture's own size on that axis. Still drawn over the HUD — for a picture that sits IN the HUD, use sam_hud_image.
   *
   * Multiplayer: screen. Shows on the screen of the player player names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.
   */
  function sam_show_image_at(player: number, image: string, x: number, y: number, w: number, h: number, duration_ms?: number, alpha?: number): boolean;

  /**
   * Whether you can see your own character. Normally you cannot: the camera is inside your head, so the engine hides your body and draws a first-person weapon instead. Turning this on shows the body and hides that weapon, which is what any camera outside the head needs.
   *
   * Multiplayer: screen. Shows on the screen of the player player names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.
   */
  function sam_show_own_body(player: number, on: boolean): boolean;

  /**
   * Spawn a floating COMPANION (a JoJo-style "Stand" / familiar) that renders one of your custom .vox models and trails the player a short distance behind, with a gentle hover. Follows the player every frame and faces where they face. The optional scale (default 1.0) sizes the model; it is clamped to 1.99 with a warning, the most the network can carry. Drive the punch motion with sam_companion_punch, and clear it with sam_remove_entity. It's a decorative follower (PASSABLE, no AI, does no damage on its own; pair it with sam_cast_spell / sam_deal_damage for the actual attack). Re-spawn it on each new floor (entities are cleared on descent).
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Every S.A.M player sees it follow and animate. A player whose game does not run S.A.M cannot draw a companion's mod model at all.
   */
  function sam_spawn_companion(player: number, model_id: string, scale?: number): number | undefined;

  /**
   * Put something in the world that runs your behaviour. This is the other half of sam_register_behavior: that one supplies the code, this gives it a body. The entity starts passable with no collision of its own, because your behaviour decides what it collides with. Leave model empty and it is invisible, which is almost never what you want.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Every player sees it move, turn, rise and fall smoothly, and its mod model is sent by NAME, so a player whose mods loaded in another order still sees the right model; sam_get_model(uid) answers the model it was spawned with on every machine. A player whose game does not run S.A.M sees a spawned entity with a special vanilla model (a torch, door, gate, chest lid, portal or gold bag) behave like that object, and it may stay where it first appeared: use a mod model or an ordinary vanilla model for anything that moves.
   */
  function sam_spawn_entity(tile_x: number, tile_y: number, behaviour: string, model?: string): number | undefined;

  /**
   * Spawn a ground item at a map tile. status, beatitude and count let you put an item back exactly as you found it — without them a stash could record that you owned a cursed, worn ring and then only ever hand back a pristine one. The uid comes back so you can move it (sam_set_position) or clear it (sam_remove_entity) later; a uid is never 0, so an older if sam_spawn_item(...) check still behaves as it did.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). A ground stack above 255 shows as its count modulo 256 on other players' screens (the game's own packet carries one byte), but picking it up gives the full count. Spawn smaller stacks if the display matters.
   */
  function sam_spawn_item(x: number, y: number, item_name: string, status?: number, beatitude?: number, count?: number): number | undefined;

  /**
   * Summon a monster at a map tile. "shopkeeper" makes a working shop; the optional shop_type (0-14) picks the store kind.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).
   */
  function sam_spawn_monster(tile_x: number, tile_y: number, monster_name: string, shop_type?: number): number | undefined;

  /**
   * Spawn count (1-8) monsters of a type near an anchor entity's UID.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.
   */
  function sam_spawn_monsters(near_uid: number, monster_type: string, count: number): number;

  /**
   * A vanilla particle burst at a tile, so a mod's own effect looks like part of the game.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A poof's scale is clamped to 0.01..655 with a warning, so every player sees the same size.
   */
  function sam_spawn_particle(kind: string, tileX: number, tileY: number, z?: number, scale?: number): boolean;

  /**
   * Spawn a purely DECORATIVE portal (the swirling vortex) at a map tile: it animates and glows but is never interactive and never sends anyone to the next floor. Walkable, so a player can stand on it. Returns the uid so you can move it (sam_set_position) or clear it (sam_remove_entity), e.g. a portal-gun marker. In JavaScript both tile arguments are required.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Every player sees it (it used to show on the host only).
   */
  function sam_spawn_portal(tile_x: number, tile_y: number): number | undefined;

  /**
   * Fire a moving projectile with its own speed, model, damage and lifetime. Until this the only thing a script could launch was a fixed vanilla spell, which ruled out ranged enemies with real attack patterns, telegraphed boss volleys, and weapons that fire anything but an arrow. It stops on the first thing it hits and fires an "on_projectile_hit" event with .projectile, .target, .x, .y and .damage: spawn a follow-up there for a burst or an explosion. Giving an owner stops the shot killing the player who fired it on its first frame. Leave model empty and the projectile is INVISIBLE, which is almost never what you want.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Damage, collisions and the hit event are decided on the host, and every player sees the shot fly smoothly between network updates (it used to stand still between them).
   */
  function sam_spawn_projectile(tile_x: number, tile_y: number, angle: number, speed: number, damage?: number, lifetime?: number, model?: string, owner?: number): number | undefined;

  /**
   * Hand the music back. The game crossfades to whatever it would have been playing.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_stop_music(): boolean;

  /**
   * Stop every playing copy of one of your sounds, for every player. The way to end a sound declared with "loop": true, which otherwise plays until the floor changes.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0. Reaches S.A.M players in order after the play it stops.
   */
  function sam_stop_sound(sound: string): number;

  /**
   * Can something WALK (or fly) from A to B at all? The softlock check: after a mod edits terrain, ask whether the exit is still reachable before committing.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Answers correctly on a client after the host changes terrain.
   */
  function sam_tiles_connected(x1: number, y1: number, x2: number, y2: number, flying?: boolean): boolean;

  /**
   * Flip a lever or switch, driving whatever it is wired to.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.
   */
  function sam_toggle_switch(uid: number): boolean;

  /**
   * Send the party to any floor, including BACK UP, which the game otherwise never does: a ladder only ever counts upward, so before this no hub, home base or shop you walk back to was possible. The trip is deferred exactly as a ladder defers it, so it is safe to call from inside an event handler. Refused, with a logged reason, on a client, while another level change is already under way, or before a game has started. Nothing on the old floor is preserved: floors regenerate from the map seed, so put anything that must survive in a stash chest or in sam_world_save. opts.secret follows one rule in both runtimes: in Lua, { secret = 0 } means not secret, as in JavaScript.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. The engine's periodic level-change reminder is held back while a change is pending, so a player cannot be sent to the wrong floor. In Lua, opts.secret = 0 now means not secret, as it does in JavaScript.
   */
  function sam_travel_to_level(floor: number, opts?: any): boolean;

  /**
   * Put a clickable button in a panel. Clicking it fires a "ui.on_click" event whose .panel and .widget match what you passed here, so one handler can serve every button by switching on .widget. The panel must have been opened with modal = true or the player will have no cursor to click with.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_ui_button(panel: string, id: string, x: number, y: number, w: number, h: number, text: string, player?: number): boolean;

  /**
   * Remove every widget from a panel but leave the panel itself open. This is how you rebuild a changing screen — clear, then re-declare the rows — without the window flickering shut and open again.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_ui_clear(panel: string, player?: number): boolean;

  /**
   * Close one panel, or every panel your mod has open if you pass nothing (sam_ui_close(nil, p) closes all of them on one player's screen). Closing the last modal panel restores the player's camera control. Always close your panels on player.on_death and game.on_game_start so a leftover window cannot follow the player into the next run.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_ui_close(panel?: string, player?: number): boolean;

  /**
   * Change the font of one widget, or of an entire panel by passing an empty id -- which is the only way to restyle a panel's text in one call rather than widget by widget. Panels default to a small 16px face because the game's standard 32px font makes any list look enormous. The number after the first # is the pixel size — raise it for a heading, and raise the row height to match if it is a list.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_ui_font(panel: string, id: string, font: string, player?: number): boolean;

  /**
   * Put one of your mod's pictures in a panel, scaled to w by h. Resolves the same way sam_show_image does. The colour argument tints the picture and its alpha fades it, so the same file can be reused greyed-out for a locked entry.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_ui_image(panel: string, id: string, x: number, y: number, w: number, h: number, image: string, color?: number, player?: number): boolean;

  /**
   * Put an editable text box in a panel — a search field, a name entry, a price offer. Read what the player typed with sam_ui_input_text. Place the box clear of any label: a label wide enough to overlap the box will sit on top of it.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_ui_input(panel: string, id: string, x: number, y: number, w: number, h: number, text?: string, player?: number): boolean;

  /**
   * Read what the player has typed into one of your text boxes. Poll it from a button handler, or when a ui.on_submit arrives. Pressing Enter in the box also fires "ui.on_submit" with the text in .value.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). Left out, player is the player the current event is about, else this machine's own. On the host, a player on another machine is answered from what their game last reported (typing is reported a few times a second), so right after sam_ui_input it can still hold the old text for a moment.
   */
  function sam_ui_input_text(panel: string, id: string, player?: number): string;

  /**
   * Ask whether one of your panels is on screen. Useful to make a key or an item toggle a window instead of re-opening it, and to skip expensive refresh work while it is closed.
   *
   * Multiplayer: read. The host can read every player; a client can read only its own player (player), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, undefined in JavaScript. Left out, player is the player the current event is about, else this machine's own. On the host, a player on another machine is answered from what their game last reported, so right after sam_ui_open it can still say closed for a moment.
   */
  function sam_ui_is_open(panel: string, player?: number): boolean | undefined;

  /**
   * Put a line of text in a panel. x/y are measured from the panel's top-left corner, not the screen. Give w enough room for the text or it will be cut off — sam_ui_text_size measures a string before you place it. Re-declaring the same id replaces the text, which is how you update a running total.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_ui_label(panel: string, id: string, x: number, y: number, w: number, text: string, color?: number, player?: number): boolean;

  /**
   * Create an empty scrolling list in a panel. Fill it with sam_ui_list_add. This is the widget for a shop's stock, a bestiary, a recipe index or a quest log — anything with more entries than fit on screen.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_ui_list(panel: string, id: string, x: number, y: number, w: number, h: number, player?: number): boolean;

  /**
   * Append one row to a list. Clicking a row fires "ui.on_select" with .panel, .widget set to the list and .value set to the row_id you chose here — so make row_id something you can act on, like an item id, rather than a display string.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_ui_list_add(panel: string, id: string, row_id: string, text: string, color?: number, player?: number): boolean;

  /**
   * Empty one list without touching the rest of the panel. Use this before re-filling a list from a search box or a filter, so the old results do not pile up under the new ones.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_ui_list_clear(panel: string, id: string, player?: number): boolean;

  /**
   * Set how tall each row of a list is. Raise it if you switched that list to a larger font, or rows will overlap.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_ui_list_row_height(panel: string, id: string, pixels: number, player?: number): boolean;

  /**
   * Open one of your mod's panels at a position and size given in VIRTUAL screen units (1280x720 at the default UI scale, not your monitor's pixels). modal = true frees the mouse cursor so the player can click your widgets, and hands camera control back when the panel closes — use it for anything with buttons. A non-modal panel is display-only and leaves the player in normal look-around mode. Opening a panel id that is already open re-positions it instead of opening a second one.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message. A modal panel shown to a client frees that client's mouse, and typing into its text box does not move that player. Clicks fire ui.on_click, ui.on_select and ui.on_submit on the host, with .player set to who clicked.
   */
  function sam_ui_open(panel: string, x: number, y: number, w: number, h: number, title?: string, modal?: boolean, player?: number): boolean;

  /**
   * Recolour a panel's background and border. Nothing about a panel's look is fixed by the framework — set the background fully transparent for a bare overlay, or opaque for a solid window. Colours accept the same forms as the HUD calls.
   *
   * Multiplayer: screen. Shows on one player's own screen, picked by the optional last argument player: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.
   */
  function sam_ui_panel_style(panel: string, background: number, border: number, border_width?: number, player?: number): boolean;

  /**
   * Measure a string before you place it. This is how you lay a panel out properly instead of guessing: size a label to its own text so it cannot overlap the widget beside it, right-align a column of numbers, or centre a heading in a panel of known width.
   *
   * Multiplayer: any. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.
   */
  function sam_ui_text_size(text: string, font?: string): number | undefined;

  /**
   * Revert a class stat/skill patch.
   *
   * Multiplayer: all. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.
   */
  function sam_unpatch_class(class_: any): boolean;

  /**
   * How much of the savegame's mod-state budget is in use. sam_world_save shares one 64 KB allowance between every mod in the run.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet.
   */
  function sam_world_bytes(): number | undefined;

  /**
   * How much room is left before sam_world_save starts refusing writes. Check this before storing something large, rather than discovering the ceiling when a save quietly fails mid-run.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet.
   */
  function sam_world_bytes_free(): number | undefined;

  /**
   * Forget one key for the current character. The whole store is dropped automatically when a run ends, so you only need this to reset something mid-run.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet.
   */
  function sam_world_clear(key: string): boolean;

  /**
   * List the keys your mod has saved for this character. Handy for migrating an older save's data, or for showing the player what a mod is remembering about their run.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns an empty table (an empty array in JavaScript). Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet.
   */
  function sam_world_keys(): any;

  /**
   * Read a value back from the current character's savegame. nil on a key you have never written is the signal that this is a fresh character — that is the natural place to run first-time setup, like anchoring a home floor.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet.
   */
  function sam_world_load(key: string): any;

  /**
   * Save a value inside the CURRENT character's savegame. A brand new character starts with none of it, so a hub's unlock flags, a quest's progress or a bank balance cannot leak from one run into the next. Deliberately size-capped (8KB per value, 64KB across all mods) because oversized save data can produce a savegame that fails to load — keep flags and counters here and keep items in a stash chest, which the game persists properly on its own.
   *
   * Multiplayer: host. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet.
   */
  function sam_world_save(key: string, value: any): boolean;

  /** Every event name the engine fires. */
  type SamEventName =
    /** Fires a script calls sam_fire_hook("namespace:name", event); delivered cross-runtime (Lua<->JS<->TS) to every loaded script's on_event. Multiplayer: Fires on the machine that calls sam_fire_hook, which is the host (a client's call is refused). */
    | "<namespace>:<hook_name>"
    /** Fires the game is won or the party wipes. Multiplayer: Fires on the host, once per run (a party wipe and every win path alike). */
    | "game.on_game_end"
    /** Fires a new game begins. Multiplayer: Fires on the host, once for every player. */
    | "game.on_game_start"
    /** Fires a floor finishes loading. Multiplayer: Fires on the host, once for every connected player on every arrival (ladders, portals, teleports and sam_travel_to_level alike). */
    | "game.on_level_entered"
    /** Fires a BOUND action goes down — e.g. the player presses whatever they have "Use" mapped to. Multiplayer: Fires on the host (and in singleplayer), for every player: each local seat, and each joiner's buttons as their game reports them. A joiner whose game does not run S.A.M produces none. */
    | "on_action_pressed"
    /** Fires a bound action goes back up. Multiplayer: Fires on the host (and in singleplayer), for every player, the same way as on_action_pressed. */
    | "on_action_released"
    /** Fires before a player's HP is reduced (bracketed around Entity::modHP). Multiplayer: Fires on the host, for every player. */
    | "on_before_damage"
    /** Fires a status effect is about to be applied to any creature, before the engine's own immunity checks. Multiplayer: Fires on the host. */
    | "on_before_effect_applied"
    /** Fires before a monster's HP is reduced. Multiplayer: Fires on the host. */
    | "on_before_monster_damage"
    /** Fires while a hit's damage multiplier is being decided, after every vanilla effect has had its say. Multiplayer: Fires on the host. */
    | "on_damage_multiplier"
    /** Fires a supported RAW key transitions to down (A-Z, 0-9, F1-F12). Multiplayer: Fires on the host, for every player: the host's keyboard (player is the seat holding it) and each joiner's keys (A-Z, 0-9, F1-F12) as their game reports them, a moment after the press. A joiner whose game does not run S.A.M produces no key events. */
    | "on_key_pressed"
    /** Fires a supported key transitions to up (A-Z, 0-9, F1-F12). Multiplayer: Fires on the host, for every player, the same way as on_key_pressed. */
    | "on_key_released"
    /** Fires any monster takes damage. Multiplayer: Fires on the host. */
    | "on_monster_damaged"
    /** Fires any monster dies (melee, ranged, magic, or scripted). Multiplayer: Fires on the host. */
    | "on_monster_died"
    /** Fires a mod-defined message sent with sam_send_packet arrives. Multiplayer: Fires on the machine the packet was sent to: a client's scripts for a packet from the host, the host's scripts for a packet from a client. */
    | "on_packet"
    /** Fires a projectile from sam_spawn_projectile stops against a wall or an entity. Multiplayer: Fires on the host. */
    | "on_projectile_hit"
    /** Fires every game tick (50/sec), for every script that defines on_tick(event). Multiplayer: Fires on the host only (and in singleplayer). A client gets no tick: update HUD and panels when something changes and pass the player, because every call for a remote player is a network message. */
    | "on_tick"
    /** Fires a player starts an attack swing (any weapon). Multiplayer: Fires on the host, for every player. */
    | "player.on_attack_start"
    /** Fires a dead player becomes a ghost. Multiplayer: Fires on the host, for every player. */
    | "player.on_became_ghost"
    /** Fires before a player equips an item. Multiplayer: Fires on the equipping player's own machine, before anything is equipped or sent: the host for its own and splitscreen players, the joiner's own game for a joiner. In a joiner's game, use only functions that work there (reads of that player, and kinds any and local) and return false to refuse. A joiner whose game does not run S.A.M is never asked. */
    | "player.on_before_equip"
    /** Fires a player's melee swing has connected and the damage is decided, but not yet applied. Multiplayer: Fires on the host, for every player. */
    | "player.on_before_hit"
    /** Fires before an item the player walked over enters the inventory. Multiplayer: Fires on the host, for every player (a client's pickup is decided there). A grant with sam_grant_item never fires it. */
    | "player.on_before_item_pickup"
    /** Fires before a dead player is brought back. Multiplayer: Fires on the host only (or in splitscreen singleplayer), once per dead player, before the level change. Its answer applies on every machine; a joiner's game no longer asks its own scripts. */
    | "player.on_before_revive"
    /** Fires bleeding ticks damage on a player. Multiplayer: Fires on the host, for every player. */
    | "player.on_bleed_tick"
    /** Fires a player blocks a hit while defending with a shield (partial or full). Multiplayer: Fires on the host, for every player. */
    | "player.on_block"
    /** Fires a player uses the callout / ping command. Multiplayer: Fires on the host, for every player (a joiner's ping arrives there). A veto hides the ping from everyone; on a S.A.M joiner it also takes the marker off their own screen, but their ping sound has already played. A joiner without S.A.M keeps their own marker. */
    | "player.on_callout"
    /** Fires a player opens a chest. Multiplayer: Fires on the host, for every player. */
    | "player.on_chest_opened"
    /** Fires a player takes damage from any source. Multiplayer: Fires on the host, for every player. */
    | "player.on_damage_taken"
    /** Fires a player dies. Multiplayer: Fires on the host, for every player. */
    | "player.on_death"
    /** Fires a status effect is newly applied to a player (a genuine off→on transition, not a refresh). Multiplayer: Fires on the host, for every player. */
    | "player.on_effect_applied"
    /** Fires a status effect runs out on a player. Multiplayer: Fires on the host, for every player. */
    | "player.on_effect_expired"
    /** Fires a status effect ends (cleared or expired). Multiplayer: Fires on the host, for every player. */
    | "player.on_effect_removed"
    /** Fires a player equips an item. Multiplayer: Fires on the host, for every player (a joiner's equip arrives there as the game's own echo). */
    | "player.on_equip"
    /** Fires the party takes a ladder to a new floor. Multiplayer: Fires on the host, once for every connected player when the party takes a ladder; initiator is the player who climbed. */
    | "player.on_floor_change"
    /** Fires a player's game-over window opens on their own machine. Multiplayer: Fires on the dying player's own machine (a client's own game for a client) when that player's game-over window opens. In co-op that can be a death while others still live (survived tells you), not only the end of the run. The host never sees a remote player's. */
    | "player.on_game_over"
    /** Fires a player picks up gold. Multiplayer: Fires on the host, for every player. */
    | "player.on_gold_collected"
    /** Fires a player's melee weapon hits an entity. Multiplayer: Fires on the host, for every player (melee is resolved there). */
    | "player.on_hit"
    /** Fires hunger crosses a tier edge. Multiplayer: Fires on the host, for every player (hunger is counted there). */
    | "player.on_hunger_change"
    /** Fires a player buys from a shop. Multiplayer: Fires on the host, for every player. */
    | "player.on_item_bought"
    /** Fires a player's equipped item breaks. Multiplayer: Fires on the host, for every player. */
    | "player.on_item_broken"
    /** Fires a player drops an item. Multiplayer: Fires on the host, for every player. */
    | "player.on_item_dropped"
    /** Fires a player identifies an item. Multiplayer: Fires on the host, for every player, once per identification: directly for the host's own players, and as reported by a joiner's own game (appraisal, a scroll or spell, a curse revealed on equip, a carried sam_identify_item). A joiner whose game does not run S.A.M reports nothing, except a curse the host sees on its copy of their worn item. */
    | "player.on_item_identified"
    /** Fires a player picks an item up off the ground. Multiplayer: Fires on the host, for every player. */
    | "player.on_item_pickup"
    /** Fires a player sells to a shop. Multiplayer: Fires on the host, for every player. */
    | "player.on_item_sold"
    /** Fires a player uses a consumable (potion / scroll / food). Multiplayer: Fires on the host, for every player. A joiner whose game runs S.A.M asks the host before using anything deliberately, so a veto uses up nothing and an allowed use happens one network round trip later. For a joiner without S.A.M (or in the first moment of a game) a veto stops the effect, but their game has already used the item up. */
    | "player.on_item_use"
    /** Fires a player's melee blow kills an entity. Multiplayer: Fires on the host, for every player (melee is resolved there). */
    | "player.on_kill"
    /** Fires a player gains a level. Multiplayer: Fires on the host, for every player. */
    | "player.on_level_up"
    /** Fires a player's melee swing connects with nothing. Multiplayer: Fires on the host, for every player. */
    | "player.on_miss"
    /** Fires a client joins the lobby. Multiplayer: Fires on the host, in the lobby, when a client joins. It is a lobby notification only: per-player state written here (sam_set_player_data) is cleared when the game starts, and anything sent to the joiner here is dropped. Set per-player state up in game.on_game_start or game.on_level_entered. */
    | "player.on_player_joined"
    /** Fires a client leaves, times out or is kicked, in the lobby or in a game. Multiplayer: Fires on the host, exactly once per departure: a leave, a keep-alive drop or a kick (including /kick), in the lobby or in a game. It pairs with player.on_player_joined. */
    | "player.on_player_left"
    /** Fires a dead player comes back: on a new floor, or when their ghost respawns. Multiplayer: Fires on the host. floor_load fires once the new level has loaded, just before game.on_level_entered; ghost_respawn fires when a dead player's ghost respawns (the host's own or a joiner's). */
    | "player.on_player_revived"
    /** Fires poison ticks damage on a player. Multiplayer: Fires on the host, for every player. */
    | "player.on_poison_tick"
    /** Fires a skill rank goes up. Multiplayer: Fires on the host, for every player. */
    | "player.on_proficiency_increased"
    /** Fires a player opens trade with a shopkeeper. Multiplayer: Fires on the host, for every player. */
    | "player.on_shop_entered"
    /** Fires a player casts a spell. Multiplayer: Fires on the host, for every player (a joiner's cast arrives before mana is spent). */
    | "player.on_spell_cast"
    /** Fires a cast fizzles or is blocked. Multiplayer: Fires on the host, for every player: a fizzle from the host's own cast, and not_enough_mana as reported by the caster's own game. A joiner whose game does not run S.A.M reports no mana failures. */
    | "player.on_spell_failed"
    /** Fires a player learns a spell. Multiplayer: Fires on the host, for every player, once, when the spell is actually learned: a joiner's own game reports it (including a carried sam_grant_spell). A joiner whose game does not run S.A.M reports nothing. */
    | "player.on_spell_learned"
    /** Fires an active status effect ticks. Multiplayer: Fires on the host, for every player (effect timers are counted there). */
    | "player.on_status_effect_tick"
    /** Fires a player unequips an item. Multiplayer: Fires on the host, for every player (a joiner's unequip arrives there as the game's own echo). */
    | "player.on_unequip"
    /** Fires a player gains XP from a kill. Multiplayer: Fires on the host, for every player. */
    | "player.on_xp_gained"
    /** Fires the player clicks a button you placed with sam_ui_button. Multiplayer: Fires on the host (in singleplayer, locally), with player = who clicked. A client's click is sent to the host and does not fire in the client's own scripts. */
    | "ui.on_click"
    /** Fires the player clicks a row in a list you built with sam_ui_list / sam_ui_list_add. Multiplayer: Fires on the host (in singleplayer, locally), with player = who clicked the row. A client's click is sent to the host and does not fire in the client's own scripts. */
    | "ui.on_select"
    /** Fires the player commits the contents of a text box placed with sam_ui_input. Multiplayer: Fires on the host (in singleplayer, locally), with player = who typed. A client's submit is sent to the host and does not fire in the client's own scripts. */
    | "ui.on_submit"
    /** Fires before a chest opens. Multiplayer: Fires on the host, for every player. */
    | "world.on_before_chest_open"
    /** Fires a boulder trap launches. Multiplayer: Fires on the host. */
    | "world.on_boulder_triggered"
    /** Fires a player first walks up close to a chest (proximity — fires once per chest, NOT on opening it). Multiplayer: Fires on the host, for every player. */
    | "world.on_chest_found"
    /** Fires a player opens a wooden door. Multiplayer: Fires on the host, for every player. */
    | "world.on_door_opened"
    /** Fires a player drinks from / uses a fountain. Multiplayer: Fires on the host, for every player. */
    | "world.on_fountain_used"
    /** Fires a thrown gadget lands and something must be built there. Multiplayer: Fires on the host (a thrown gadget lands there), for every player. */
    | "world.on_item_deployed"
    /** Fires a monster is summoned at runtime. Multiplayer: Fires on the host. */
    | "world.on_monster_spawned"
    /** Fires a player places an orb on a pedestal. Multiplayer: Fires on the host, for every player. */
    | "world.on_orb_placed"
    /** Fires a fired projectile strikes an entity. Multiplayer: Fires on the host. */
    | "world.on_projectile_hit"
    /** Fires a player uses a sink. Multiplayer: Fires on the host, for every player. */
    | "world.on_sink_used"
    /** Fires a player flips a lever or switch. Multiplayer: Fires on the host, for every player. */
    | "world.on_switch_toggled"
    /** Fires a player uses a teleporter (pad or tunnel-spell). Multiplayer: Fires on the host, for every player. */
    | "world.on_teleport"
    /** Fires an arrow / spike / magic trap fires. Multiplayer: Fires on the host. */
    | "world.on_trap_triggered";

  interface SamEvent {
    name: SamEventName;
    [field: string]: any;
  }
}

export {};
