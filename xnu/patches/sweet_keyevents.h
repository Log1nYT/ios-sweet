/*
 * SWEET key state machine: debounce, long press and the Home/Menu key.
 *
 * The raw key transitions come from bsd/dev/sweet_keys.c (the PMIC
 * poller).  They are noisy and carry no timing, so sweet_keyevents.c
 * turns them into clean HID button events before they reach the IOKit
 * HID layer (sweet_hid_key() in iokit/Kernel/arm/hid/SweetHID.cpp).
 *
 * The key indices are the ones sweet_hid_key() uses:
 *
 *   0  power / side button   Consumer page 0x0C usage 0x30 (Power)
 *   1  volume up             Consumer page 0x0C usage 0xE9
 *   2  volume down           Consumer page 0x0C usage 0xEA
 *   3  home / menu           Consumer page 0x0C usage 0x40 (Menu)
 *
 * Index 3 has no source on venus (the phone has no home button and the
 * ADT tells iOS 16 that the device has none, home-button-type = 2); it
 * is handled here so that a future source can use it unchanged.
 */
#ifndef SWEET_KEYEVENTS_H
#define SWEET_KEYEVENTS_H

#define SWEET_KEY_POWER		0
#define SWEET_KEY_VOLUME_UP	1
#define SWEET_KEY_VOLUME_DOWN	2
#define SWEET_KEY_HOME		3
#define SWEET_KEY_COUNT		4

/* Boot args:
 *   sweetkeyevents=1	enable the state machine (default: off)
 *   sweetkeylong=<ms>	long press threshold, 0 disables it (default 1200)
 *   sweetkeylongmark=0	log the long press but publish no extra event
 *   sweetkeydebounce=<ms>	debounce window (default 30)
 *   sweetkeydisplayoff=1	treat the display as off (no long press events)
 */

/* Start the state machine thread.  Safe to call more than once. */
extern void sweet_keyevents_init(void);

/* Feed one raw key transition (1 = down, 0 = up) into the state machine. */
extern void sweet_keyevents_key(int key, int down);

/* Tell the state machine that the display is off: no long press event is
 * published and every armed timer is cancelled.  Nothing calls this yet
 * (the venus display never powers off), it is the hook for a display
 * power patch.
 */
extern void sweet_keyevents_set_display_off(int off);

#endif /* SWEET_KEYEVENTS_H */
