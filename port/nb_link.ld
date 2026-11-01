/*
 * nb_link.ld -- one program header and one output section, for the two pieces of
 * nbCode state that the PYTHON LAUNCHER has to find and fill in.
 *
 * WHY THIS IS A LINKER SCRIPT AND NOT A #pragma
 * -------------------------------------------------------------------------
 * The launch config has to satisfy three things at once:
 *
 *   1. the launcher must be able to FIND it in the image FILE (so it has file
 *      bytes: an initialized object, in an allocated section);
 *   2. the launcher must be able to find it by the value of its FIRST WORD, so
 *      that word has to be the first byte of the structure (a linker-placed
 *      magic before the object would move the object and the launcher's
 *      assumed offset apart);
 *   3. nothing else may live in the same bytes (the launcher writes a few
 *      kilobytes into it).
 *
 * A big initialized object would normally carry its whole 24 KB of zeros in the
 * .data segment, and a NOLOAD one would have no magic in the file at all.  So:
 * a section of its own, with the slot's file bytes and the rest NOLOAD, and put
 * AFTER .bss where nothing else lands.  (Before this, the config store and the
 * log ring shared .data and the launcher's write clobbered the ring: the image
 * logged happily while the launcher read zeros.)
 */
SECTIONS
{
  /* NOT NOLOAD: the slot's magic and the config's magic have to be IN THE
   * IMAGE FILE, because that is the only thing the launcher can search.  The
   * 24 KB of zeros this carries is the price of a protocol that cannot go
   * wrong -- and it is 24 KB of a ~500 KB image in a 32 MB heap. */
  .nbcodecfg : ALIGN(8)
  {
    __nbcodecfg_start = .;
    KEEP(*(.nbcodecfg))
    __nbcodecfg_end = .;
  }
}
INSERT AFTER .bss;
