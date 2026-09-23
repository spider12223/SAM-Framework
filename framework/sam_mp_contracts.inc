// S.A.M: how every script function behaves in multiplayer. See sam_net.hpp for the kinds.
//
// SAM_MP(function, kind, target, argument, refusal)
//   kind     Host | Owner | Screen | Read | All | Local | Any
//   target   None | Player | Uid | Item | OptPlayer   (which argument names the player)
//   refusal  False | Nil | Zero | Empty | None        (what a refused call returns)
//
// Every function registered with samLuaRegister / samJsRegister must have a line here, and
// its samApi.js entry must carry the same kind as `mp` (tools/gen_api_docs.mjs checks both).
// A NEW function: decide its kind first. If it is about a player, it is almost never Any.
//
// CHOOSING THE REFUSAL. It has to be a value the function can NEVER legitimately return, or a
// script reads a refusal as a real answer and acts on it -- the warning goes to the log, never
// into the script's control flow. 0 is a real answer for a stat, an effect strength, a light
// level, a kill count and a move multiplier; false is a real answer for every "is it?" question.
// Those use None: no value at all in Lua, undefined in JavaScript. Nil is not enough on its own
// for a number, because JavaScript coerces null to 0 and `sam_get_stat(p, "HP") <= 0` would
// still be true after a refusal; None is the one value that compares false in both runtimes.


// ---- Camera
SAM_MP(sam_get_camera,                    Local,  Player,    1, Nil)
SAM_MP(sam_reset_camera,                  Screen, Player,    1, False)
SAM_MP(sam_set_camera_angle,              Screen, Player,    1, False)
SAM_MP(sam_set_camera_collision,          Screen, Player,    1, False)
SAM_MP(sam_set_camera_offset,             Screen, Player,    1, False)
SAM_MP(sam_set_camera_position,           Screen, Player,    1, False)
SAM_MP(sam_set_camera_target,             Screen, Player,    1, False)
SAM_MP(sam_show_own_body,                 Screen, Player,    1, False)

// ---- Combat
SAM_MP(sam_break_armor,                   Host,   None,      0, False)
SAM_MP(sam_consume_mp,                    Host,   None,      0, False)
SAM_MP(sam_drain_mp,                      Host,   None,      0, False)
SAM_MP(sam_get_attack,                    Read,   Uid,       1, Nil)
SAM_MP(sam_get_bonus_attack_vs,           Read,   Uid,       1, Nil)
SAM_MP(sam_get_healring,                  Read,   Uid,       1, Nil)
SAM_MP(sam_get_hp,                        Read,   Uid,       1, Nil)
SAM_MP(sam_get_max_hp,                    Read,   Uid,       1, Nil)
SAM_MP(sam_get_max_mp,                    Read,   Uid,       1, Nil)
SAM_MP(sam_get_mp,                        Read,   Uid,       1, Nil)
SAM_MP(sam_get_ranged_attack,             Read,   Uid,       1, Nil)
SAM_MP(sam_get_regen_interval,            Read,   Uid,       1, Nil)
SAM_MP(sam_get_thrown_attack,             Read,   Uid,       1, Nil)
SAM_MP(sam_is_parrying,                   Host,   None,      0, None)
SAM_MP(sam_mod_mp,                        Host,   None,      0, Nil)
SAM_MP(sam_obituary,                      Host,   None,      0, False)
SAM_MP(sam_revive_player,                 Host,   None,      0, False)
SAM_MP(sam_set_defending,                 Host,   None,      0, False)
SAM_MP(sam_set_parry,                     Host,   None,      0, False)
SAM_MP(sam_spawn_projectile,              Host,   None,      0, Nil)

// ---- Context
SAM_MP(sam_get_date,                      Local,  None,      0, Nil)
SAM_MP(sam_get_fps,                       Local,  None,      0, Zero)
SAM_MP(sam_get_level_info,                Any,    None,      0, Nil)
SAM_MP(sam_get_mods,                      Local,  None,      0, Empty)
SAM_MP(sam_get_real_time,                 Local,  None,      0, Zero)
SAM_MP(sam_get_run_time,                  Local,  None,      0, Zero)
SAM_MP(sam_get_tick_rate,                 Any,    None,      0, Zero)
SAM_MP(sam_is_in_game,                    Local,  None,      0, False)
SAM_MP(sam_is_loading,                    Local,  None,      0, False)
SAM_MP(sam_is_mod_loaded,                 Local,  None,      0, False)
SAM_MP(sam_is_paused,                     Local,  None,      0, False)

// ---- Custom events
SAM_MP(sam_fire_hook,                     Host,   None,      0, Zero)
SAM_MP(sam_register_hook,                 Any,    None,      0, None)

// ---- Damage
SAM_MP(sam_add_damage_multiplier,         Host,   None,      0, False)
SAM_MP(sam_clear_species_damage_resist,   All,    None,      0, Zero)
SAM_MP(sam_deal_damage,                   Host,   None,      0, False)
SAM_MP(sam_deal_damage_typed,             Host,   None,      0, Zero)
SAM_MP(sam_get_damage_resist,             Read,   Uid,       1, Nil)
SAM_MP(sam_get_magic_resist,              Read,   Uid,       1, Nil)
SAM_MP(sam_heal,                          Host,   None,      0, Nil)
SAM_MP(sam_modify_damage,                 Host,   None,      0, None)
SAM_MP(sam_preview_damage,                Host,   None,      0, Nil)
SAM_MP(sam_set_species_damage_resist,     All,    None,      0, False)

// ---- Entities
SAM_MP(sam_get_distance,                  Any,    None,      0, Nil)
SAM_MP(sam_get_distance_to,               Any,    None,      0, Nil)
SAM_MP(sam_get_entity_size,               Any,    None,      0, Nil)
SAM_MP(sam_get_entity_sprite,             Any,    None,      0, Nil)
SAM_MP(sam_get_entity_ticks,              Local,  Uid,       1, Nil)
SAM_MP(sam_get_entity_type,               Any,    None,      0, Nil)
SAM_MP(sam_get_facing,                    Any,    None,      0, Nil)
SAM_MP(sam_get_nearby_entities,           Any,    None,      0, Empty)
SAM_MP(sam_get_position_precise,          Any,    None,      0, Nil)
SAM_MP(sam_get_velocity,                  Any,    None,      0, Nil)

// ---- Game content
SAM_MP(sam_get_food_satiation,            Any,    None,      0, Nil)
SAM_MP(sam_get_item_info,                 Any,    None,      0, Nil)
SAM_MP(sam_list_items,                    Any,    None,      0, Empty)
SAM_MP(sam_list_monsters,                 Any,    None,      0, Empty)
SAM_MP(sam_list_spells,                   Any,    None,      0, Empty)

// ---- HUD
SAM_MP(sam_hud_bar,                       Screen, OptPlayer, 8, False)
SAM_MP(sam_hud_clear,                     Screen, OptPlayer, 2, False)
SAM_MP(sam_hud_text,                      Screen, OptPlayer, 6, False)

// ---- Hooks
SAM_MP(sam_modify_monster_damage,         Host,   None,      0, False)
SAM_MP(sam_modify_value,                  Host,   None,      0, False)

// ---- Input
SAM_MP(sam_get_action_binding,            Read,   Player,    1, Nil)
SAM_MP(sam_is_action_held,                Read,   Player,    1, None)
SAM_MP(sam_is_key_held,                   Read,   Player,    2, None)
SAM_MP(sam_register_action,               All,    None,      0, False)

// ---- Inventory
SAM_MP(sam_can_items_stack,               Read,   Player,    1, Nil)
SAM_MP(sam_can_unequip,                   Read,   Player,    1, Nil)
SAM_MP(sam_get_equipped_item,             Read,   Player,    1, Nil)
SAM_MP(sam_get_equipped_item_id,          Read,   Player,    1, Nil)
SAM_MP(sam_get_inventory_count,           Read,   Player,    1, Nil)
SAM_MP(sam_get_item,                      Read,   Item,      1, Nil)
SAM_MP(sam_get_item_ac,                   Read,   Item,      1, Nil)
SAM_MP(sam_get_item_attack,               Read,   Item,      1, Nil)
SAM_MP(sam_get_item_name,                 Read,   Item,      1, Nil)
SAM_MP(sam_get_item_owner,                Read,   Item,      1, Nil)
SAM_MP(sam_get_item_slot,                 Any,    None,      0, Nil)
SAM_MP(sam_get_item_value,                Read,   Item,      1, Nil)
SAM_MP(sam_get_item_weight,               Read,   Item,      1, Nil)
SAM_MP(sam_get_max_stack,                 Read,   Player,    1, Nil)
SAM_MP(sam_identify_item,                 Owner,  Item,      2, False)
SAM_MP(sam_inventory_has_space,           Read,   Player,    1, Nil)
SAM_MP(sam_is_better_armor,               Read,   Item,      1, False)
SAM_MP(sam_is_better_weapon,              Read,   Item,      1, False)
SAM_MP(sam_is_item_equipped,              Read,   Player,    1, Nil)
SAM_MP(sam_is_melee_weapon,               Read,   Item,      1, False)
SAM_MP(sam_is_potion_bad,                 Read,   Item,      1, False)
SAM_MP(sam_is_ranged_weapon,              Any,    None,      0, False)
SAM_MP(sam_is_shield,                     Read,   Item,      1, False)
SAM_MP(sam_item_has_trait,                Any,    None,      0, False)
SAM_MP(sam_set_item_appearance,           Owner,  Item,      1, False)
SAM_MP(sam_set_item_beatitude,            Owner,  Item,      1, False)
SAM_MP(sam_set_item_count,                Owner,  Item,      1, False)
SAM_MP(sam_set_item_droppable,            Owner,  Item,      1, False)
SAM_MP(sam_set_item_owner,                Owner,  Item,      1, False)
SAM_MP(sam_set_item_status,               Owner,  Item,      1, False)

// ---- Live patching
SAM_MP(sam_add_class_passive,             All,    None,      0, False)
SAM_MP(sam_patch_class,                   All,    None,      0, False)
SAM_MP(sam_patch_item,                    All,    None,      0, False)
SAM_MP(sam_patch_monster,                 Host,   None,      0, False)
SAM_MP(sam_remove_class_passive,          All,    None,      0, False)
SAM_MP(sam_unpatch_class,                 All,    None,      0, False)

// ---- Logging
SAM_MP(sam_log,                           Local,  None,      0, None)
SAM_MP(sam_message,                       Host,   None,      0, False)

// ---- Loot
SAM_MP(sam_add_item_to_container,         Host,   None,      0, Nil)
SAM_MP(sam_get_loot_pool,                 Any,    None,      0, Nil)
SAM_MP(sam_remove_item_from_container,    Host,   None,      0, False)
SAM_MP(sam_roll_loot,                     Host,   None,      0, None)
SAM_MP(sam_set_loot_category_weight,      All,    None,      0, False)
SAM_MP(sam_set_loot_context,              All,    None,      0, False)
SAM_MP(sam_set_loot_fallback,             All,    None,      0, False)
SAM_MP(sam_set_loot_floor_range,          All,    None,      0, False)
SAM_MP(sam_set_loot_weight,               All,    None,      0, False)
SAM_MP(sam_set_shop_stock,                Host,   None,      0, False)
SAM_MP(sam_set_shop_type,                 Host,   None,      0, False)
SAM_MP(sam_set_spell_droppable,           All,    None,      0, False)

// ---- Mechanisms
SAM_MP(sam_power_entity,                  Host,   None,      0, False)
SAM_MP(sam_set_door,                      Host,   None,      0, False)
SAM_MP(sam_set_door_locked,               Host,   None,      0, False)
SAM_MP(sam_toggle_switch,                 Host,   None,      0, False)

// ---- Monsters
SAM_MP(sam_alert_allies,                  Host,   None,      0, False)
SAM_MP(sam_apply_monster_effect,          Host,   None,      0, False)
SAM_MP(sam_clear_monster_target,          Host,   None,      0, False)
SAM_MP(sam_get_monster_data,              Host,   None,      0, Nil)
SAM_MP(sam_get_monster_effect_duration,   Host,   None,      0, Nil)
SAM_MP(sam_get_monster_effect_strength,   Host,   None,      0, None)
SAM_MP(sam_get_monster_effects,           Host,   None,      0, Empty)
SAM_MP(sam_get_monster_stat,              Host,   None,      0, None)
SAM_MP(sam_get_monster_target,            Host,   None,      0, Nil)
SAM_MP(sam_get_monster_target_uid,        Host,   None,      0, None)
SAM_MP(sam_kill_monster,                  Host,   None,      0, False)
SAM_MP(sam_monster_attack,                Host,   None,      0, False)
SAM_MP(sam_monster_can_wield,             Host,   None,      0, None)
SAM_MP(sam_monster_charge,                Host,   None,      0, False)
SAM_MP(sam_monster_equip,                 Host,   None,      0, False)
SAM_MP(sam_monster_face,                  Host,   None,      0, False)
SAM_MP(sam_monster_has_effect,            Host,   None,      0, None)
SAM_MP(sam_monster_path_to,               Host,   None,      0, False)
SAM_MP(sam_monster_unequip,               Host,   None,      0, False)
SAM_MP(sam_remove_monster_effect,         Host,   None,      0, False)
SAM_MP(sam_set_monster_data,              Host,   None,      0, False)
SAM_MP(sam_set_monster_name,              Host,   None,      0, False)
SAM_MP(sam_set_monster_stat,              Host,   None,      0, False)
SAM_MP(sam_set_monster_target,            Host,   None,      0, False)
SAM_MP(sam_set_monster_target_uid,        Host,   None,      0, False)
SAM_MP(sam_spawn_monsters,                Host,   None,      0, Zero)

// ---- Multiplayer
SAM_MP(sam_test_done,                     Local,  None,      0, False)
SAM_MP(sam_get_game_speed,                Any,    None,      0, Zero)
SAM_MP(sam_set_game_speed,                Local,  None,      0, False)
SAM_MP(sam_is_host,                       Local,  None,      0, False)
SAM_MP(sam_local_player,                  Local,  None,      0, Zero)
SAM_MP(sam_player_count,                  Any,    None,      0, Zero)

// ---- Networking
SAM_MP(sam_send_packet,                   Any,    None,      0, False)

// ---- Panels
SAM_MP(sam_ui_button,                     Screen, OptPlayer, 8, False)
SAM_MP(sam_ui_clear,                      Screen, OptPlayer, 2, False)
SAM_MP(sam_ui_close,                      Screen, OptPlayer, 2, False)
SAM_MP(sam_ui_font,                       Screen, OptPlayer, 4, False)
SAM_MP(sam_ui_image,                      Screen, OptPlayer, 9, False)
SAM_MP(sam_ui_input,                      Screen, OptPlayer, 8, False)
SAM_MP(sam_ui_input_text,                 Read,   Player,    3, Nil)
SAM_MP(sam_ui_is_open,                    Read,   Player,    2, None)
SAM_MP(sam_ui_label,                      Screen, OptPlayer, 8, False)
SAM_MP(sam_ui_list,                       Screen, OptPlayer, 7, False)
SAM_MP(sam_ui_list_add,                   Screen, OptPlayer, 6, False)
SAM_MP(sam_ui_list_clear,                 Screen, OptPlayer, 3, False)
SAM_MP(sam_ui_list_row_height,            Screen, OptPlayer, 4, False)
SAM_MP(sam_ui_open,                       Screen, OptPlayer, 8, False)
SAM_MP(sam_ui_panel_style,                Screen, OptPlayer, 5, False)
SAM_MP(sam_ui_text_size,                  Any,    None,      0, Nil)

// ---- Persistence
SAM_MP(sam_delete_data,                   Local,  None,      0, False)
SAM_MP(sam_get_player_data,               Host,   None,      0, Nil)
SAM_MP(sam_has_data,                      Local,  None,      0, False)
SAM_MP(sam_list_data_keys,                Local,  None,      0, Empty)
SAM_MP(sam_load_data,                     Local,  None,      0, Nil)
SAM_MP(sam_save_data,                     Local,  None,      0, False)
SAM_MP(sam_set_player_data,               Host,   None,      0, None)
SAM_MP(sam_world_bytes,                   Host,   None,      0, Nil)
SAM_MP(sam_world_bytes_free,              Host,   None,      0, Nil)
SAM_MP(sam_world_clear,                   Host,   None,      0, False)
SAM_MP(sam_world_keys,                    Host,   None,      0, Empty)
SAM_MP(sam_world_load,                    Host,   None,      0, Nil)
SAM_MP(sam_world_save,                    Host,   None,      0, False)

// ---- Pictures
SAM_MP(sam_clear_model,                   Host,   None,      0, False)
SAM_MP(sam_get_entity_flag,               Any,    None,      0, False)
SAM_MP(sam_get_image_size,                Any,    None,      0, Nil)
SAM_MP(sam_get_model,                     Any,    None,      0, Nil)
SAM_MP(sam_get_scale,                     Any,    None,      0, Nil)
SAM_MP(sam_hide_image,                    Screen, Player,    1, False)
SAM_MP(sam_hud_image,                     Screen, OptPlayer, 8, False)
SAM_MP(sam_is_visible,                    Any,    None,      0, False)
SAM_MP(sam_set_elevation,                 Host,   None,      0, False)
SAM_MP(sam_set_entity_flag,               Host,   None,      0, False)
SAM_MP(sam_set_entity_size,               Host,   None,      0, False)
SAM_MP(sam_set_model,                     Host,   None,      0, False)
SAM_MP(sam_set_scale,                     Host,   None,      0, False)
SAM_MP(sam_set_visible,                   Host,   None,      0, False)
SAM_MP(sam_show_image,                    Screen, Player,    1, False)
SAM_MP(sam_show_image_at,                 Screen, Player,    1, False)

// ---- Player state
SAM_MP(sam_add_move_speed,                Host,   None,      0, None)
SAM_MP(sam_get_class,                     Any,    None,      0, Nil)
SAM_MP(sam_get_floor,                     Any,    None,      0, Zero)
SAM_MP(sam_get_kills,                     Host,   None,      0, None)
SAM_MP(sam_get_move_speed,                Any,    None,      0, Zero)
SAM_MP(sam_get_race,                      Any,    None,      0, Nil)
SAM_MP(sam_get_stat,                      Read,   Player,    1, None)
SAM_MP(sam_get_time_played,               Local,  None,      0, Zero)
SAM_MP(sam_is_defending,                  Read,   Player,    1, None)
SAM_MP(sam_level_up,                      Host,   None,      0, False)
SAM_MP(sam_play_sound,                    Host,   None,      0, False)
SAM_MP(sam_set_move_speed,                Host,   None,      0, False)
SAM_MP(sam_set_stat,                      Host,   None,      0, False)

// ---- Presentation
SAM_MP(sam_camera_shake,                  Host,   None,      0, False)
SAM_MP(sam_damage_number,                 Host,   None,      0, False)
SAM_MP(sam_gib,                           Host,   None,      0, False)
SAM_MP(sam_hitstop,                       Host,   None,      0, False)
SAM_MP(sam_impact_frame,                  Screen, Player,    1, False)
SAM_MP(sam_play_sound_at,                 Host,   None,      0, False)
SAM_MP(sam_play_sound_entity,             Host,   None,      0, False)
SAM_MP(sam_screen_flash,                  Screen, Player,    1, False)
SAM_MP(sam_spawn_particle,                Host,   None,      0, False)

// ---- Rewards
SAM_MP(sam_get_item_category,             Any,    None,      0, Nil)
SAM_MP(sam_grant_gold,                    Host,   None,      0, False)
SAM_MP(sam_grant_item,                    Host,   None,      0, False)
SAM_MP(sam_item_id,                       Any,    None,      0, Nil)
SAM_MP(sam_spawn_item,                    Host,   None,      0, Nil)

// ---- Rules
SAM_MP(sam_add_monster_stat_modifier,     Host,   None,      0, False)
SAM_MP(sam_add_stat_modifier,             Host,   None,      0, False)
SAM_MP(sam_clear_immunities,              Host,   None,      0, Zero)
SAM_MP(sam_clear_stat_modifiers,          Host,   None,      0, Zero)
SAM_MP(sam_clear_xp_curve,                Host,   None,      0, Zero)
SAM_MP(sam_get_stat_modifier,             Host,   None,      0, Nil)
SAM_MP(sam_get_xp_threshold,              Any,    None,      0, Zero)
SAM_MP(sam_grant_xp,                      Host,   None,      0, Nil)
SAM_MP(sam_is_immune,                     Host,   None,      0, None)
SAM_MP(sam_remove_monster_stat_modifier,  Host,   None,      0, Zero)
SAM_MP(sam_remove_stat_modifier,          Host,   None,      0, Zero)
SAM_MP(sam_set_immunity,                  Host,   None,      0, False)
SAM_MP(sam_set_monster_immunity,          Host,   None,      0, False)
SAM_MP(sam_set_species_immunity,          Host,   None,      0, False)
SAM_MP(sam_set_xp_curve,                  Host,   None,      0, False)

// ---- Settings
SAM_MP(sam_get_setting,                   Local,  None,      0, None)
SAM_MP(sam_list_settings,                 Local,  None,      0, Empty)
SAM_MP(sam_register_setting,              All,    None,      0, False)
SAM_MP(sam_set_setting,                   Local,  None,      0, False)

// ---- Sound & music
SAM_MP(sam_get_music,                     Local,  None,      0, Nil)
SAM_MP(sam_list_music,                    Any,    None,      0, Nil)
SAM_MP(sam_list_sounds,                   Any,    None,      0, Nil)
SAM_MP(sam_play_music,                    Host,   None,      0, False)
SAM_MP(sam_stop_music,                    Host,   None,      0, False)
SAM_MP(sam_stop_sound,                    Host,   None,      0, Zero)

// ---- Spells
SAM_MP(sam_cast_spell,                    Host,   None,      0, False)
SAM_MP(sam_cast_spell_at,                 Host,   None,      0, Nil)
SAM_MP(sam_cast_spell_pos,                Host,   None,      0, Nil)
SAM_MP(sam_get_spells,                    Read,   Player,    1, Nil)
SAM_MP(sam_get_tome_spell,                Read,   Item,      1, Nil)
SAM_MP(sam_grant_spell,                   Owner,  Player,    1, False)
SAM_MP(sam_monster_cast_spell,            Host,   None,      0, Nil)
SAM_MP(sam_player_knows_spell,            Read,   Player,    1, Nil)
SAM_MP(sam_remove_spell,                  Owner,  Player,    1, False)

// ---- Status effects
SAM_MP(sam_apply_effect,                  Host,   None,      0, False)
SAM_MP(sam_clear_effects,                 Host,   None,      0, Zero)
SAM_MP(sam_get_effect_duration,           Host,   None,      0, Nil)
SAM_MP(sam_get_effect_strength,           Read,   Player,    1, None)
SAM_MP(sam_get_effects,                   Host,   None,      0, Empty)
SAM_MP(sam_has_effect,                    Read,   Player,    1, None)
SAM_MP(sam_remove_effect,                 Host,   None,      0, False)
SAM_MP(sam_set_effect_duration,           Host,   None,      0, False)
SAM_MP(sam_set_effect_strength,           Host,   None,      0, False)

// ---- Terrain
SAM_MP(sam_find_entities,                 Any,    None,      0, Empty)
SAM_MP(sam_get_container_items,           Host,   None,      0, Nil)
SAM_MP(sam_get_light_at,                  Host,   None,      0, None)
SAM_MP(sam_get_tile,                      Any,    None,      0, Nil)
SAM_MP(sam_is_spawnable,                  Any,    None,      0, False)
SAM_MP(sam_is_tile_diggable,              Any,    None,      0, False)
SAM_MP(sam_line_of_sight,                 Any,    None,      0, Nil)
SAM_MP(sam_set_tile,                      Host,   None,      0, False)
SAM_MP(sam_tiles_connected,               Any,    None,      0, False)

// ---- Timers
SAM_MP(sam_cancel_timer,                  Host,   None,      0, None)
SAM_MP(sam_set_repeating_timer,           Host,   None,      0, None)
SAM_MP(sam_set_timer,                     Host,   None,      0, None)

// ---- Truth
SAM_MP(sam_get_ac,                        Read,   Uid,       1, Nil)
SAM_MP(sam_get_effective_stat,            Read,   Uid,       1, Nil)
SAM_MP(sam_get_flag,                      Any,    None,      0, False)
SAM_MP(sam_get_monster_name,              Host,   None,      0, Nil)
SAM_MP(sam_get_monster_type,              Host,   None,      0, Nil)
SAM_MP(sam_get_seed,                      Any,    None,      0, Zero)
SAM_MP(sam_get_skill,                     Read,   Uid,       1, None)
SAM_MP(sam_is_enemy,                      Host,   None,      0, None)
SAM_MP(sam_is_friend,                     Host,   None,      0, None)
SAM_MP(sam_is_ghost,                      Read,   Player,    1, None)
SAM_MP(sam_is_spirit_ghost,               Read,   Player,    1, None)
SAM_MP(sam_monster_has_trait,             Host,   None,      0, None)
SAM_MP(sam_random,                        Host,   None,      0, Nil)
SAM_MP(sam_random_chance,                 Host,   None,      0, None)
SAM_MP(sam_random_float,                  Host,   None,      0, Nil)
SAM_MP(sam_random_from_list,              Host,   None,      0, Nil)
SAM_MP(sam_random_weighted,               Host,   None,      0, Nil)

// ---- World
SAM_MP(sam_apply_force,                   Host,   None,      0, False)
SAM_MP(sam_can_stand,                     Host,   None,      0, Nil)
SAM_MP(sam_companion_punch,               Host,   None,      0, False)
SAM_MP(sam_get_exit_position,             Any,    None,      0, Nil)
SAM_MP(sam_get_inventory,                 Read,   Player,    1, Nil)
SAM_MP(sam_get_map_flags,                 Any,    None,      0, Nil)
SAM_MP(sam_get_map_seed,                  Any,    None,      0, Zero)
SAM_MP(sam_get_playable_bounds,           Any,    None,      0, Nil)
SAM_MP(sam_get_player_uid,                Any,    None,      0, Nil)
SAM_MP(sam_get_position,                  Any,    None,      0, Nil)
SAM_MP(sam_is_damage_immune,              Host,   None,      0, Nil)
SAM_MP(sam_is_dark_level,                 Any,    None,      0, False)
SAM_MP(sam_move_entity,                   Host,   None,      0, Nil)
SAM_MP(sam_remove_entity,                 Host,   None,      0, False)
SAM_MP(sam_remove_item,                   Owner,  Item,      1, False)
SAM_MP(sam_set_chest_stash,               Host,   None,      0, False)
SAM_MP(sam_set_damage_immune,             Host,   None,      0, False)
SAM_MP(sam_set_on_fire,                   Host,   None,      0, False)
SAM_MP(sam_set_position,                  Host,   None,      0, False)
SAM_MP(sam_spawn_companion,               Host,   None,      0, Nil)
SAM_MP(sam_spawn_monster,                 Host,   None,      0, Nil)
SAM_MP(sam_spawn_portal,                  Host,   None,      0, Nil)
SAM_MP(sam_travel_to_level,               Host,   None,      0, False)

// ---- Your own logic
SAM_MP(sam_attach_behavior,               Host,   None,      0, False)
SAM_MP(sam_detach_behavior,               Host,   None,      0, False)
SAM_MP(sam_get_entity_facing,             Any,    None,      0, Nil)
SAM_MP(sam_look_at,                       Host,   None,      0, False)
SAM_MP(sam_register_behavior,             Any,    None,      0, False)
SAM_MP(sam_set_entity_facing,             Host,   None,      0, False)
SAM_MP(sam_spawn_entity,                  Host,   None,      0, Nil)
