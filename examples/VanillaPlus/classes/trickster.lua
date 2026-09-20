-- Trickster — fights dirty. A cunning strike lands extra, and slipping on the cloak
-- drops you into a brief vanish.
--
-- The on-hit proc is bonus sneak damage rather than an enemy debuff, because this class
-- already carries a DUST_BALL for the blinding cloud and a thrown item reads better than a
-- silent status change. (If you do want to debuff a monster from a script, that is
-- sam_apply_monster_effect(uid, "BLIND", ticks); it takes the same effect names.)

local CLASS = "vanillaplus:trickster"

-- Once a script is loaded it receives EVERY event, whatever content it shipped beside:
-- dispatch is global, not scoped to this class. So every handler starts by asking whose
-- event this is and whether that player actually picked this class. Without this line every
-- class in the game would get the bonus-damage proc and the cloak vanish.
local function mine(player)
  return player ~= nil and player >= 0 and sam_get_class(player) == CLASS
end

function on_event(event)
  if event.name == "player.on_hit" then
    -- ~20% chance: a sneaky follow-up cut for bonus damage.
    if not mine(event.player) then return end
    if event.target_uid and math.random(100) <= 20 then
      sam_deal_damage(event.target_uid, 6)
      sam_message(event.player, "A cunning strike slips past their guard!")
    end

  elseif event.name == "player.on_equip" then
    -- Donning the cloak drops you into a brief vanish (~2s).
    if not mine(event.player) then return end
    if event.slot == "cloak" then
      sam_apply_effect(event.player, "INVISIBLE", 100)
      sam_message(event.player, "You pull up the cloak and melt into the shadows.")
    end
  end
end
