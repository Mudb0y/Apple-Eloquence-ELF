/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * mark_probe -- send a sentence with three <mark>s through spd-say and
 * verify the IndexReply callback fires the right names. Manual; not in CI.
 *
 * Run AFTER installing the new sd_eloquence module:
 *   build/examples/mark_probe
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

int main(void) {
    /* Use spd-say with -m for SSML. The actual mark events are reported
     * back via SSIP "INDEX-MARK <name>" lines, but spd-say doesn't echo
     * them; this probe exists to catch crashes. For real verification,
     * tail /var/log/speech-dispatcher.log with debug enabled. */
    const char *ssml =
        "<speak>"
        "First <mark name=\"alpha\"/> "
        "second <mark name=\"beta\"/> "
        "third <mark name=\"gamma\"/>."
        "</speak>";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "spd-say -o eloquence -m '%s'", ssml);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "spd-say returned %d\n", rc);
        return 1;
    }
    fprintf(stderr,
            "mark_probe: utterance sent. Check speech-dispatcher.log for\n"
            "'INDEX-MARK alpha', 'INDEX-MARK beta', 'INDEX-MARK gamma' lines.\n");
    return 0;
}
