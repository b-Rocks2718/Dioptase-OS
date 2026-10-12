/* Emberfall scripted end-to-end pass, run as `emberfall --e2e`.
 *
 * Drives the shipping game through its public interfaces in one guest run:
 * raw PS/2 words through a stdin pipe (exactly what the terminal forwards),
 * real MMIO framebuffer readbacks, and real high-score files. Prints one
 * baseline line and leaves stats plus framebuffer dumps on SD1.
 */

#ifndef EMBER_E2E_H
#define EMBER_E2E_H

/* Run the full scripted matrix; 0 prints ***PASS, 1 prints ***FAIL. */
int run_e2e(void);

#endif
