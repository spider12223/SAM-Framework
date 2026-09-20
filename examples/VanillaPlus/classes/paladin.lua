-- Paladin — faith mends the shield-arm, and shelters the faithful at death's door.
--
-- player.on_block arrived in framework v1.6.0, which is why this mod's mod.json asks for
-- that version: on anything older the block-heal below would simply never fire, silently.

local CLASS = "vanillaplus:paladin"

-- Once a script is loaded it receives EVERY event, whatever content it shipped beside:
-- dispatch is global, not scoped to this class. So every handler starts by asking whose
-- event this is and whether that player actually picked this class. Without this line every
-- class in the game would get the Paladin's damage negation.
local function mine(player)
  return player ~= nil and player >= 0 and sam_get_class(player) == CLASS
end

function on_event(event)
  if event.name == "player.on_block" then
    -- Every full block mends a little (bounded to your max HP by sam_set_stat).
    if not mine(event.player) then return end
    local hp = sam_get_stat(event.player, "HP")
    if hp then sam_set_stat(event.player, "HP", hp + 2) end

  elseif event.name == "on_before_damage" then
    -- Divine intervention: once per floor, a blow that finds you at/below 30% HP is
    -- turned aside and you are healed. (Emulated "invincibility" — there is no
    -- EFF_INVINCIBLE, so we cancel the incoming damage directly. sam_modify_damage
    -- only works inside on_before_damage.)
    if not mine(event.player) then return end
    local hp = sam_get_stat(event.player, "HP")
    local maxhp = sam_get_stat(event.player, "MAXHP")
    -- Per player and per run. sam_save_data would write one file for the whole mod, so in
    -- co-op two Paladins would share the charge and it would survive into the next run.
    local used = sam_get_player_data(event.player, "intervened") or 0
    if hp and maxhp and used == 0 and hp <= maxhp * 0.30 then
      sam_modify_damage(event.player, 0)
      sam_set_stat(event.player, "HP", math.min(maxhp, hp + 15))
      sam_set_player_data(event.player, "intervened", 1)
      sam_message(event.player, "Divine intervention shields you from a mortal blow!")
    end

  elseif event.name == "player.on_floor_change" then
    if not mine(event.player) then return end
    sam_set_player_data(event.player, "intervened", 0)
  end
end
