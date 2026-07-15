-- Paladin — faith mends the shield-arm, and shelters the faithful at death's door.

function on_event(event)
  if event.name == "player.on_block" then
    -- Every full block mends a little (bounded to your max HP by sam_set_stat).
    local hp = sam_get_stat(event.player, "HP")
    if hp then sam_set_stat(event.player, "HP", hp + 2) end

  elseif event.name == "on_before_damage" then
    -- Divine intervention: once per floor, a blow that finds you at/below 30% HP is
    -- turned aside and you are healed. (Emulated "invincibility" — there is no
    -- EFF_INVINCIBLE, so we cancel the incoming damage directly. sam_modify_damage
    -- only works inside on_before_damage.)
    local hp = sam_get_stat(event.player, "HP")
    local maxhp = sam_get_stat(event.player, "MAXHP")
    local used = sam_load_data("intervened") or 0
    if hp and maxhp and used == 0 and hp <= maxhp * 0.30 then
      sam_modify_damage(event.player, 0)
      sam_set_stat(event.player, "HP", math.min(maxhp, hp + 15))
      sam_save_data("intervened", 1)
      sam_message(event.player, "Divine intervention shields you from a mortal blow!")
    end

  elseif event.name == "player.on_floor_change" then
    sam_save_data("intervened", 0)
  end
end
