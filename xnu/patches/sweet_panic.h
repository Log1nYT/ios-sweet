/*
 * SWEET panic capture (venus): the layout of the panic capture area and the
 * interfaces between the kernel panic path and bsd/dev/sweet_panic.c.
 *
 * The panic region (/pram + /chosen/embedded-panic-log-size, see
 * logs/research/impl/05-panic-log-carveout.md) is written by XNU itself:
 * the embedded panic header at the base, the panic log right behind it.
 * Our own copy of the panic text lives in a private slot at the very end of
 * the region, so that the two never fight over the same bytes.
 *
 * See logs/research/impl/23-panic-capture.md for the design.
 */
#ifndef _SWEET_PANIC_H_
#define _SWEET_PANIC_H_

#include <stdarg.h>
#include <stdint.h>

/* The size of our private slot at the end of the panic region. */
#define SP_SLOT_SIZE       (64u * 1024u)
/* The maximum length of the panic text we keep. */
#define SP_TEXT_MAX        4096u
/* 'SWPN' */
#define SP_MAGIC           0x4e505753u
#define SP_VERSION         1u

struct sp_slot {
	uint32_t magic;       /* SP_MAGIC */
	uint32_t version;     /* SP_VERSION */
	uint32_t len;         /* length of the text in bytes */
	uint32_t crc;         /* crc32 of the text */
	uint64_t seq;         /* one more for every panic */
	uint64_t time;        /* mach_absolute_time() of the panic */
	uint64_t caller;      /* the PC that raised the panic */
};

/* The text of our slot follows the header. */
#define SP_SLOT_TEXT(slot) ((char *)(slot) + sizeof(struct sp_slot))

/* bsd/dev/sweet_ufs.c: called once the root device is up. */
void sweet_panic_init(void);

/* osfmk/kern/debug.c: the panic path. */
void sweet_panic_capture(int is_panic, const char *fmt, va_list *args,
    uint64_t caller);

/* pexpert/arm/pe_init.c: called before the panic region is cleared at boot. */
void sweet_panic_previous_boot(uintptr_t region, unsigned int size);

/* pexpert/arm/pe_init.c: the console is up, print the previous panic. */
void sweet_panic_console_up(void);

/* bsd/dev/sweet_reboot.c: the fastboot cookie could not be set. */
void sweet_panic_fallback_reset(void);

#endif /* _SWEET_PANIC_H_ */
