/*
 * Optional status reporting over USB CDC (APP_USB_STATUS=1, diagnostic
 * builds only, B-20). Once per second a snapshot is taken and emitted as a
 * few short "key=value" lines, one line per main-loop iteration, and only
 * when the CDC buffer has room for the whole line, so nothing waits on USB.
 */
#ifndef STATUS_H
#define STATUS_H

void status_init(void);
void status_poll(void);

#endif /* STATUS_H */
