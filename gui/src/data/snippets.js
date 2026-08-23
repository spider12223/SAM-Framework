/*
 * Insertable behavior-script snippets for the in-tool script editor.
 * Each has a Lua and a JS form (the same global sam_* API is available in both;
 * .ts scripts are transpiled to JS, so the JS form applies there too).
 * These are illustrative starters — the API Reference panel is the authoritative
 * source for exact function signatures.
 */
export const SNIPPETS = [
  {
    title: 'A shop window (v1.11.0)',
    desc: 'A real window: a scrolling list built from the game\'s own item table, and a click that sells.',
    lua: `-- Opens on a key, lists every weapon the game knows about, sells the one you click.
local PRICE_MULT = 2

local stock = {}   -- row id -> item type

local function refresh()
    sam_ui_list_clear("shop", "stock")
    stock = {}
    for _, item in ipairs(sam_list_items("WEAPON")) do
        local id = tostring(item.type)
        stock[id] = item
        sam_ui_list_add("shop", "stock", id,
            string.format("%-28s %d gold", item.name, item.value * PRICE_MULT))
    end
end

function on_event(event)
    if event.name == "on_action_pressed" and event.action == "Use" then
        -- modal = true, or the player has no cursor to click with
        sam_ui_open("shop", 240, 120, 700, 460, "Trader", true)
        sam_ui_panel_style("shop", 0xE01A1410, 0xFF6B5A3E, 2)
        sam_ui_list("shop", "stock", 20, 20, 660, 340)
        sam_ui_button("shop", "done", 20, 380, 160, 44, "Leave")
        refresh()

    elseif event.name == "ui.on_select" and event.widget == "stock" then
        -- the row id is in .value, NOT .row
        local item = stock[event.value]
        if not item then return end
        local cost = item.value * PRICE_MULT
        if sam_get_stat(event.player, "GOLD") < cost then
            sam_message(event.player, "You cannot afford that.")
            return
        end
        sam_set_stat(event.player, "GOLD", sam_get_stat(event.player, "GOLD") - cost)
        sam_grant_item(event.player, item.name)
        sam_message(event.player, "Bought " .. item.name .. ".")

    elseif event.name == "ui.on_click" and event.widget == "done" then
        sam_ui_close("shop")
    end
end`,
    js: `// Opens on a key, lists every weapon the game knows about, sells the one you click.
const PRICE_MULT = 2;
let stock = {};

function refresh() {
    sam_ui_list_clear("shop", "stock");
    stock = {};
    for (const item of sam_list_items("WEAPON")) {
        const id = String(item.type);
        stock[id] = item;
        sam_ui_list_add("shop", "stock", id, item.name + "   " + (item.value * PRICE_MULT) + " gold");
    }
}

function on_event(event) {
    if (event.name === "on_action_pressed" && event.action === "Use") {
        // modal = true, or the player has no cursor to click with
        sam_ui_open("shop", 240, 120, 700, 460, "Trader", true);
        sam_ui_panel_style("shop", 0xE01A1410, 0xFF6B5A3E, 2);
        sam_ui_list("shop", "stock", 20, 20, 660, 340);
        sam_ui_button("shop", "done", 20, 380, 160, 44, "Leave");
        refresh();

    } else if (event.name === "ui.on_select" && event.widget === "stock") {
        // the row id is in .value, NOT .row
        const item = stock[event.value];
        if (!item) return;
        const cost = item.value * PRICE_MULT;
        if (sam_get_stat(event.player, "GOLD") < cost) {
            sam_message(event.player, "You cannot afford that.");
            return;
        }
        sam_set_stat(event.player, "GOLD", sam_get_stat(event.player, "GOLD") - cost);
        sam_grant_item(event.player, item.name);
        sam_message(event.player, "Bought " + item.name + ".");

    } else if (event.name === "ui.on_click" && event.widget === "done") {
        sam_ui_close("shop");
    }
}`,
  },
  {
    title: 'A hub you can walk back to (v1.11.0)',
    desc: 'Travel to a floor and back UP again, with a chest whose contents survive the descent.',
    lua: `-- Barony floors are one way: a ladder only ever counts upward. sam_travel_to_level is
-- the only route back, which is what makes a home base possible.
local HUB_FLOOR = 2

function on_event(event)
    if event.name == "on_action_pressed" and event.action == "Use" then
        local here = sam_get_floor()
        if here == HUB_FLOOR then
            -- remember where we left from, so we can come back to it
            sam_world_save("away_floor", here + 2)
            sam_travel_to_level(here + 2)
        else
            sam_world_save("away_floor", here)
            sam_travel_to_level(HUB_FLOOR)
        end

    elseif event.name == "player.on_chest_opened" then
        -- Any chest in the hub becomes permanent storage. What is left in it follows the
        -- player down every floor and survives saving and loading.
        if sam_get_floor() == HUB_FLOOR then
            sam_set_chest_stash(event.chest_uid, true)
            sam_message(event.player, "This chest keeps what you leave in it.")
        end
    end
end`,
    js: `// Barony floors are one way: a ladder only ever counts upward. sam_travel_to_level is
// the only route back, which is what makes a home base possible.
const HUB_FLOOR = 2;

function on_event(event) {
    if (event.name === "on_action_pressed" && event.action === "Use") {
        const here = sam_get_floor();
        if (here === HUB_FLOOR) {
            sam_world_save("away_floor", here + 2);
            sam_travel_to_level(here + 2);
        } else {
            sam_world_save("away_floor", here);
            sam_travel_to_level(HUB_FLOOR);
        }

    } else if (event.name === "player.on_chest_opened") {
        // Any chest in the hub becomes permanent storage.
        if (sam_get_floor() === HUB_FLOOR) {
            sam_set_chest_stash(event.chest_uid, true);
            sam_message(event.player, "This chest keeps what you leave in it.");
        }
    }
}`,
  },
  {
    title: 'Progress that belongs to one character (v1.11.0)',
    desc: 'sam_world_save lives inside ONE savegame, so a new character never inherits it.',
    lua: `-- Compare sam_save_data, which writes a file shared by every character you ever roll and
-- outlives the save that made it. That is right for a mod's settings and wrong for a
-- character's progress. sam_world_save is the other half.

local function unlocked(name)
    return sam_world_load("unlock_" .. name) ~= nil
end

function on_event(event)
    if event.name == "player.on_level_up" then
        if sam_get_stat(event.player, "LEVEL") >= 5 and not unlocked("veteran") then
            sam_world_save("unlock_veteran", true)
            sam_message(event.player, "Veteran unlocked -- for THIS character.")
        end

    elseif event.name == "game.on_game_start" then
        -- nil on a brand new character: that is your first-run hook
        local trips = sam_world_load("trips")
        if trips == nil then
            sam_message(event.player, "A new life begins.")
            sam_world_save("trips", 0)
        else
            sam_message(event.player, "Welcome back. Runs so far: " .. tostring(trips))
        end
    end
end`,
    js: `// Compare sam_save_data, which writes a file shared by every character you ever roll and
// outlives the save that made it. sam_world_save is the other half.

function unlocked(name) {
    return sam_world_load("unlock_" + name) !== undefined;
}

function on_event(event) {
    if (event.name === "player.on_level_up") {
        if (sam_get_stat(event.player, "LEVEL") >= 5 && !unlocked("veteran")) {
            sam_world_save("unlock_veteran", true);
            sam_message(event.player, "Veteran unlocked -- for THIS character.");
        }

    } else if (event.name === "game.on_game_start") {
        // undefined on a brand new character: that is your first-run hook
        const trips = sam_world_load("trips");
        if (trips === undefined) {
            sam_message(event.player, "A new life begins.");
            sam_world_save("trips", 0);
        } else {
            sam_message(event.player, "Welcome back. Runs so far: " + trips);
        }
    }
}`,
  },
  {
    title: 'A ranged attack of your own (v1.11.0)',
    desc: 'Fire something with its own speed, model and damage, and act on what it hits.',
    lua: `-- Until v1.11.0 the only thing a script could launch was a fixed vanilla spell.
local COOLDOWN = 25          -- ticks; 50 = one second
local ready_at = 0

function on_event(event)
    if event.name == "on_action_pressed" and event.action == "Use" then
        local now = sam_get_time_played()
        if now < ready_at then return end
        ready_at = now + COOLDOWN

        local uid = sam_get_player_uid(event.player)
        if not uid then return end
        local x, y = sam_get_position(uid)          -- takes a UID, not a player index
        local yaw = sam_get_facing(event.player)
        if not x or not yaw then return end

        -- x, y, angle, speed, damage, lifetime(ticks), model, owner
        sam_spawn_projectile(x, y, yaw, 7.0, 12, 100, "166", event.player)

    elseif event.name == "on_projectile_hit" then
        -- target is 0 when it hit a wall, so check before treating it as a creature
        if event.target ~= 0 then
            sam_screen_flash(255, 200, 60, 90, 0.15, 80)
        end
    end
end`,
    js: `// Until v1.11.0 the only thing a script could launch was a fixed vanilla spell.
const COOLDOWN = 25;   // ticks; 50 = one second
let readyAt = 0;

function on_event(event) {
    if (event.name === "on_action_pressed" && event.action === "Use") {
        const now = sam_get_time_played();
        if (now < readyAt) return;
        readyAt = now + COOLDOWN;

        const uid = sam_get_player_uid(event.player);
        if (!uid) return;
        const [x, y] = sam_get_position(uid);       // takes a UID, not a player index
        const yaw = sam_get_facing(event.player);
        if (x === undefined || yaw === undefined) return;

        // x, y, angle, speed, damage, lifetime(ticks), model, owner
        sam_spawn_projectile(x, y, yaw, 7.0, 12, 100, "166", event.player);

    } else if (event.name === "on_projectile_hit") {
        // target is 0 when it hit a wall
        if (event.target !== 0) {
            sam_screen_flash(255, 200, 60, 90, 0.15, 80);
        }
    }
}`,
  },
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
