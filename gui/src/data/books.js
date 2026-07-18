/*
 * Barony's readable books, in the exact order they load from books/compiled_books.json.
 *
 * A READABLE_BOOK item stores no title — the engine picks the book by the item's `appearance`
 * field, used as `appearance % numbooks` to index this list (items.cpp getBookLocalizedName-
 * FromIndex, book.cpp allBooks built in compiled_books.json member order). So the ARRAY INDEX
 * here IS the appearance value to write. A starting READABLE_BOOK with appearance 25 reads as
 * "The Lusty Goblin Maid" in game.
 *
 * Vanilla ships 34 books (indices 0-33). A book mod that adds .txt files extends the list, so
 * an index chosen here can shift if such a mod is also loaded — the modulo means it never
 * crashes, just wraps. This is the stock set; keep it in this exact order.
 */
export const BOOKS = [
  'A Brief Survey of Goblins',
  'Assessing the ZAP Brigade',
  'Bottle Book',
  'Cave Beasts',
  'Character Attributes',
  'Citadel Servant FAQ',
  'Concerning the Undead',
  'Controlling Goblins',
  'Dethroning Herx',
  'How to be Strong',
  'Lost Journal',
  "Miner's Christmas",
  'Mining My Soul',
  'My Journal',
  'Newspaper Clipping',
  'On Giantism, Vol 1',
  'On Giantism, Vol 2',
  'Poem of the Mines',
  'Sightings of the Lich',
  'Surviving the Mines',
  'The Adventurer Who Went To Hell',
  'The Art of Mine Warfare',
  'The Campaign',
  'The Flying Minecart',
  'The History of Baron Herx',
  'The Lusty Goblin Maid',
  'The Meaning of the Labyrinth',
  'The Pirate King',
  'To Emily',
  "Winny's Report",
  "Worker's Journal, Entry 1",
  "Worker's Journal, Entry 2",
  "Worker's Journal, Entry 3",
  "Worker's Journal, Entry 4",
];

/** Title for an appearance value (wraps like the engine's `appearance % numbooks`). */
export function bookTitle(appearance) {
  const n = Number(appearance) || 0;
  return BOOKS[((n % BOOKS.length) + BOOKS.length) % BOOKS.length];
}
