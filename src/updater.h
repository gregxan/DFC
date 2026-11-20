/*
 * Firmware updater (Rev 2.25, work in progress: step 2 of the plan).
 *
 * Entry is always by reboot, never by jumping into the updater from the
 * running application:
 *
 *   1. The application (after checking its own magic key in its protocol)
 *      or the diag console ('!') calls updater_request(link, key).
 *   2. updater_request() checks the 64-bit request key. If it is right, it
 *      writes the boot flag and reboots. If not, it returns false and does
 *      nothing else.
 *   3. At the top of main(), updater_check_boot() reads the boot flag and
 *      clears it at once, whatever it held. A flag that is valid in every
 *      bit (64-bit boot key, link, check word) runs the updater, which
 *      never returns. Anything else boots normally.
 *
 * The boot flag lives in POWMAN scratch registers 0-3. They survive the
 * reboot, but a power cycle or the RESET button (RUN pin) clears them
 * (datasheet table 560).
 *
 * The updater listens on the requested link and serves the host's commands
 * (tools/dfc_update.py). With no valid command for UPDATER_IDLE_TIMEOUT_S,
 * it reboots into the current firmware. Step 2 implements the USB link
 * (diag builds) with HELLO and REBOOT. Flash writes, trial boot and the UART
 * links follow in later steps.
 */
#ifndef UPDATER_H
#define UPDATER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    UPDATER_LINK_UART0 = 0,
    UPDATER_LINK_UART1 = 1,
    UPDATER_LINK_USB = 2, /* diagnostic builds only */
} updater_link_t;

/* The key a caller must pass to updater_request(). It only guards against
 * accidental calls: anything built with this header can use it. The boot
 * flag uses a separate key, which only updater.c knows. */
#define UPDATER_REQUEST_KEY 0xDFCC0DEB007ED00Dull /* "DFC CODE BOOTED DOOD" */

/* Updater protocol version. Raise it whenever the host protocol changes in
 * a way older tools cannot drive. It goes into every image's updater info
 * block, and HELLO reports it. */
#define UPDATER_PROTOCOL 1u

/* Lowest updater protocol an image may carry to be installed by this
 * firmware. Raise it in a release once the tools no longer support an older
 * protocol, so such an image can never be installed again (it would leave
 * the unit unreachable). Checked on the board at FINISH. */
#define UPDATER_PROTOCOL_FLOOR 1u

/* Key the host must send in BEGIN to install an image WITHOUT a trial boot
 * (UPD_BEGIN_NO_TRIAL): the image becomes permanent as soon as it boots, with
 * no confirm and no automatic fallback. tools/dfc_update.py makes the user
 * type it. Only for deliberately installing firmware that has no DFC
 * updater (or must not be trial-booted). */
#define UPDATER_NO_TRIAL_KEY 0xDFCBADA55B007ED0ull /* "DFC BADASS BOOTED" */

/* Idle timeout: with no valid command from the host for this long, the
 * updater reboots into the current firmware. */
#ifndef UPDATER_IDLE_TIMEOUT_S
#define UPDATER_IDLE_TIMEOUT_S 90u
#endif

/* Request the updater on `link`. Returns false, with nothing changed, if the
 * key or the link is wrong (UPDATER_LINK_USB is accepted only in diagnostic
 * builds). Otherwise it reboots into the updater and does not return. Call
 * from the main loop. */
bool updater_request(updater_link_t link, uint64_t key);

/* Optional helper for applications: the generic trigger, a run of
 * UPDATER_TRIGGER_COUNT consecutive '!' characters on a link.
 *
 * Feed it every received byte of a link, in order (any chunking). It returns
 * true once the run is complete; the caller then calls
 * updater_request(link, UPDATER_REQUEST_KEY). Any other byte restarts the
 * count. The user and test applications use it on both UARTs;
 * tools/dfc_update.py sends it with --enter --uart. An application with its
 * own protocol can use its own "enter updater" message instead.
 *
 * The run carries no key, so any stream containing 20 '!' in a row enters
 * the updater. Then nothing is written: after 90 s without a host it
 * reboots back into the firmware. */
#define UPDATER_TRIGGER_CHAR '!'
#define UPDATER_TRIGGER_COUNT 20u
typedef struct {
    uint32_t run; /* consecutive trigger characters seen */
} updater_trigger_t;

bool updater_trigger_scan(updater_trigger_t *t, const uint8_t *p, size_t n);

/* Rejected updater_request() calls since boot (diagnostics). */
uint32_t updater_rejects(void);

/* True while this image runs on trial (booted by the updater, not yet
 * confirmed): the bootrom's 16.7 s watchdog is running and must not be
 * touched, so a failed image falls back to the previous one. */
bool updater_trial_pending(void);

/* First thing in main(): runs the updater (and does not return) if a valid
 * boot flag was left by updater_request(). Always clears the flag. */
void updater_check_boot(void);

#endif /* UPDATER_H */
