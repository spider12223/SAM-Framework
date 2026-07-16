/*
 * Vanilla Patches catalog — the verified set of Barony data files a mod can patch,
 * with the REAL dot-paths into each.
 *
 * Every target here was confirmed to (a) exist in a stock install and (b) actually be
 * read by the engine at runtime; every field path was read off the real file. That
 * verification is the whole point: the patcher has no allowlist and silently skips a
 * target it can't resolve, so a wrong path in a curated dropdown is worse than making
 * someone type it — they'd trust it and get nothing.
 *
 * DELIBERATELY ABSENT (these were suggested before and are all dead):
 *   data/monstercurve.json, data/gameplaymodifiers.json
 *     - the engine reads ONLY these non-sample names, and neither ships. Patching them
 *       errors; patching the *_sample.json files that DO ship is worse still: the patch
 *       applies, logs success, and nothing ever reads it. Ship the file itself instead.
 *   data/shopkeeper.json, items/items_global_inventory.json
 *     - neither string exists anywhere in the engine. Shop data lives in
 *       data/shop_consumables.json, which is catalogued below.
 *
 * TARGET STRINGS ARE CANONICAL, NOT COSMETIC: forward slashes, no leading "/", no
 * "./", exact lowercase. Ops are grouped by the raw target string and the overlay is
 * truncated on write, so two mods spelling the same file differently silently destroy
 * each other's patches.
 */

export const VANILLA_TARGETS = [
  {
    path: "items/items.json",
    label: "Items & Spells Table",
    whatItControls: "The master item table: 524 entries governing gold value, weight, random-generation level, category, equip slot, models/icons and tooltip stats for every item in Barony. The single best patch target and the one to lead the catalog with.",
    rootKey: "items",
    entryKeyStyle: "lowercase snake_case internal names (wooden_shield, iron_sword, potion_healing, spellbook_forcebolt). Insertion order is identical to item_id 0..523. The same file has a second root key `spells` (223 entries, keys like spell_forcebolt).",
    exampleEntries: ["iron_sword", "steel_shield", "potion_healing", "ring_regeneration", "amulet_lifesaving", "spellbook_forcebolt", "gem_diamond", "artifact_sword"],
    caveat: "WARNING — six footguns. (1) NO add_entry on this target: Barony's loader walks d[\"items\"] MEMBER ORDER and maps entry i -> items[i] (mod_tools.cpp:990-1012), so a new key desyncs/overflows the positional mapping. Use the S.A.M custom-item system instead. (2) Patching gold_value, weight_value or item_level DISABLES STEAM ACHIEVEMENTS — those three feed the integrity hash (mod_tools.cpp:1138-1140, verdict at :1470). item_category, equip_slot, stats, item_images and model indices are achievement-safe. (3) stats.AC and stats.ATK are TOOLTIP-ONLY on vanilla items — real combat uses hardcoded `type ==` chains (items.cpp:4797/5726) and the JSON value is applied only when type >= NUMITEMS (custom items). Patching them makes the tooltip lie about real AC/ATK. (4) items.<entry>.item_id is a SILENT NO-OP — the apply loop writes by positional index and only logs a warning. (5) An unrecognised item_category string silently becomes GEM (no validation, final else at mod_tools.cpp:1088) — dropdown only, never free text. equip_slot falls back to NO_EQUIP. (6) 600000-byte static read buffer; the file is already 463,570 bytes (77.3% full). Overflow truncates the JSON mid-parse and the ENTIRE item table falls back to defaults.",
    noAddEntry: true, // loader maps member ORDER -> item_id; a new key desyncs every id after it
    fields: [
      { path: "items.<entry>.gold_value", label: "Gold Value", type: "integer", example: "45", note: "LIVE. Base shop/sell price and appraisal difficulty (items.cpp:5588, actplayer.cpp:8594). ACHIEVEMENT-BREAKING — feeds the integrity hash.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.weight_value", label: "Weight", type: "integer", example: "30", note: "LIVE. Per-unit inventory weight (items.cpp:5645-5649). ACHIEVEMENT-BREAKING — feeds the integrity hash.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.item_level", label: "Item Level (generation curve)", type: "integer", example: "8", note: "LIVE. Dungeon-depth band for random generation (items.cpp:649). Set -1 to exclude the item from all random drops (artifact_sword uses -1). ACHIEVEMENT-BREAKING — feeds the integrity hash.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.item_category", label: "Category", type: "string", example: "WEAPON", note: "LIVE, achievement-safe. DROPDOWN ONLY — one of ARMOR, WEAPON, SPELLBOOK, TOOL, POTION, GEM, FOOD, SCROLL, MAGICSTAFF, RING, THROWN, AMULET, TOME_SPELL, BOOK, SPELL_CAT. Any unrecognised string silently becomes GEM.", ops: ["edit_field", "remove_field"] },
      { path: "items.<entry>.equip_slot", label: "Equip Slot", type: "string", example: "mainhand", note: "LIVE, achievement-safe. DROPDOWN ONLY — mainhand, offhand, helm, torso, mask, gloves, boots, ring, cloak, amulet, spell, or \"\" (empty = not equippable). Unknown values fall back to NO_EQUIP.", ops: ["edit_field", "remove_field"] },
      { path: "items.<entry>.stats.no_stack", label: "Disable Stacking", type: "integer", example: "1", note: "LIVE (items.cpp:7155). Value 1 prevents the item stacking in inventory. Note the lowercase spelling — the only lowercase stats key.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.stats.UNSELLABLE", label: "Unsellable", type: "integer", example: "1", note: "LIVE (shops.cpp:471). Presence blocks selling the item to shopkeepers. Achievement-safe.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.stats.SHOP_EXCLUDE_FROM_CATEGORY_1", label: "Shop Exclude (curve 1)", type: "integer", example: "1", note: "LIVE (items.cpp:669-671). Compared against itemLevelCurveShop to exclude the item from shop stock.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.stats.SHOP_EXCLUDE_FROM_CATEGORY_2", label: "Shop Exclude (curve 2)", type: "integer", example: "1", note: "LIVE (items.cpp:679). Second shop-exclusion curve slot.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.stats.SEASONING_FOOD", label: "Seasoning / Food link", type: "integer", example: "359", note: "LIVE (interface.cpp:8510-8512). Item id of the ration this seasons into; drives the cooking/seasoning system.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.stats.RATE_OF_FIRE", label: "Rate of Fire", type: "integer", example: "100", note: "LIVE (playerinventory.cpp:5830). Ranged-weapon fire rate percentage.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.stats.ARMOR_PIERCE", label: "Armor Pierce", type: "integer", example: "1", note: "LIVE (playerinventory.cpp:5838). One of the few EFF/stat keys with a real consumer outside the tooltip system.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.stats.EFF_WEIGHT_BURDEN", label: "Weight Burden Multiplier", type: "integer", example: "50", note: "LIVE (actplayer.cpp:4474) — divided by 100.0 and multiplied into the player's speedFactor. One of the very few EFF_* stats with real gameplay effect.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.stats.AC", label: "Armor Class (TOOLTIP ONLY on vanilla items)", type: "integer", example: "4", note: "DISPLAY ONLY for vanilla items — Item::armorGetAC (items.cpp:5726) uses a hardcoded type== chain; the JSON value is applied only when type >= NUMITEMS (custom items). Patching desyncs the tooltip from real AC. Achievement-safe but misleading.", ops: ["edit_field", "multiply_field", "remove_field"], tooltipOnly: true },
      { path: "items.<entry>.stats.ATK", label: "Attack Power (TOOLTIP ONLY on vanilla items)", type: "integer", example: "5", note: "DISPLAY ONLY for vanilla items — Item::weaponGetAttack (items.cpp:4797) uses a hardcoded type== chain; JSON value applied only for custom items (type >= NUMITEMS).", ops: ["edit_field", "multiply_field", "remove_field"], tooltipOnly: true },
      { path: "items.<entry>.stats.SPELLBOOK_CAST_BONUS", label: "Spellbook Cast Bonus (marker — value inert)", type: "integer", example: "50", note: "Presence-checked only (castSpell.cpp:9069, playerinventory.cpp:5931). The real bonus comes from getSpellbookBaseINTBonus (castSpell.cpp:445), so editing the number does nothing. Roughly 150 distinct EFF_* keys exist and most (EFF_UNBREAKABLE, EFF_LIFESAVING, EFF_LEVITATION, EFF_REGENERATION) have no consumer outside the tooltip renderer — treat unlisted stats keys as display markers.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.tooltip.type", label: "Tooltip Template", type: "string", example: "tooltip_sword", note: "Selects the tooltip layout from item_tooltips.json. 125 distinct values in use (tooltip_sword, tooltip_armor_shield, tooltip_spellbook, tooltip_potion_healing...). `tooltip` has exactly one subkey: type. Achievement-safe.", ops: ["edit_field", "remove_field"] },
      { path: "items.<entry>.item_images", label: "Inventory Icons", type: "array", example: "[\"items/images/IronSword.png\"]", note: "Array of icon paths; array LENGTH sets items[i].variations (number of random appearance variants). ARRAY — replace wholesale with edit_field, or edit one element by numeric index (items.<entry>.item_images.0). add_entry/remove_field are silently skipped on arrays. Achievement-safe.", ops: ["edit_field", "remove_field"] },
      { path: "items.<entry>.icon_label_path", label: "Icon Label Overlay", type: "string", example: "images/ui/Inventory/potions/healing.png", note: "Optional — present on only 28 of 524 entries. Small overlay badge drawn on the inventory icon. edit_field on a missing path is skipped; use add_entry to introduce it. Achievement-safe.", ops: ["edit_field", "remove_field"] },
      { path: "items.<entry>.first_person_model_index", label: "First-Person Model Index", type: "integer", example: "317", note: "Model/sprite index for the held first-person view. Must reference an existing model index or rendering breaks. Achievement-safe.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.third_person_model_index", label: "Third-Person Model Index", type: "integer", example: "15", note: "Model index for the dropped/worn third-person view. Achievement-safe.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.third_person_model_index_short", label: "Third-Person Model (short races)", type: "integer", example: "6", note: "Optional — present on only 24 of 524 entries. Alternate third-person model for short races (gnome/goblin). Achievement-safe.", ops: ["edit_field", "multiply_field", "remove_field"] },
      { path: "items.<entry>.item_id", label: "Item ID (READ-ONLY — DO NOT EDIT)", type: "integer", example: "8", note: "SILENT NO-OP. The apply loop writes items[i] by positional index and ignores this value (mod_tools.cpp:990); editing only emits a log warning. Expose read-only or omit from the GUI entirely. Ids >= 5000 are the reserved S.A.M custom range.", ops: [], readOnly: true },
    ],
  },
  {
    path: "data/shop_consumables.json",
    label: "Shop Stock & Consumable Prices",
    whatItControls: "What each shop type stocks, how likely each item is to appear, how many, its condition/beatitude, and the global consumable price multiplier. This is the REAL shop target — it replaces the GUI's fabricated data/shopkeeper.json (mod_tools.cpp:9330).",
    rootKey: "store_types",
    entryKeyStyle: "Lowercase internal store names under store_types; then a `slots` OBJECT whose keys are NUMERIC STRINGS (\"0\",\"1\",...); then an `items` ARRAY indexed numerically. The two numeric levels mean different things — slots.<n> is an object key, items.<i> is an array index.",
    exampleEntries: ["arms_armor", "hats", "jewelry", "books", "potions", "staffs", "food", "hardware", "hunting", "general"],
    caveat: "MIXED SHAPE. store_types.<store>.slots.<n> is an object (add_entry works to add a slot), but .items is a real ARRAY — add_entry/remove_field on items.<i> are SILENTLY SKIPPED; only edit_field and multiply_field function there, and indices are positional, not stable ids. 65536-byte engine buffer vs 13,826 bytes on disk (9,491 compact) — comfortable headroom, add_entry on slots is safe. Achievement-safe.",
    fields: [
      { path: "consumable_buy_value_multiplier", label: "Consumable price multiplier (%)", type: "integer", example: "100", note: "LIVE (mod_tools.cpp:9363-9365), int percent. 100 = vanilla. The single global shop-price knob — set 50 to halve consumable prices. Best multiply_field candidate in the catalog.", ops: ["edit_field", "multiply_field", "remove_field", "add_entry"] },
      { path: "store_types.<store>.slots.<n>.trading_req", label: "Trading skill required for slot", type: "integer", example: "0", note: "LIVE (mod_tools.cpp:9426). Gates this stock slot behind a Trading skill level.", ops: ["edit_field", "multiply_field", "remove_field", "add_entry"] },
      { path: "store_types.<store>.slots.<n>.items.<i>.type", label: "Item internal name", type: "string", example: "scroll_removecurse", note: "LIVE (mod_tools.cpp:9440). Lowercase internal item name — same key style as items/items.json (food_bread, scroll_blank, food_fish). Swap this to change what a shop stocks. Array path: edit_field only.", ops: ["edit_field"], arrayBacked: true },
      { path: "store_types.<store>.slots.<n>.items.<i>.spawn_percent_chance", label: "Spawn chance (%)", type: "integer", example: "75", note: "LIVE (mod_tools.cpp:9600). Percent chance this item appears in the slot (vanilla values 75/80/90). Primary stock-rarity knob. Array path: edit_field/multiply_field only.", ops: ["edit_field", "multiply_field"], arrayBacked: true },
      { path: "store_types.<store>.slots.<n>.items.<i>.slot_weighted_chance", label: "Slot weighted chance", type: "integer", example: "1", note: "LIVE (mod_tools.hpp:308). Relative weight among competing items in the same slot; higher = picked more often.", ops: ["edit_field", "multiply_field"], arrayBacked: true },
      { path: "store_types.<store>.slots.<n>.items.<i>.count", label: "Possible stack counts", type: "array", example: "[1,2]", note: "LIVE (mod_tools.cpp:9538). Array of candidate counts, one picked at random (e.g. [1,2,3,4]). Replace the whole array via edit_field — element-level add/remove is skipped.", ops: ["edit_field"], arrayBacked: true },
      { path: "store_types.<store>.slots.<n>.items.<i>.drop_percent_chance", label: "Drop chance (%)", type: "integer", example: "0", note: "Chance the item is dropped rather than stocked. 0 throughout vanilla.", ops: ["edit_field", "multiply_field"], arrayBacked: true },
      { path: "store_types.<store>.slots.<n>.items.<i>.identified", label: "Stocked identified", type: "boolean", example: "true", note: "Whether the shop item comes pre-identified. Set false to force players to appraise shop goods — pairs naturally with data/appraisal_tables.json edits. BOOLEAN: edit_field must supply true/false, not a string; the patcher does no type checking.", ops: ["edit_field"], arrayBacked: true },
      { path: "store_types.<store>.slots.<n>.items.<i>.status", label: "Item condition", type: "array", example: "[\"excellent\"]", note: "Candidate condition strings, one picked at random. Replace the whole array via edit_field.", ops: ["edit_field"], arrayBacked: true },
      { path: "store_types.<store>.slots.<n>.items.<i>.beatitude", label: "Blessed/cursed values", type: "array", example: "[0]", note: "Candidate beatitude values; 0 = uncursed. Replace the whole array via edit_field, e.g. [-1] to stock cursed goods.", ops: ["edit_field"], arrayBacked: true },
    ],
  },
  {
    path: "data/entity_data.json",
    label: "Breakable Objects & Collider Damage Rules",
    whatItControls: "Which weapon/skill types damage each breakable collider, whether it burns, whether bombs attach, whether boulders smash it, and whether the minotaur can path through it. Genuine combat and level-interaction gameplay (mod_tools.cpp:10884).",
    rootKey: "entities",
    entryKeyStyle: "Two sub-tables. entities.collider_dmg_types keys are NUMERIC STRINGS (\"0\",\"1\",\"2\"...) that are real OBJECT KEYS, not array indices — add_entry works there. entities.collider_dmg_calcs keys are lowercase snake_case rule names.",
    exampleEntries: ["collider_dmg_types.0 (default)", "collider_dmg_types.1 (wood_barricade)", "collider_dmg_types.2 (mines_weakwall)", "collider_dmg_types.4 (spiderweb)", "collider_dmg_calcs.default", "collider_dmg_calcs.stone_wall", "collider_dmg_calcs.no_magic"],
    caveat: "WARNING — damage_calc is a STRING REFERENCE into entities.collider_dmg_calcs. Pointing it at a name that does not exist is a SILENT MISCONFIGURATION the patcher cannot catch: the edit applies, the log says success, and the rule quietly falls apart at runtime. Validate the value against the live key list in the GUI. bonus_damage_skills / resist_damage_skills are ARRAYS of PRO_* strings — replace wholesale with edit_field; element add/remove is skipped. 65536-byte engine buffer vs 48,030 bytes pretty (32,315 compact) — safe for field edits, but the patcher's 65000 cap means large add_entry batches can blow the overlay. Achievement-safe.",
    fields: [
      { path: "entities.collider_dmg_types.<n>.name", label: "Collider name", type: "string", example: "wood_barricade", note: "Identifies the breakable (default, wood_barricade, mines_weakwall, stone_wall, spiderweb). Read-only in practice — change damage_calc instead.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "entities.collider_dmg_types.<n>.damage_calc", label: "Damage rule reference", type: "string", example: "wood_wall", note: "Highest-leverage field here. Points at a key under entities.collider_dmg_calcs (default, wood_wall, stone_wall, cordage_wall). Repoint a breakable at a different rule set to change how it can be destroyed. MUST match an existing collider_dmg_calcs key exactly — a typo is silent.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "entities.collider_dmg_calcs.<rule>.burnable", label: "Burnable", type: "boolean", example: "false", note: "Whether fire destroys objects using this rule. Set true on wood_wall to let players burn barricades.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "entities.collider_dmg_calcs.<rule>.melee", label: "Damageable by melee", type: "boolean", example: "true", note: "Whether melee attacks damage this collider.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "entities.collider_dmg_calcs.<rule>.magic", label: "Damageable by magic", type: "boolean", example: "true", note: "Whether spells damage this collider. The no_magic rule exists purely to set this false.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "entities.collider_dmg_calcs.<rule>.minotaur_path_and_break", label: "Minotaur can break through", type: "boolean", example: "true", note: "Whether the minotaur can path through and smash this. Significant chase-difficulty knob (false on no_magic).", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "entities.collider_dmg_calcs.<rule>.bombs_attach", label: "Bombs can attach", type: "boolean", example: "true", note: "Whether bomb items can be attached to this collider.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "entities.collider_dmg_calcs.<rule>.boulder_destroy", label: "Destroyed by boulders", type: "boolean", example: "true", note: "Whether rolling boulders smash this collider.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "entities.collider_dmg_calcs.<rule>.bonus_damage_skills", label: "Skills dealing bonus damage", type: "array", example: "[\"PRO_AXE\",\"PRO_MACE\"]", note: "PRO_* skill strings that get bonus damage vs this collider. Replace the whole array via edit_field — element-level ops are skipped on arrays.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "entities.collider_dmg_calcs.<rule>.resist_damage_skills", label: "Skills dealing reduced damage", type: "array", example: "[\"PRO_SWORD\",\"PRO_POLEARM\"]", note: "PRO_* skill strings that are resisted. ABSENT on some rules (no_magic has no resist list) — edit_field on a missing path is silently skipped; use add_entry to introduce it.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "entities.collider_dmg_calcs.<rule>.minimap_appear_as_wall", label: "Shows as wall on minimap", type: "boolean", example: "true", note: "Minimap presentation, but it changes the navigation information the player gets — borderline gameplay.", ops: ["edit_field", "remove_field", "add_entry"] },
    ],
  },
  {
    path: "data/appraisal_tables.json",
    label: "Appraisal Difficulty & Timing",
    whatItControls: "How long appraising an item takes and what a given Appraisal skill level can appraise instantly, slowly, or not at all. The appraisal-difficulty knob (interface/identify_and_appraise.cpp:587).",
    rootKey: "(no single root — flat scalars fast_time_seconds / per_stat_mult plus two arrays: appraisal_times, appraisal_tables)",
    entryKeyStyle: "Array indices (numeric dot tokens). appraisal_tables is ordered HIGHEST skill first (index 0 = skill 50, index 9 = skill 0); appraisal_times is ordered HIGHEST value first (index 0 = value 3000, index 6 = value -1 catch-all).",
    exampleEntries: ["appraisal_tables.0 (skill 50)", "appraisal_tables.9 (skill 0)", "appraisal_times.0 (value 3000)", "appraisal_times.6 (value -1 catch-all)"],
    caveat: "WARNING — TWO HAZARDS. (1) FRAGILE LOADER: identify_and_appraise.cpp:618-621 hard-requires appraisal_times, appraisal_tables, fast_time_seconds AND per_stat_mult to ALL be present and bails out of the whole load if any is missing — a remove_field on any of those four silently reverts the ENTIRE appraisal config to defaults instead of erroring. Do not offer remove_field on this target's top-level keys. (2) CAP BUG: the engine's real read buffer is only 32000 bytes (identify_and_appraise.cpp:605) with NO size guard, while sam_patcher.cpp's safeMaxFor applies its 65000 default — an overlay between 32000 and 65000 passes the patcher and is then truncated by the engine, corrupting the file. The shipped file is only 1,270 bytes (920 compact) so realistic edits are safe, but safeMaxFor needs a 31000 entry. ARRAY-BACKED: only edit_field/multiply_field work on the indexed rows; add_entry/remove_field are silently skipped. Achievement-safe.",
    fields: [
      { path: "fast_time_seconds", label: "Fast appraisal time (seconds)", type: "integer", example: "5", note: "LIVE (identify_and_appraise.cpp:630, multiplied by TICKS_PER_SECOND). Time taken when the item is within the player's instant-appraise gold limit.", ops: ["edit_field", "multiply_field", "remove_field", "add_entry"] },
      { path: "per_stat_mult", label: "Per-stat speed multiplier", type: "integer", example: "1", note: "LIVE (identify_and_appraise.cpp:631, read as int). Scales how much the appraisal stat speeds up the process.", ops: ["edit_field", "multiply_field", "remove_field", "add_entry"] },
      { path: "appraisal_times.<index>.value", label: "Gold value breakpoint", type: "integer", example: "3000", note: "Item gold value at/above which this row's slow_time applies (3000 at index 0, 50 at index 5, -1 at index 6 = catch-all). Rows are ordered DESCENDING — keep that order or the lookup picks the wrong row.", ops: ["edit_field", "multiply_field"], arrayBacked: true },
      { path: "appraisal_times.<index>.slow_time_seconds", label: "Slow appraisal time (seconds)", type: "integer", example: "720", note: "Seconds to appraise an item at this value breakpoint when it exceeds the skill's instant limit (720 at index 0, 15 at index 6). The single best knob for making appraisal faster or slower overall — good multiply_field target.", ops: ["edit_field", "multiply_field"], arrayBacked: true },
      { path: "appraisal_tables.<index>.skill", label: "Appraisal skill breakpoint", type: "integer", example: "50", note: "Skill level this row applies to. Ordered descending: index 0 = skill 50, index 9 = skill 0.", ops: ["edit_field", "multiply_field"], arrayBacked: true },
      { path: "appraisal_tables.<index>.gold_value_limit", label: "Max appraisable gold value", type: "integer", example: "999999", note: "Highest item gold value this skill level can appraise at all (999999 at index 0, 30 at index 9). The core difficulty knob — raise it to let low skill appraise expensive items.", ops: ["edit_field", "multiply_field"], arrayBacked: true },
      { path: "appraisal_tables.<index>.fast_time_gold", label: "Instant-appraise gold ceiling", type: "integer", example: "300", note: "Items at/below this gold value are appraised in fast_time_seconds instead of the slow time (300 at index 0, 5 at index 9).", ops: ["edit_field", "multiply_field"], arrayBacked: true },
    ],
  },
  {
    path: "data/status_effects.json",
    label: "Status Effect Text & Icons (UI ONLY — NOT durations)",
    whatItControls: "Status effect display name, description text, HUD icon and tooltip width (ui/GameUI.cpp:5832). Presentation layer only.",
    rootKey: "effects",
    entryKeyStyle: "UPPERCASE EFF_* internal names under `effects`; SUSTAIN_* names under a second root key `sustained_effects`.",
    exampleEntries: ["EFF_HUNGER", "EFF_BURNING", "EFF_POISONED", "EFF_PARALYZED", "SUSTAIN_LIGHT"],
    caveat: "WARNING — DOES NOT CONTROL DURATIONS OR MAGNITUDES. The GUI's original premise was wrong. A full key census over all 164 effects yields exactly {id, internal_name, name, desc, img_path, img_from_spell_id, tooltip_width, never_display, use_entry_for_sustained_spell} — there is no duration field and no magnitude field anywhere in this file; those are hardcoded in C++. Real + engine-loaded + safely patchable (65536-byte buffer vs 45,980 bytes pretty / 37,405 compact), but a patch here changes on-screen text and icons and NOTHING else. Ship it labelled 'UI text' or not at all. Watch large add_entry batches against the 65000 cap. Achievement-safe.",
    fields: [
      { path: "effects.<EFF_NAME>.name", label: "Display name", type: "string", example: "Poisoned", note: "The effect name shown on the HUD/tooltip. Text only.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "effects.<EFF_NAME>.desc", label: "Description text", type: "string", example: "Losing health over time.", note: "Tooltip description. Text only — it describes hardcoded behaviour it does not implement, so a misleading edit here desyncs the player's understanding from reality.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "effects.<EFF_NAME>.img_path", label: "HUD icon path", type: "string", example: "images/ui/HUD/effects/poisoned.png", note: "Icon image for the effect. Art only.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "effects.<EFF_NAME>.tooltip_width", label: "Tooltip width (px)", type: "integer", example: "180", note: "Optional — present on only 46 of 164 entries. edit_field on a missing path is silently skipped; use add_entry to introduce it.", ops: ["edit_field", "multiply_field", "remove_field", "add_entry"] },
      { path: "effects.<EFF_NAME>.never_display", label: "Hide from HUD", type: "boolean", example: "true", note: "Optional — present on only 11 of 164 entries. Suppresses the effect from the HUD. Display only.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "effects.<EFF_NAME>.id", label: "Effect ID (READ-ONLY)", type: "integer", example: "3", note: "Engine effect index. Do not edit — it is an identity field, not a tunable.", ops: [], readOnly: true },
      { path: "effects.<EFF_NAME>.internal_name", label: "Internal name (READ-ONLY)", type: "string", example: "EFF_POISONED", note: "Mirrors the entry key. Do not edit.", ops: [], readOnly: true },
    ],
  },
  {
    path: "data/monster_data.json",
    label: "Monster Icons & Model Indexes (ART/UI ONLY — NOT stats)",
    whatItControls: "Ally-HUD icon images and model-index lists per monster variant (mod_tools.cpp:8433).",
    rootKey: "monsters",
    entryKeyStyle: "Lowercase monster names under `monsters`, each containing variant names; plus a separate specialNPCs sub-object.",
    exampleEntries: ["nothing", "human", "rat", "goblin", "slime", "troll"],
    caveat: "WARNING — CONTAINS NO MONSTER STATS. Do not catalog this as a stats or scaling target; the GUI's original premise was wrong. A full key census across every entry yields only {icon:166, models:136, localized_name, localized_short_name:5} — there is no HP, damage, speed or scaling field anywhere in this file. It is real, engine-loaded and safely patchable (65536-byte buffer vs 20,834 bytes pretty / 14,529 compact), but purely cosmetic. THERE IS NO VANILLA MONSTER-STAT PATCH TARGET AT ALL: monster stats live in data/custom-monsters/*, which ships only 5 *_sample.json files the engine never reads, referenced by name from data/monstercurve.json, which does not exist in a stock install. Monster stats/scaling require the modder to SHIP both files — they cannot be a Vanilla Patch. Achievement-safe.",
    fields: [
      { path: "monsters.<monster>.<variant>.icon", label: "Ally HUD icon path", type: "string", example: "images/ui/HUD/allies/rat.png", note: "Icon shown for this monster in the ally HUD. Art only.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "monsters.<monster>.<variant>.models", label: "Model index list", type: "array", example: "[210,211,212]", note: "Model/sprite indexes used to render the monster. ARRAY — replace wholesale via edit_field; element add/remove is silently skipped. Indexes must exist or rendering breaks.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "monsters.<monster>.<variant>.localized_name", label: "Display name", type: "string", example: "Rat", note: "Localized monster name. Text only.", ops: ["edit_field", "remove_field", "add_entry"] },
      { path: "monsters.<monster>.<variant>.localized_short_name", label: "Short display name", type: "string", example: "Rat", note: "Optional — present on only 5 entries. Compact name for tight HUD space. edit_field on a missing path is skipped; use add_entry.", ops: ["edit_field", "remove_field", "add_entry"] },
    ],
  },
];

/** Catalog entry for a target path, or undefined for a hand-typed one. */
export function vanillaTarget(path) {
  return VANILLA_TARGETS.find((t) => t.path === path);
}

/** Ops that are actually safe for a field. Empty = read-only, don't patch it. */
export function opsForField(field) {
  return field?.ops ?? [];
}

/**
 * Fill a path template's <placeholders> with a concrete entry key.
 * "items.<entry>.gold_value" + "iron_sword" -> "items.iron_sword.gold_value"
 */
export function fillPath(template, entry) {
  if (!entry) return template;
  return template.replace(/<[^>]+>/, entry);
}
