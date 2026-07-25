-- Bone Hunter — a worked example.
--
-- Every capability the framework added in v1.9.0 appears here exactly once, each in its
-- own numbered section. Read the section that matches what you're trying to build, copy
-- it, delete the rest.
--
-- The loop this mod builds:
--   kill things -> they drop bones -> craft bone gear at the Hunter's Workbench ->
--   the gear does something the engine has no idea about.

local NS = "bonehunter:"

-- Resolve ids ONCE at load, never inline. The enum numbers are not what you'd guess
-- (TOOL_TORCH is 148, not 8), and a wrong number doesn't error — it silently never
-- matches, which is a genuinely miserable afternoon. sam_item_id takes both a vanilla
-- name and your own "namespace:item", so one call covers both.
local BONE   = sam_item_id(NS .. "monster_bone")
local AXE    = sam_item_id(NS .. "bone_axe")
local SNARE  = sam_item_id(NS .. "bone_snare")
local CHARM  = sam_item_id(NS .. "hunters_charm")


-- =====================================================================================
-- 1. Monsters drop your item                          [player.on_kill + sam_spawn_item]
-- =====================================================================================
-- The plain way to make your content show up: put it there yourself when something
-- happens. No engine involvement, works today, works in every version.
--
-- Watch the argument types here, because they are the most common mistake in the whole
-- API. `e.player` is a player INDEX (0-3). sam_get_position wants a entity UID, which is
-- a completely different number. Pass the index where a uid belongs and you don't get an
-- error -- you get nil, or somebody else's position.
--
--   sam_get_position(e.player)                  -- wrong, silently
--   sam_get_position(sam_get_player_uid(e.player))  -- right

local function on_kill(e)
  local drops = 1

  -- Wearing the charm doubles the drop. This is how you make an item do something the
  -- engine has no concept of: the item itself is inert, and the script reads it.
  if sam_get_equipped_item_id(e.player, "amulet") == CHARM then
    drops = 2
  end

  -- e.target_uid is the thing that died, so we drop the bones on the corpse.
  local x, y = sam_get_position(e.target_uid)
  if not x then return end          -- the body may already be gone; always check

  for _ = 1, drops do
    sam_spawn_item(x, y, NS .. "monster_bone")
  end
end


-- =====================================================================================
-- 2. Your item appears in the world on its own                    [the "level" field]
-- =====================================================================================
-- No script at all. hunters_charm.json says "level": 4, so the charm rolls in chests,
-- shops and floor loot from around dungeon depth 4, exactly like a vanilla amulet.
--
-- The bone and the snare say "level": -1, which keeps them OUT of random generation —
-- that's what you want for crafting materials and anything a class starts with.
--
-- Check it without playing:  /sam_loot AMULET 100


-- =====================================================================================
-- 3. A thrown item becomes something real           [world.on_item_deployed, CANCELLABLE]
-- =====================================================================================
-- bone_snare.json declares the "tinker_throwable" trait, which is what lets the engine
-- recognise it as throwable at all. Without the trait it flies, lands, and vanishes,
-- because the engine's list of deployable things is hardcoded vanilla ids.
--
-- The trait gets it thrown. THIS is what makes it do something.
--
-- Returning false means "don't build your gadget, I built mine". If you return nothing,
-- the engine tries to construct a vanilla trap from your custom item and gets confused.

local function on_deployed(e)
  if e.item_type ~= SNARE then return end   -- not ours, let the engine handle it

  -- A snare is three angry rats where it lands. Note sam_spawn_monster (singular) takes
  -- tile coordinates; sam_spawn_monsters (plural) takes a UID to spawn near instead.
  -- Two similarly named functions, two different first arguments.
  for _ = 1, 3 do
    sam_spawn_monster(e.x, e.y, "RAT")
  end
  sam_play_sound(76)                        -- sound first, no player argument

  return false                              -- we handled it
end


-- =====================================================================================
-- 4. Rewriting a number the engine was about to use      [on_before_monster_damage]
-- =====================================================================================
-- The engine has worked out the damage and is about to apply it. This hook hands you the
-- number first. Call sam_modify_monster_damage and the engine uses yours instead.
--
-- Bone weapons bite harder into undead. There is no "bonus vs undead" system in Barony —
-- this IS the system, and it's six lines.

local function on_monster_damage(e)
  -- The trait test. bone_tyrant.json declared "undead", and this reads it back.
  if not sam_monster_has_trait(e.monster_uid, "undead") then return end

  -- Honest limitation: this event tells you the monster and the damage, but NOT who
  -- swung. There is no e.player here. So we check player 0's weapon, which is correct
  -- in singleplayer and correct for the host in multiplayer, and wrong for other
  -- players. If your rule must be exact per-player, hang it off player.on_hit (which
  -- does carry e.player) and stash the result for this hook to read.
  if sam_get_equipped_item_id(0, "weapon") ~= AXE then return end

  sam_modify_monster_damage(e.damage * 2)
end
-- Setting it to 0 negates the hit completely. Negative values clamp to zero, so you
-- can't heal something by passing -50; use sam_deal_damage for that.


-- =====================================================================================
-- 5. Refusing something the player tried to do          [player.on_before_equip, CANCELS]
-- =====================================================================================
-- Class and stat restrictions, cursed gear, quest locks. Return false and the equip does
-- not happen; the player gets a message.
--
-- Careful here: this fires for real player equips only, not for starting gear or scripted
-- grants, which is deliberate — vetoing those crashes character creation. If you want to
-- block a grant too, don't grant it.

local function on_before_equip(e)
  if e.item_type ~= AXE then return end

  if sam_get_stat(e.player, "STR") < 8 then
    sam_message(e.player, "The bone axe is too heavy for you.")
    return false
  end
end


-- =====================================================================================
-- 6. Custom monsters that the engine treats as special       [monster traits, JSON only]
-- =====================================================================================
-- bone_tyrant.json is a skeleton variant, so without help the engine treats it as an
-- ordinary skeleton. Three words in JSON change that:
--
--   "traits": ["boss", "undead", "never_retreat"]
--
--   boss          -> boss health bar, boss music, boss death handling
--   undead        -> smite and holy damage work on it, and section 4 above sees it
--   never_retreat -> fights to the death, and can't be routed by Fear
--
-- Worth knowing: a trait only CHANGES something if the base type doesn't already have it.
-- Skeletons are already smite-weak and already never flee, so on this monster `undead` and
-- `never_retreat` are declared for clarity rather than effect -- only `boss` actually
-- changes behaviour here. Put the same two traits on a rat and both do real work, because
-- a rat is neither. Check what a base type already gives you before assuming a trait is
-- what's doing the work.
--
-- No script needed for any of that. Check what actually applied:  /sam_montraits
--
-- A script can read traits back with sam_monster_has_trait, which is how you write a
-- rule about a KIND of monster instead of one specific monster. Note we don't compare
-- e.monster_type to anything: the tyrant is a skeleton as far as the type is concerned,
-- so the type would match every skeleton in the dungeon. The trait is the real question.

local function on_monster_spawned(e)
  if sam_monster_has_trait(e.monster_uid, "boss") then
    sam_message(0, "Something large is awake down here.")
  end
end


-- =====================================================================================
-- 7. Crafting                                                      [recipes/*.json]
-- =====================================================================================
-- Also no script. recipes/bone_axe.json says which item, which materials, how many, and
-- crucially:
--
--   "kit": "sam:hunters_workbench"
--
-- That line is required and it decides which bench the recipe lives on. Point it at the
-- Hunter's Workbench and your recipes sit on their own bench alongside vanilla tinkering
-- rather than inside it. The vanilla kit is never touched by this mod.
--
-- The workbench itself ships with the framework, so you don't have to make one. Give it
-- to a class in the Class Editor, or in a script:  sam_grant_item(0, "sam:hunters_workbench")


-- =====================================================================================
-- Dispatch
-- =====================================================================================
-- One entry point, one branch per event. Returning the result of a handler is what makes
-- cancelling work — `return on_deployed(e)` passes its false up; a bare `on_deployed(e)`
-- would throw the answer away and the cancel would silently do nothing.

function on_event(e)
  if     e.name == "player.on_kill"              then return on_kill(e)
  elseif e.name == "world.on_item_deployed"      then return on_deployed(e)
  elseif e.name == "on_before_monster_damage"    then return on_monster_damage(e)
  elseif e.name == "player.on_before_equip"      then return on_before_equip(e)
  elseif e.name == "world.on_monster_spawned"    then return on_monster_spawned(e)
  end
end

sam_log("Bone Hunter loaded. bone=" .. BONE .. " axe=" .. AXE .. " snare=" .. SNARE)
