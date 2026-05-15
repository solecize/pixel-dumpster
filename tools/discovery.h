#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <stdbool.h>

/**
 * Initialize discovery service
 * Listens for DISCOVER broadcasts and responds with ANNOUNCE
 */
bool discovery_init(const char *hostname, const char *daemon_version,
                   const char *es_version, bool browsing_events, bool launch_events,
                   const char **event_methods, int method_count);

/**
 * Start discovery listener thread
 */
void discovery_start(void);

/**
 * Stop discovery service
 */
void discovery_stop(void);

/**
 * Send periodic announcement (heartbeat)
 */
void discovery_announce(void);

#endif /* DISCOVERY_H */
