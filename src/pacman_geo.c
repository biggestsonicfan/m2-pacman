/*
 * pacman_geo.c — src/pacman.c with its sprites drawn as GEO polygons (m2-sdk m2_sprite.h).
 *
 * The stock build plots the sprites into the tile plane's chars, so char RAM changes every
 * frame. Here each sprite is a textured quad per pen and the char RAM only changes with the
 * tiles. Needs the SHARC firmware (src/cpres1.h, cpres2.h), like src/pacman.c.
 *
 * Build:  cmake ... -DM2_GAME=pacman_geo  ->  roms/pacman_geo/game.bin
 */
#define PAC_GEO_SPRITES
#include "pacman.c"
