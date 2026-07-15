-- Trickster — fights dirty. A cunning strike lands extra, and slipping on the cloak
-- drops you into a brief vanish.
--
-- NOTE: Lua effects (sam_apply_effect) can only target YOURSELF, not monsters, so a
-- "smoke bomb that blinds enemies" can't be done in script — that's what the class's
-- DUST_BALL item is for (throw it for a real blinding cloud). Here the on-hit proc is
-- bonus sneak damage instead of an enemy debuff.

function on_event(event)
  if event.name == "player.on_hit" then
    -- ~20% chance: a sneaky follow-up cut for bonus damage.
    if event.target_uid and math.random(100) <= 20 then
      sam_deal_damage(event.target_uid, 6)
      sam_message(event.player, "A cunning strike slips past their guard!")
    end

  elseif event.name == "player.on_equip" then
    -- Donning the cloak drops you into a brief vanish (~2s).
    if event.slot == "cloak" then
      sam_apply_effect(event.player, "INVISIBLE", 100)
      sam_message(event.player, "You pull up the cloak and melt into the shadows.")
    end
  end
end
