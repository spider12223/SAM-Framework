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

  The clamps are the point. A decaying buff that clamps on the way up is not reversible on
  the way down, and the player ends the buff WORSE than they started. Only a simulator that
  clamps like the engine can catch that.
]]

local S = {}

local state, timers, log, seq

function S.reset(stats)
  state = { move_speed = 1.0, stats = {}, effects = {}, messages = {} }
  for k, v in pairs(stats or {}) do state.stats[k] = v end
  state.stats.MAXHP = state.stats.MAXHP or 100
  state.stats.MAXMP = state.stats.MAXMP or 100
  state.stats.HP = state.stats.HP or state.stats.MAXHP
  state.stats.MP = state.stats.MP or state.stats.MAXMP
  timers, log, seq = {}, {}, 0
end

local ATTRS = { STR=1, DEX=1, CON=1, INT=1, PER=1, CHR=1 }
local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

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
function sam_get_stat(_, name) return state.stats[name] or 0 end
function sam_set_stat(_, name, v)
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
function sam_get_move_speed(_) return state.move_speed end
function sam_set_move_speed(_, m)
  if m ~= m then m = 1.0 end                       -- NaN -> 1.0, before the clamp
  -- g_samMoveSpeed is a double; the +0.0 keeps Lua 5.4 from storing an integer subtype for
  -- a whole-number multiplier, so what the test sees matches what the engine holds.
  state.move_speed = clamp(m, 0.1, 3.0) + 0.0
  return true
end
function sam_add_move_speed(_, d)
  d = tonumber(d) or 0
  state.move_speed = clamp(state.move_speed + d, 0.1, 3.0) + 0.0  -- additive, same clamp as set
  return state.move_speed
end
function sam_level_up(_, n)
  n = math.max(1, math.floor(tonumber(n) or 1))
  state.stats.LVL = clamp((state.stats.LVL or 1) + n, 1, 255)     -- engine grants real benefits; sim tracks the level
  return true
end
function sam_has_effect(_, e) return state.effects[e] == true end
function sam_apply_effect(_, e, _) state.effects[e] = true; return true end
function sam_remove_effect(_, e) state.effects[e] = nil; return true end
function sam_message(_, t) table.insert(state.messages, t); return true end
function sam_is_defending(_) return state.defending == true end
function sam_get_class(_) return state.class or 'My Class' end
function sam_get_floor() return state.floor or 0 end
function sam_get_time_played() return state.tick or 0 end
function sam_cast_spell() return true end
function sam_grant_item() return true end
function sam_grant_gold() return true end
function sam_play_sound() return true end
function sam_deal_damage() return true end
function sam_kill_monster() return true end
function sam_modify_damage() return true end
function sam_get_equipped_item_id() return nil end
function sam_item_id(n) return n end

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
