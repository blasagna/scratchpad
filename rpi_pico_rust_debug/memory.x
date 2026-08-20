MEMORY {
    BOOT2 : ORIGIN = 0x10000000, LENGTH = 0x100
    FLASH : ORIGIN = 0x10000100, LENGTH = 2048K - 0x100
    RAM   : ORIGIN = 0x20000000, LENGTH = 264K
}

/* No `.boot2` SECTIONS block and no EXTERN here: embassy-rp places the second
   stage bootloader itself (a `#[link_section = ".boot2"]` static) and its own
   `link-rp.x.in` — pulled in by build.rs via `-Tlink-rp.x` — already defines
   the output section. Upstream `embassy/examples/rp/memory.x` is MEMORY-only
   for the same reason. */
