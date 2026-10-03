INCLUDE ipl-ram.ld
__stack = 0xf83800;
/* Installed pce-mkcd resolves only absolute entry-point symbols. */
_start = ABSOLUTE(_start);
