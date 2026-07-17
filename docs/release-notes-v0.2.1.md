# Disco Boy 0.2.1

Disco Boy is an optional Catastrophe-native music player for Leaf on the
Miniloong Pocket 1. Install or update it through Pak Rat.

## Changes since 0.1.1

- Scan the ordered `Music` roots on both SD cards.
- Keep physical source identity so duplicate albums or tracks remain available.
- Separate the two cards in the Folders view when both are mounted.
- Tolerate an absent secondary card.
- Keep launching with the primary Music root when older firmware exports an
  incomplete plural-path environment.
- Restore strict validation when both plural Music and SD-card lists are
  present.

The update preserves music files and app-independent library content. Disco Boy
remains optional and is not included in Leaf SD release ZIPs.
