#ifndef RESONATE_MODULE_SIGNAL_H
#define RESONATE_MODULE_SIGNAL_H

/*
 * Signals: same-frame notification for events that cannot wait for a flush
 * point, such as a viewport resize or an input release.
 *
 * A signal is a private member of the object that owns the event; the connection
 * node lives inside the subscriber.
 *
 * Subscribers are notified in connection order, and a subscriber may disconnect
 * from inside its own callback. A subscriber connected during an emit is
 * notified from the next one.
 *
 * `ResonateSignalStorage`, `ResonateSignalNode` and `ResonateSignalInvoke` are
 * declared in abi.h, and a module reaches the operations through
 * `ResonateHostApi` — never through this header, which the host uses to
 * implement them.
 */

#include "resonate/module/abi.h"

#ifdef __cplusplus
extern "C"
{
#endif

ResonateStatus resonate_signal_create(ResonateSignalStorage** out_storage,
                                      const ResonateHostApi* host);
void resonate_signal_destroy(ResonateSignalStorage* storage);

/* Fails with RESONATE_E_INVALID if an argument is null or the node is already
   connected; the node must outlive the connection. */
ResonateStatus resonate_signal_connect(ResonateSignalStorage* storage, ResonateSignalNode* node,
                                       ResonateSignalInvoke invoke);
void resonate_signal_disconnect(ResonateSignalStorage* storage, ResonateSignalNode* node);

/* Returns 1 if at least one subscriber ran, 0 if there were none. */
uint32_t resonate_signal_emit(ResonateSignalStorage* storage, const void* payload);

/* Stops after the first subscriber that returns non-zero, and returns that value. */
uint32_t resonate_signal_emit_until(ResonateSignalStorage* storage, const void* payload);

uint32_t resonate_signal_subscriber_count(const ResonateSignalStorage* storage);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_MODULE_SIGNAL_H */
