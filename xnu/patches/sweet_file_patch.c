/*
 * sweet: boot-time file patcher.
 *
 * The iOS userland image is not on the Mac any more (the 9 GB userdata
 * image was lost), so every change to a file that lives on the phone's
 * root volume has to be delivered by the kernel.  This patcher does that:
 * it rewrites a few bytes in a file after the volume is mounted, using the
 * in-kernel VFS API (vnode_open + vn_rdwr).
 *
 * Boot argument:
 *
 *   sweetfilepatch=<path>:<off>:<oldhex>:<newhex>[,<path>:<off>:<oldhex>:<newhex>...]
 *
 * <off> is hexadecimal, <oldhex>/<newhex> are hex byte strings of the same
 * length.  The old bytes are verified before the write: if the file does
 * not contain them we log a mismatch and leave the file alone, so a
 * different build of the same binary is never corrupted.
 *
 * The path alias "mad" expands to /private/usr/libexec/mobileactivationd
 * to keep the kernel command line short.
 *
 * The patch thread starts with the root device driver, long before the
 * root volume is mounted, so it retries until the file shows up.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <libkern/libkern.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/kauth.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>

void sweet_filepatch_init(void);

#define SFP_MAX_PATCH   8
#define SFP_MAX_BYTES   16
#define SFP_PATHLEN     160
#define SFP_ARGLEN      256

struct sfp_patch {
	char     path[SFP_PATHLEN];
	uint64_t off;
	uint32_t len;
	uint8_t  oldb[SFP_MAX_BYTES];
	uint8_t  newb[SFP_MAX_BYTES];
	int      done;
};

static struct sfp_patch sfp[SFP_MAX_PATCH];
static int sfp_count;

static int
sfp_hexval(int c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static int
sfp_unhex(const char *hex, uint8_t *buf, int max)
{
	int n = 0;

	while (hex[0] != '\0' && hex[1] != '\0' && n < max) {
		int hi = sfp_hexval((unsigned char)hex[0]);
		int lo = sfp_hexval((unsigned char)hex[1]);
		if (hi < 0 || lo < 0) {
			return -1;
		}
		buf[n++] = (uint8_t)((hi << 4) | lo);
		hex += 2;
	}
	return (hex[0] == '\0') ? n : -1;
}

static uint64_t
sfp_parse_off(const char *s)
{
	uint64_t v = 0;
	int any = 0;

	for (;;) {
		int d = sfp_hexval((unsigned char)*s);
		if (d < 0) {
			break;
		}
		v = (v << 4) | (uint64_t)d;
		any = 1;
		s++;
	}
	return any ? v : (uint64_t)-1;
}

static void
sfp_add(const char *path, uint64_t off, const char *oldhex, const char *newhex)
{
	struct sfp_patch *p;
	int oldlen, newlen;

	if (sfp_count >= SFP_MAX_PATCH) {
		printf("SWEET FILEPATCH: too many patches\n");
		return;
	}
	p = &sfp[sfp_count];
	oldlen = sfp_unhex(oldhex, p->oldb, SFP_MAX_BYTES);
	newlen = sfp_unhex(newhex, p->newb, SFP_MAX_BYTES);
	if (oldlen <= 0 || oldlen != newlen) {
		printf("SWEET FILEPATCH: bad hex in \"%s\"\n", path);
		return;
	}
	p->len = (uint32_t)oldlen;
	p->off = off;
	if (strcmp(path, "mad") == 0) {
		strlcpy(p->path, "/private/usr/libexec/mobileactivationd", SFP_PATHLEN);
	} else {
		strlcpy(p->path, path, SFP_PATHLEN);
	}
	sfp_count++;
}

static void
sfp_parse(char *spec)
{
	char *p = spec;

	while (p != NULL && *p != '\0') {
		char *comma = strchr(p, ',');
		char *c1, *c2, *c3;

		if (comma != NULL) {
			*comma = '\0';
		}
		c1 = strchr(p, ':');
		if (c1 == NULL) {
			break;
		}
		*c1++ = '\0';
		c2 = strchr(c1, ':');
		if (c2 == NULL) {
			break;
		}
		*c2++ = '\0';
		c3 = strchr(c2, ':');
		if (c3 == NULL) {
			break;
		}
		*c3++ = '\0';
		sfp_add(p, sfp_parse_off(c1), c2, c3);
		p = (comma != NULL) ? comma + 1 : NULL;
	}
}

static int
sfp_apply(struct sfp_patch *p)
{
	vnode_t vp = NULL;
	vfs_context_t ctx = vfs_context_kernel();
	uint8_t buf[SFP_MAX_BYTES];
	int resid = 0;
	int err;

	err = vnode_open(p->path, FREAD | FWRITE, 0, 0, &vp, ctx);
	if (err != 0) {
		return err;
	}
	err = vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)p->len, (off_t)p->off,
	    UIO_SYSSPACE, IO_NODELOCKED | IO_NOAUTH, kauth_cred_get(), &resid, NULL);
	if (err != 0 || resid != 0) {
		printf("SWEET FILEPATCH: read %s@0x%llx failed (err %d resid %d)\n",
		    p->path, (unsigned long long)p->off, err, resid);
		vnode_close(vp, 0, ctx);
		return (err != 0) ? err : EIO;
	}
	if (memcmp(buf, p->oldb, p->len) != 0) {
		printf("SWEET FILEPATCH: %s@0x%llx mismatch (have %02x%02x%02x%02x,"
		    " want %02x%02x%02x%02x) - skipped\n",
		    p->path, (unsigned long long)p->off,
		    (p->len > 0) ? buf[0] : 0, (p->len > 1) ? buf[1] : 0,
		    (p->len > 2) ? buf[2] : 0, (p->len > 3) ? buf[3] : 0,
		    (p->len > 0) ? p->oldb[0] : 0, (p->len > 1) ? p->oldb[1] : 0,
		    (p->len > 2) ? p->oldb[2] : 0, (p->len > 3) ? p->oldb[3] : 0);
		vnode_close(vp, 0, ctx);
		return EIO;
	}
	err = vn_rdwr(UIO_WRITE, vp, (caddr_t)p->newb, (int)p->len, (off_t)p->off,
	    UIO_SYSSPACE, IO_NODELOCKED | IO_NOAUTH, kauth_cred_get(), &resid, NULL);
	if (err != 0 || resid != 0) {
		printf("SWEET FILEPATCH: write %s@0x%llx failed (err %d resid %d)\n",
		    p->path, (unsigned long long)p->off, err, resid);
		vnode_close(vp, 0, ctx);
		return (err != 0) ? err : EIO;
	}
	printf("SWEET FILEPATCH: patched %s@0x%llx (%u bytes)\n",
	    p->path, (unsigned long long)p->off, p->len);
	vnode_close(vp, 0, ctx);
	return 0;
}

static void
sfp_thread(void *arg, wait_result_t result)
{
	int tries, i;

	(void)arg;
	(void)result;

	for (tries = 0; tries < 180; tries++) {
		int pending = 0;

		for (i = 0; i < sfp_count; i++) {
			if (sfp[i].done) {
				continue;
			}
			pending++;
			if (sfp_apply(&sfp[i]) == 0) {
				sfp[i].done = 1;
			}
		}
		if (pending == 0) {
			break;
		}
		/* the root volume is mounted a little after this thread starts */
		delay(1000 * 1000);
	}
	for (i = 0; i < sfp_count; i++) {
		if (!sfp[i].done) {
			printf("SWEET FILEPATCH: %s NOT patched\n", sfp[i].path);
		}
	}
	printf("SWEET FILEPATCH: done\n");
}

void
sweet_filepatch_init(void)
{
	char spec[SFP_ARGLEN];
	thread_t t;

	if (!PE_parse_boot_argn("sweetfilepatch", spec, sizeof(spec))) {
		return;
	}
	sfp_parse(spec);
	if (sfp_count == 0) {
		printf("SWEET FILEPATCH: no valid patches in \"%s\"\n", spec);
		return;
	}
	printf("SWEET FILEPATCH: %d patch(es) queued\n", sfp_count);
	if (kernel_thread_start(sfp_thread, NULL, &t) == KERN_SUCCESS) {
		thread_deallocate(t);
	} else {
		printf("SWEET FILEPATCH: thread start failed\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
