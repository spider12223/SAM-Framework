-- Running Skeleton -- a worked example of the picture API added in v1.10.3.
--
-- One skeleton spawns on every floor, runs you down, and when it reaches you the mod's
-- own picture takes the whole screen. Everything here is the ordinary S.A.M script API;
-- the only new parts are sam_show_image and the "images" list in mod.json.
--
-- TO MAKE IT YOURS: drop your picture in as art/scare.png and change the sound index.
-- Nothing else in this file has to change.

-- ---- tuning ------------------------------------------------------------------------
local HUNT_RANGE   = 16      -- tiles: it starts running at you from here
local POUNCE_RANGE = 2.6     -- tiles: this close and it gets you
local CHARGE_TICKS = 45      -- how long one dash lasts (50 ticks = 1 second)
local SCARE_MS     = 900     -- how long the picture stays up
local COOLDOWN     = 400     -- ticks before it can scare the same player again
local SPOT_SOUND   = 93      -- SkeletonSpot.ogg
local RESPAWN      = 250     -- ticks before a dead runner comes back

-- ---- state -------------------------------------------------------------------------
local runner = nil           -- uid of the skeleton we are driving
local frame = 0
local nextCharge = 0
local cooldown = {}          -- per player: frame number it may be scared again
local respawnAt = 0

local function dist(x1, y1, x2, y2)
  local dx, dy = x1 - x2, y1 - y2
  return math.sqrt(dx * dx + dy * dy)
end

-- Put it somewhere real: walk outward from the player until a tile is both open and
-- actually connected to where the player is standing, so it never spawns sealed in rock.
local function spawnRunner(px, py)
  for r = 8, 20 do
    for _, d in ipairs({ {r,0}, {-r,0}, {0,r}, {0,-r}, {r,r}, {-r,-r} }) do
      local tx, ty = px + d[1], py + d[2]
      if sam_is_spawnable(tx, ty) and sam_tiles_connected(px, py, tx, ty) then
        local uid = sam_spawn_monster(tx, ty, "SKELETON")
        if uid then
          -- Faster and tougher than the skeletons it spawns next to, so being chased
          -- by this one reads as different rather than as ordinary combat.
          sam_set_monster_stat(uid, "MAXHP", 120)
          sam_set_monster_stat(uid, "HP", 120)
          sam_log("[runner] spawned at " .. tx .. "," .. ty)
          return uid
        end
      end
    end
  end
  return nil
end

function on_event(e)
  if e.name == "game.on_game_start" or e.name == "game.on_level_entered" then
    runner = nil
    respawnAt = frame + 100      -- let the floor settle before it turns up
    cooldown = {}
  end
end

-- Who it is after. Barony has at most four player slots and a slot can be empty, which is
-- what sam_get_player_uid answering nil means, so walk them rather than assuming player 0.
-- Pass nil for the skeleton's position and you get the first player in the game instead of
-- the nearest, which is what the respawn needs. In singleplayer both give you player 0, so
-- the same code runs in a solo game and in co-op.
local function pickPlayer(mx, my)
  local best, bx, by, bd
  for p = 0, 3 do
    local uid = sam_get_player_uid(p)
    if uid then
      local x, y = sam_get_position(uid)
      if x then
        local d = mx and dist(x, y, mx, my) or 0
        if not bd or d < bd then best, bx, by, bd = p, x, y, d end
        if not mx then break end
      end
    end
  end
  return best, bx, by, bd
end

function on_tick(e)
  frame = frame + 1
  if frame % 5 ~= 0 then return end   -- 10 checks a second is plenty for a chase

  -- Dead or never spawned: bring one back after a pause, beside somebody.
  if not runner or not sam_get_position(runner) then
    if frame >= respawnAt then
      local _, sx, sy = pickPlayer(nil, nil)
      if not sx then return end
      runner = spawnRunner(sx, sy)
      respawnAt = frame + RESPAWN
    end
    return
  end

  local mx, my = sam_get_position(runner)
  local victim, px, py, d = pickPlayer(mx, my)
  if not victim or d > HUNT_RANGE then return end

  -- Only hunt what it can actually see. sam_line_of_sight is the engine's own trace, so
  -- it agrees with what is drawn instead of guessing from distance alone.
  if not sam_line_of_sight(mx, my, px, py) then
    sam_monster_path_to(runner, px, py)
    return
  end

  sam_set_monster_target(runner, victim)

  if d > POUNCE_RANGE then
    -- Charge: a dash that ends by itself, so re-issuing it on a timer reads as running.
    if frame >= nextCharge then
      sam_monster_face(runner, px, py)
      sam_monster_charge(runner, CHARGE_TICKS)
      nextCharge = frame + CHARGE_TICKS
    end
    return
  end

  -- ---- it got you ------------------------------------------------------------------
  -- Everything from here names the player it happened to, so in co-op the flash, the shake
  -- and the picture land on THEIR screen and nobody else's, wherever they are playing from.
  if (cooldown[victim] or 0) > frame then return end
  cooldown[victim] = frame + COOLDOWN

  sam_play_sound(SPOT_SOUND)
  sam_screen_flash(victim, 255, 255, 255, 0.8, 120)
  sam_camera_shake(victim, 14)
  -- The whole point of this example. "runner:scare" is the id from mod.json, so if the
  -- file is missing the log said so when the mod loaded, not silently now.
  sam_show_image(victim, "runner:scare", SCARE_MS, 255, "stretch")
  sam_log("[runner] got player " .. victim .. " at " .. px .. "," .. py)
end
