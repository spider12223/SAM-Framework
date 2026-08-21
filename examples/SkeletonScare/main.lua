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

function on_tick(e)
  frame = frame + 1
  if frame % 5 ~= 0 then return end   -- 10 checks a second is plenty for a chase

  local me = sam_get_player_uid(0)
  if not me then return end
  local px, py = sam_get_position(me)
  if not px then return end

  -- Dead or never spawned: bring one back after a pause.
  if not runner or not sam_get_position(runner) then
    if frame >= respawnAt then
      runner = spawnRunner(px, py)
      respawnAt = frame + RESPAWN
    end
    return
  end

  local mx, my = sam_get_position(runner)
  local d = dist(px, py, mx, my)
  if d > HUNT_RANGE then return end

  -- Only hunt what it can actually see. sam_line_of_sight is the engine's own trace, so
  -- it agrees with what is drawn instead of guessing from distance alone.
  if not sam_line_of_sight(mx, my, px, py) then
    sam_monster_path_to(runner, px, py)
    return
  end

  sam_set_monster_target(runner, 0)

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
  if (cooldown[0] or 0) > frame then return end
  cooldown[0] = frame + COOLDOWN

  sam_play_sound(SPOT_SOUND)
  sam_screen_flash(0, 255, 255, 255, 0.8, 120)
  sam_camera_shake(0, 14)
  -- The whole point of this example. "runner:scare" is the id from mod.json, so if the
  -- file is missing the log said so when the mod loaded, not silently now.
  sam_show_image(0, "runner:scare", SCARE_MS, 255, "stretch")
  sam_log("[runner] got you at " .. px .. "," .. py)
end
