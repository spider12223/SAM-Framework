/*
 * Drives the Barony cursor's press animation (see cursor.css for the CSS side).
 *
 * The game shows cursor_hand2 while the left button is down, then lets it decay back
 * to cursor_hand over a few frames after release. We mirror that: add .sam-pressing to
 * <html> on pointerdown, and remove it a short beat after pointerup so a quick click
 * still flashes the pressed gauntlet (the game's mouseAnim decay, ~5 frames).
 *
 * Listeners are on window with capture so a release that lands outside the element
 * — or off the page entirely — still clears the state; blur covers alt-tabbing while
 * held. Right/middle buttons are ignored: the game only reacts to MenuLeftClick.
 */
const LINGER_MS = 90; // ~ the game's 0.5→0.25 decay before it reverts to the open hand

const root = document.documentElement;
let lingerTimer = null;

function press(e) {
  if (e.button !== undefined && e.button !== 0) return; // left button only
  clearTimeout(lingerTimer);
  root.classList.add('sam-pressing');
}

function release() {
  clearTimeout(lingerTimer);
  lingerTimer = setTimeout(() => root.classList.remove('sam-pressing'), LINGER_MS);
}

window.addEventListener('pointerdown', press, { capture: true });
window.addEventListener('pointerup', release, { capture: true });
window.addEventListener('pointercancel', release, { capture: true });
window.addEventListener('blur', release);
