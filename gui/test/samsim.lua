--[[
  A fake S.A.M runtime, faithful to the real one, so generated Lua can be RUN instead of
  eyeballed. Every stub below mirrors verified C++ behaviour, cited:

    sam_set_timer / sam_set_repeating_timer
        sam_lua_runtime.cpp:1231 samSetTimerImpl -- replaces a same-id timer in the SAME
        namespace (samRemoveTimer at :1218), clamps ticks<1 to 1.
    tick order
        sam_lua_runtime.cpp:2489 tickTimers -- decrements first, collects the due list,
        THEN fires. A callback registering a timer this tick does not fire it this tick.
    protectedCall
        sam_lua_runtime.cpp:221 -- a timer callback that errors is caught, not fatal.
    sam_set_stat
        sam_lua_runtime.cpp:~640 -- ABSOLUTE. STR/DEX/CON/INT/PER/CHR clamp to
        [ATTR_WIRE_MIN=-7, MAX_PLAYER_STAT_VALUE=248] (stat.hpp:550). HP clamps to
        [0,MAXHP], MP to [0,MAXMP], HUNGER to [0,1500].
    sam_set_move_speed
        sam_lua_runtime.cpp:671 -- ABSOLUTE, host-only, samSanitizeSpeed clamps to
        [0.1, 3.0] (:823-824) and maps NaN to 1.0.
    the player argument
        every function whose first argument is a player index checks
        `player < 0 || player >= MAXPLAYERS` and refuses. Some say so
        (sam_lua_runtime.cpp:1610 sam_get_stat, :1642 sam_set_stat, :1987 sam_message,
        :1553 sam_apply_effect, ... all SAM_ERROR "invalid player index N"), some answer
        "no" without a word (:2251 sam_has_effect, :2267 sam_get_effect_duration).

  The clamps are the point. A decaying buff that clamps on the way up is not reversible on
  the way down, and the player ends the buff WORSE than they started. Only a simulator that
  clamps like the engine can catch that.

  So is the player argument. These stubs used to be written `function sam_message(_, t)`,
  which threw the player away -- so sam_message(-1, "hi") recorded a delivered message and
  any test of "this event hands the script player -1" passed on a script that does nothing
  in game. A stub that ignores an argument cannot fail on that argument.
]]

local S = {}

local state, timers, log, seq, refused

function S.reset(stats)
  state = { move_speed = 1.0, stats = {}, effects = {}, messages = {}, sounds = {} }
  for k, v in pairs(stats or {}) do state.stats[k] = v end
  state.stats.MAXHP = state.stats.MAXHP or 100
  state.stats.MAXMP = state.stats.MAXMP or 100
  state.stats.HP = state.stats.HP or state.stats.MAXHP
  state.stats.MP = state.stats.MP or state.stats.MAXMP
  timers, log, seq, refused = {}, {}, 0, {}
end

local ATTRS = { STR=1, DEX=1, CON=1, INT=1, PER=1, CHR=1 }
local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

-- ---- the player argument ----------------------------------------------------------
--
-- MAXPLAYERS is 4. Anything outside 0..3 is not a player, and every S.A.M function that
-- takes one refuses it. `event.player` is -1 on the events that also fire for monsters and
-- traps (on_before_effect_applied, world.on_projectile_hit, world.on_trap_triggered,
-- world.on_item_deployed), so this is the value a generated script really gets.
--
-- Two behaviours, both copied from the runtime: the loud ones log
-- "<fn>: invalid player index N." and hand back their refusal value; the quiet ones just
-- answer no. Either way the call is recorded in `refused`, so a test can see a call the
-- game would have dropped even when the engine itself says nothing about it.
local MAXPLAYERS = 4

local function refusedPlayer(fn, p, loud)
  if type(p) == 'number' and math.tointeger(p) and p >= 0 and p < MAXPLAYERS then return false end
  table.insert(refused, { fn = fn, player = p })
  if loud then table.insert(log, fn .. ': invalid player index ' .. tostring(p) .. '.') end
  return true
end

local function clampStat(name, v)
  v = math.floor(v)
  if ATTRS[name] then return clamp(v, -7, 248) end
  if name == 'HP' then return clamp(v, 0, state.stats.MAXHP) end
  if name == 'MP' then return clamp(v, 0, state.stats.MAXMP) end
  if name == 'HUNGER' then return clamp(v, 0, 1500) end
  if name == 'GOLD' then return math.max(0, v) end
  return v
end

-- ---- the API surface scripts see -------------------------------------------------
function sam_get_stat(p, name)
  if refusedPlayer('sam_get_stat', p, true) then return 0 end
  return state.stats[name] or 0
end
function sam_set_stat(p, name, v)
  if refusedPlayer('sam_set_stat', p, true) then return false end
  state.stats[name] = clampStat(name, v)
  -- The engine drags current down when the ceiling drops:
  --   MAXHP: samClampInt(value,1,STAT_WIRE_MAX); if (s->HP > s->MAXHP) { s->HP = s->MAXHP; }
  --   MAXMP: same shape, floor 0.
  -- Without this a "temporary +MAXHP" looks reversible in simulation and is not in game.
  if name == 'MAXHP' then
    state.stats.MAXHP = clamp(state.stats.MAXHP, 1, 32767)
    if state.stats.HP > state.stats.MAXHP then state.stats.HP = state.stats.MAXHP end
  elseif name == 'MAXMP' then
    state.stats.MAXMP = clamp(state.stats.MAXMP, 0, 32767)
    if state.stats.MP > state.stats.MAXMP then state.stats.MP = state.stats.MAXMP end
  end
  return true
end
function sam_get_move_speed(p)
  if refusedPlayer('sam_get_move_speed', p, false) then return 1.0 end
  return state.move_speed
end
function sam_set_move_speed(p, m)
  if refusedPlayer('sam_set_move_speed', p, true) then return false end
  if m ~= m then m = 1.0 end                       -- NaN -> 1.0, before the clamp
  -- g_samMoveSpeed is a double; the +0.0 keeps Lua 5.4 from storing an integer subtype for
  -- a whole-number multiplier, so what the test sees matches what the engine holds.
  state.move_speed = clamp(m, 0.1, 3.0) + 0.0
  return true
end
function sam_add_move_speed(p, d)
  if refusedPlayer('sam_add_move_speed', p, true) then return false end
  d = tonumber(d) or 0
  state.move_speed = clamp(state.move_speed + d, 0.1, 3.0) + 0.0  -- additive, same clamp as set
  return state.move_speed
end
function sam_level_up(p, n)
  if refusedPlayer('sam_level_up', p, true) then return false end
  n = math.max(1, math.floor(tonumber(n) or 1))
  state.stats.LVL = clamp((state.stats.LVL or 1) + n, 1, 255)     -- engine grants real benefits; sim tracks the level
  return true
end
function sam_has_effect(p, e)
  if refusedPlayer('sam_has_effect', p, false) then return false end
  return state.effects[e] == true
end
function sam_apply_effect(p, e, ticks, strength)
  if refusedPlayer('sam_apply_effect', p, true) then return false end
  state.effects[e] = true
  state.effect_ticks = state.effect_ticks or {}
  state.effect_strength = state.effect_strength or {}
  state.effect_ticks[e] = tonumber(ticks) or 0
  local st = tonumber(strength) or 0
  state.effect_strength[e] = st > 0 and st or 1   -- default tier is 1 when unspecified
  return true
end
function sam_remove_effect(p, e)
  if refusedPlayer('sam_remove_effect', p, true) then return false end
  state.effects[e] = nil
  if state.effect_ticks then state.effect_ticks[e] = nil end
  if state.effect_strength then state.effect_strength[e] = nil end
  return true
end
function sam_get_effect_duration(p, e)
  if refusedPlayer('sam_get_effect_duration', p, false) then return 0 end
  if state.effect_ticks and state.effect_ticks[e] ~= nil then return state.effect_ticks[e] end
  return state.effects[e] and 9999 or 0            -- active-but-untracked -> large; inactive -> 0
end
function sam_get_effect_strength(p, e)
  if refusedPlayer('sam_get_effect_strength', p, false) then return 0 end
  if state.effect_strength and state.effect_strength[e] ~= nil then return state.effect_strength[e] end
  return state.effects[e] and 1 or 0
end
function sam_get_player_data(p, _)
  if refusedPlayer('sam_get_player_data', p, false) then return nil end
  return nil
end
function sam_set_player_data(p, _, _)
  if refusedPlayer('sam_set_player_data', p, false) then return false end
  return true
end
function sam_message(p, t)
  if refusedPlayer('sam_message', p, true) then return false end
  table.insert(state.messages, t); return true
end
function sam_is_defending(p)
  if refusedPlayer('sam_is_defending', p, false) then return false end
  return state.defending == true
end
function sam_get_class(p)
  if refusedPlayer('sam_get_class', p, false) then return nil end
  return state.class or 'My Class'
end
function sam_get_floor() return state.floor or 0 end
function sam_get_time_played() return state.tick or 0 end
function sam_cast_spell(p) return not refusedPlayer('sam_cast_spell', p, true) end
function sam_grant_item(p) return not refusedPlayer('sam_grant_item', p, true) end
function sam_grant_gold(p) return not refusedPlayer('sam_grant_gold', p, true) end
-- sam_play_sound(sound, vol)  --  lua_sam_play_sound, sam_lua_runtime.cpp:1510.
-- There is NO player argument (it plays for everyone). Arg 1 is the sound: a string id, or an
-- integer index read with luaL_checkinteger (anything else raises). Arg 2 is an optional
-- integer volume, also luaL_checkinteger (so 99.5 raises), default 128, clamped to 0..255.
-- Every call is recorded in state.sounds so a test can check WHAT was asked for: the old
-- block emitted sam_play_sound(player, 28), which is "sound #0 at volume 28" -- a quiet
-- footstep -- and a stub that ignored its arguments let that pass.
local function checkinteger(v, n)
  if type(v) == 'string' then v = tonumber(v) end
  local i = type(v) == 'number' and math.tointeger(v) or nil
  if i == nil then
    error(("bad argument #%d to 'sam_play_sound' (number has no integer representation)"):format(n), 2)
  end
  return i
end
function sam_play_sound(id, vol)
  if type(id) ~= 'string' then id = checkinteger(id, 1) end
  local v = 128
  if vol ~= nil then v = clamp(checkinteger(vol, 2), 0, 255) end
  table.insert(state.sounds, { id = id, vol = v })
  return true
end
function sam_deal_damage() return true end
function sam_kill_monster() return true end
-- sam_modify_damage only lands when the player matches the hook's own player
-- (beforeDamageModify: `if ( g_bdActive && player == g_bdPlayer )`), so a wrong index is a
-- silent no-op rather than an error.
function sam_modify_damage(p) return not refusedPlayer('sam_modify_damage', p, false) end
function sam_get_equipped_item_id(p)
  if refusedPlayer('sam_get_equipped_item_id', p, false) then return nil end
  return nil
end
-- sam_get_player_uid / sam_get_facing: the entity reads a few blocks use. No real entity
-- here, so they answer the way the engine does for a player who is not in the game.
function sam_get_player_uid(p)
  if refusedPlayer('sam_get_player_uid', p, false) then return nil end
  return 1000 + p
end
function sam_get_facing(p)
  if refusedPlayer('sam_get_facing', p, false) then return nil end
  return 0.0
end
function sam_item_id(n) return n end
-- test heuristic: infer a category from a name that contains a category word (real engine reads items[].category)
function sam_get_item_category(t)
  t = tostring(t or '')
  for _, c in ipairs({'WEAPON','ARMOR','AMULET','RING','POTION','SCROLL','SPELLBOOK','MAGICSTAFF','GEM','THROWN','TOOL','FOOD','BOOK'}) do
    if t:find(c, 1, true) then return c end
  end
  return nil
end
function S.setMonsterEffect(e, v) state.monster_effects = state.monster_effects or {}; state.monster_effects[e] = v end
function sam_monster_has_effect(_, e) return (state.monster_effects or {})[e] == true end

local function removeTimer(id)
  for i, t in ipairs(timers) do
    if t.id == id then table.remove(timers, i); return end
  end
end

local function setTimer(id, ticks, cb, repeating)
  assert(type(id) == 'string', 'sam_set_timer: id must be a string')
  assert(type(ticks) == 'number', 'sam_set_timer: ticks must be a number')
  assert(type(cb) == 'function', 'sam_set_timer: callback must be a function')
  removeTimer(id)                                  -- same-id replace, like samRemoveTimer
  ticks = math.floor(ticks)
  if ticks < 1 then ticks = 1 end                  -- ticks<1 -> 1
  table.insert(timers, { id = id, remaining = ticks, interval = repeating and ticks or 0,
                         repeating = repeating, cb = cb })
end

function sam_set_timer(id, ticks, cb) setTimer(id, ticks, cb, false) end
function sam_set_repeating_timer(id, ticks, cb) setTimer(id, ticks, cb, true) end
function sam_cancel_timer(id) removeTimer(id) end

-- ---- driving it ------------------------------------------------------------------

--- One engine tick. Mirrors tickTimers(): decrement-and-collect, THEN fire.
function S.tick()
  state.tick = (state.tick or 0) + 1
  local due = {}
  local i = 1
  while i <= #timers do
    local t = timers[i]
    t.remaining = t.remaining - 1
    if t.remaining <= 0 then
      table.insert(due, t.cb)
      if t.repeating then
        t.remaining = t.interval > 0 and t.interval or 1
        i = i + 1
      else
        table.remove(timers, i)
      end
    else
      i = i + 1
    end
  end
  for _, cb in ipairs(due) do
    local ok, err = pcall(cb)                      -- protectedCall: caught, not fatal
    if not ok then table.insert(log, 'TIMER ERROR: ' .. tostring(err)) end
  end
  if _G.on_tick then
    local ok, err = pcall(on_tick, { tick_count = state.tick })
    if not ok then table.insert(log, 'on_tick ERROR: ' .. tostring(err)) end
  end
end

function S.ticks(n) for _ = 1, n do S.tick() end end
function S.seconds(n) S.ticks(math.floor(n * 50)) end

function S.fire(name, extra)
  local ev = { name = name }
  for k, v in pairs(extra or {}) do ev[k] = v end
  if not _G.on_event then return end
  local ok, err = pcall(on_event, ev)
  if not ok then table.insert(log, 'on_event ERROR: ' .. tostring(err)) end
end

function S.state() return state end
function S.timers() return timers end
function S.errors() return log end
--- Calls the engine would have thrown away because the player index was not a player.
--- Includes the ones the engine refuses WITHOUT logging, which is the whole point: a test
--- that only watched the log could not see sam_has_effect(-1, ...) answering false forever.
function S.refusedPlayerCalls() return refused end
function S.pendingTimerIds()
  local ids = {}
  for _, t in ipairs(timers) do table.insert(ids, t.id) end
  table.sort(ids)
  return ids
end

--- Load a generated script the way the framework does: a parse error means NOTHING loads.
function S.load(src, name)
  local chunk, err = load(src, '@' .. (name or 'generated.lua'))
  if not chunk then return nil, 'PARSE ERROR: ' .. tostring(err) end
  _G.on_event, _G.on_tick = nil, nil
  local ok, rerr = pcall(chunk)
  if not ok then return nil, 'RUNTIME ERROR: ' .. tostring(rerr) end
  return true
end

return S
