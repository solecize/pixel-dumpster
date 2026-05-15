#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define PD_DISCOVERY_PORT 7171
#define PD_DISCOVERY_MAX_SOURCES 4
#define PD_DISCOVERY_SOURCE_TIMEOUT 120  // seconds

typedef struct {
    char source_id[64];
    char hostname[64];
    char ip[16];
    char daemon_version[16];
    char es_version[32];
    bool browsing_events;
    bool launch_events;
    char event_methods[128];
    int priority;
    uint32_t last_seen;
    bool active;
} pd_marquee_source_t;

/**
 * Initialize discovery service
 */
esp_err_t pd_discovery_init(void);

/**
 * Send discovery broadcast query
 */
void pd_discovery_send_query(void);

/**
 * Get the currently active marquee source (highest priority)
 */
pd_marquee_source_t* pd_discovery_get_active_source(void);

/**
 * Check if any source is connected
 */
bool pd_discovery_has_source(void);

/**
 * Get number of registered sources
 */
int pd_discovery_get_source_count(void);

/**
 * Get source by index
 */
pd_marquee_source_t* pd_discovery_get_source(int index);

/**
 * Get connection state for display
 * Returns: 0=searching, 1=connecting, 2=connected
 */
int pd_discovery_get_state(void);
