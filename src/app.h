/*
 * Application interface (Rev 2.14).
 *
 * main.c owns initialisation and the main loop; the application supplies
 * two functions. Exactly one application is built, chosen by the CMake
 * option APP_MODE:
 *
 *   user     app_user.c - the production application. app_poll() is called
 *            once per 1 ms epoch, aligned to the tick (EX-10).
 *   echo     app_test.c - test application, echo mode (V-1, V-2, V-3).
 *   pattern  app_test.c - test application, counting pattern (V-1, V-4).
 *            The test applications run app_poll() as fast as possible.
 *
 * app_poll() must never block (EX-2) and must finish well inside the
 * epoch: worst case < 1 ms (EX-8).
 */
#ifndef APP_H
#define APP_H

/* Once, after the drivers are initialised and before the main loop. */
void app_init(void);

/* User application: once per epoch. Test applications: every pass. */
void app_poll(void);

#endif /* APP_H */
