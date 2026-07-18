/*
 * Insertable behavior-script snippets for the in-tool script editor.
 * Each has a Lua and a JS form (the same global sam_* API is available in both;
 * .ts scripts are transpiled to JS, so the JS form applies there too).
 * These are illustrative starters — the API Reference panel is the authoritative
 * source for exact function signatures.
 */
export const SNIPPETS = [
  {
    title: 'Starter skeleton',
    desc: 'The two entry points: on_event(event) dispatched by event.name, and on_tick(event).',
    lua: `-- Behavior script. Both handlers are optional.
function on_event(event)
    if event.name == "player.on_hit" then
        -- event.player, event.target_uid, event.damage
    elseif event.name == "player.on_level_up" then
        -- react to a level-up
    end
end

function on_tick(event)
    -- runs every game tick (50/sec). event.tick_count is available.
end`,
    js: `// Behavior script. Both handlers are optional.
function on_event(event) {
    if (event.name === "player.on_hit") {
        // event.player, event.target_uid, event.damage
    } else if (event.name === "player.on_level_up") {
        // react to a level-up
    }
}

function on_tick(event) {
    // runs every game tick (50/sec). event.tick_count is available.
}`,
  },
  {
    title: 'Proc bonus damage on hit',
    desc: 'A % chance on each melee hit to deal extra damage to the target.',
    lua: `function on_event(event)
    if event.name == "player.on_hit" then
        if math.random(100) <= 25 then           -- 25% chance
            sam_deal_damage(event.target_uid, 8)  -- bonus damage
            sam_message(event.player, "Critical strike!")
        end
    end
end`,
    js: `function on_event(event) {
    if (event.name === "player.on_hit") {
        if (Math.floor(Math.random() * 100) < 25) {   // 25% chance
            sam_deal_damage(event.target_uid, 8);      // bonus damage
            sam_message(event.player, "Critical strike!");
        }
    }
}`,
  },
  {
    title: 'Heal on block',
    desc: 'Restore a little HP whenever the player blocks with a shield.',
    lua: `function on_event(event)
    if event.name == "player.on_block" then
        local p = event.player
        local hp = sam_get_stat(p, "HP")
        local maxhp = sam_get_stat(p, "MAXHP")
        sam_set_stat(p, "HP", math.min(maxhp, hp + 3))
    end
end`,
    js: `function on_event(event) {
    if (event.name === "player.on_block") {
        const p = event.player;
        const hp = sam_get_stat(p, "HP");
        const maxhp = sam_get_stat(p, "MAXHP");
        sam_set_stat(p, "HP", Math.min(maxhp, hp + 3));
    }
}`,
  },
  {
    title: 'Cancel / reduce incoming damage',
    desc: 'on_before_damage is the ONLY place damage can be changed — call sam_modify_damage().',
    lua: `function on_event(event)
    -- NOTE: no "player." prefix on this event.
    if event.name == "on_before_damage" then
        -- halve incoming damage while below 30% HP
        local hp = sam_get_stat(event.player, "HP")
        local maxhp = sam_get_stat(event.player, "MAXHP")
        if hp < maxhp * 0.3 then
            sam_modify_damage(math.floor(event.damage * 0.5))
        end
    end
end`,
    js: `function on_event(event) {
    // NOTE: no "player." prefix on this event.
    if (event.name === "on_before_damage") {
        // halve incoming damage while below 30% HP
        const hp = sam_get_stat(event.player, "HP");
        const maxhp = sam_get_stat(event.player, "MAXHP");
        if (hp < maxhp * 0.3) {
            sam_modify_damage(Math.floor(event.damage * 0.5));
        }
    }
}`,
  },
  {
    title: 'Persistent state (save / load)',
    desc: 'Per-mod persistence. Reset floor-scoped state on player.on_floor_change.',
    lua: `function on_event(event)
    if event.name == "on_monster_died" then
        local kills = (sam_load_data("kills") or 0) + 1
        sam_save_data("kills", kills)
        if kills % 10 == 0 then
            sam_grant_gold(event.killer_player or 0, 25)
        end
    elseif event.name == "player.on_floor_change" then
        sam_save_data("kills", 0)   -- reset per floor
    end
end`,
    js: `function on_event(event) {
    if (event.name === "on_monster_died") {
        const kills = (sam_load_data("kills") || 0) + 1;
        sam_save_data("kills", kills);
        if (kills % 10 === 0) {
            sam_grant_gold(event.killer_player || 0, 25);
        }
    } else if (event.name === "player.on_floor_change") {
        sam_save_data("kills", 0);   // reset per floor
    }
}`,
  },
  {
    title: 'Repeating timer',
    desc: 'Run logic on an interval (ticks; 50 = 1s). Cancel by id when done.',
    lua: `function on_event(event)
    if event.name == "player.on_equip" then
        -- tick a regen aura every second while equipped
        sam_set_repeating_timer("regen", 50, function()
            local p = 0
            sam_set_stat(p, "HP", sam_get_stat(p, "HP") + 1)
        end)
    elseif event.name == "player.on_unequip" then
        sam_cancel_timer("regen")
    end
end`,
    js: `function on_event(event) {
    if (event.name === "player.on_equip") {
        // tick a regen aura every second while equipped
        sam_set_repeating_timer("regen", 50, function () {
            const p = 0;
            sam_set_stat(p, "HP", sam_get_stat(p, "HP") + 1);
        });
    } else if (event.name === "player.on_unequip") {
        sam_cancel_timer("regen");
    }
}`,
  },
  {
    title: 'Equip-triggered self buff',
    desc: 'Apply a status effect to yourself on equip (effects target the local player).',
    lua: `function on_event(event)
    if event.name == "player.on_equip" then
        -- FAST / INVISIBLE / BLIND / CONFUSED ; duration in ticks (50/sec)
        sam_apply_effect(event.player, "FAST", 250)
    end
end`,
    js: `function on_event(event) {
    if (event.name === "player.on_equip") {
        // FAST / INVISIBLE / BLIND / CONFUSED ; duration in ticks (50/sec)
        sam_apply_effect(event.player, "FAST", 250);
    }
}`,
  },
  {
    title: 'Custom hook (register + fire)',
    desc: 'Define your own event, fire it from anywhere; it cross-dispatches Lua<->JS.',
    lua: `sam_register_hook("mymod:combo_finished")

function on_event(event)
    if event.name == "player.on_hit" then
        -- ... track a combo, then:
        sam_fire_hook("mymod:combo_finished", { player = event.player })
    elseif event.name == "mymod:combo_finished" then
        sam_message(event.player, "Combo!")
    end
end`,
    js: `sam_register_hook("mymod:combo_finished");

function on_event(event) {
    if (event.name === "player.on_hit") {
        // ... track a combo, then:
        sam_fire_hook("mymod:combo_finished", { player: event.player });
    } else if (event.name === "mymod:combo_finished") {
        sam_message(event.player, "Combo!");
    }
}`,
  },
];
