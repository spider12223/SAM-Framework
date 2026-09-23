# Mod settings: keys and sliders in the game's own menus

A mod can put two kinds of thing in the game's Settings screens: a rebindable action in
**Settings > Controls > Bindings**, and a slider, toggle, dropdown, number or text box in a
**MOD SETTINGS** section at the end of **Settings > General**. Both appear under your mod's name,
both persist, and neither needs a line of UI code.

```lua
sam_register_action("slot1", "Game speed slot 1", "Keypad 0")
sam_register_setting("slot1_speed", { type = "slider", label = "Slot 1 speed",
	min = 0.1, max = 8, step = 0.1, default = 0.5 })

function on_event(e)
	if e.name == "on_action_pressed" and e.action == "mymod:slot1" then
		sam_set_game_speed(sam_get_setting("slot1_speed"))
	end
end
```

Register at the top of your script, so the rows exist in the main menu's Settings before a game
starts. Registering the same id again is harmless.

## Actions

`sam_register_action(id, label, default_key [, default_pad])` adds a rebindable row. The action's
full name is `"namespace:id"`, and that is the name `on_action_pressed` and `on_action_released`
carry and the name `sam_is_action_held` and `sam_get_action_binding` take.

Give the key its Bindings-page spelling: `"Keypad 0"`, `"F5"`, `"Left Shift"`, `"Mouse3"`, or
`"[unbound]"`. `"KP0"` is refused, and the refusal names the accepted forms. The controller
default is optional and takes `"ButtonY"`, `"LeftTrigger"`, `"DpadX+"` and so on.

What you pass are **defaults**. If the player has already bound the action to something, that
binding is kept: it survives restarts, it survives your mod being unloaded, and Restore Defaults
brings your default back. There is no conflict check in the game, so choose a default nothing else
uses (the keypad is free in every vanilla layout) and let the player rebind.

A mod action is polled and reported exactly like a vanilla action: in co-op a joiner's press
reaches the host as `on_action_pressed` with the joiner's own binding.

## Settings

`sam_register_setting(id, spec)` adds a row to the MOD SETTINGS section, in both the main menu and
the pause menu. The spec is a table:

| key | |
|---|---|
| `type` | `"slider"` (needs `min` and `max`, within -1000000000..1000000000 and at most 1,000,000 steps apart; `step` optional, values snap to it; from the keyboard or a controller the handle moves one step at a time, or by one percent of a continuous range), `"toggle"` (a boolean), `"dropdown"` (needs `options`; the default is the first unless you give one), `"number"` (`min` and `max` optional), `"text"` (up to 31 characters) |
| `label` | the row's text |
| `tooltip` | optional; `tip` is accepted too |
| `default` | the value it starts with |

`sam_get_setting(id)` is the value in force on this machine, as a number, boolean or string. Read it
when you need it rather than caching it at load, because the player can change it mid-game.
`sam_set_setting(id, value)` changes it from the script with the same validation the screen applies,
saves it, and fires the event. `sam_list_settings()` lists everything you declared with the values in
force.

The value the player confirms is saved in your mod's own data folder
(`savegames/sam_mod_data/<namespace>/settings.sam`), never in `config.json`. It survives a restart
and it survives your mod being unloaded. The file is read whole and must be under 64 KB; an entry
that is not a number, a boolean or a string is dropped with a warning in `sam_log.txt`. Restore Defaults in the vanilla settings only reaches it
through the MOD SETTINGS section.

## The event

`mod.on_setting_changed` fires on the machine whose setting changed, once per setting, after the
value is stored: when the player presses Confirm (`source` is `"ui"`), when they press Restore
Defaults and then Confirm (`"reset"`), and when a script calls `sam_set_setting` (`"script"`). It
carries `mod`, `id`, `type`, `old`, `new` (as text, because event fields are numbers and strings)
and `source`. Every mod hears every mod's changes, so compare `e.mod` with your namespace, and read
the typed value back with `sam_get_setting` rather than parsing `e.new`. Nothing fires for Discard,
for a Confirm that changed nothing, or for a set to the value already in force.

## Multiplayer

The registrations are `all` calls, like `sam_patch_item`: a host's registration is carried to every
S.A.M client and replayed to a late joiner, so everyone's screens show the rows. A setting's
**value** is per machine: the joiner's slider is the joiner's own. A mod that needs one value
everywhere reads it on the host and sends it with `sam_send_packet`.

So a setting must not feed an `all` table (`sam_set_loot_weight`, `sam_set_loot_context`,
`sam_patch_item`...) at load or from a joiner's `mod.on_setting_changed`: while mods load
`sam_is_host()` is true on every machine, so each one would build its own copy of the table from
its own value, and a joiner's call is refused later. Apply a setting-driven rule from a host-only
event such as `game.on_game_start`, where an `all` call carries the host's value to everyone and to
a late joiner, and guard the `mod.on_setting_changed` branch with `sam_is_host()`.
`examples/ItemRandomizer` does exactly this for its Lite toggle.

## Known limits

The General tab grows with every setting and scrolls. A text setting is 31 characters because that
is the field. The rows live in the General tab rather than inside the Bindings window, because that
window's Confirm and Discard only know about bindings. `/sam_reload` is refused while the Settings
window is open; close it first.

## A whole mod

`examples/GameSpeedControl` registers four actions and five settings and reads them in
`on_action_pressed`. Its acceptance test is `SettingsTest`, which proves persistence across a
restart when run twice.
