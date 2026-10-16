/* SPDX-License-Identifier: MIT
 * Internal service contract; private errno, copied inputs, session owners.
 * Public SDK/ABI is contract/include/aster/mqtt.h. No Wi-Fi configuration.
 */
#ifndef ASTER_MQTT_SERVICE_H
#define ASTER_MQTT_SERVICE_H
#include "aster_mqtt_codec.h"
#define ASTER_MQTT_COMPLETIONS 4
#define ASTER_MQTT_CLI_OWNER UINT64_C(1)
enum aster_mqtt_state {
    ASTER_MQTT_CONNECTING = 1, ASTER_MQTT_HANDSHAKE, ASTER_MQTT_CONNACK,
    ASTER_MQTT_READY, ASTER_MQTT_BACKOFF, ASTER_MQTT_FAILED, ASTER_MQTT_CANCELLED
};
struct aster_mqtt_snapshot {
    uint32_t state, reconnects, queued, pending, subscriptions, received, sent,
        connack, last_operation, last_operation_kind;
    int32_t error, operation_error;
    uint32_t tls_verify;
    int32_t tls_error, transport_error;
};
struct aster_mqtt_message {
    char topic[ASTER_MQTT_TOPIC_MAX + 1];
    uint8_t data[ASTER_MQTT_PAYLOAD_MAX];
    uint32_t length, qos, retain, dup;
};
struct aster_mqtt_stats { uint32_t active, closing, tls_active, psram_bytes; };
/* clean_session=true only; QoS1 in flight is failed on connection loss, not
 * silently retried as business success. Reconnect restores confirmed filters.
 * At most two queued operations, four filters, two received messages/client.
 * No offline publish queue. All data copied before return. */
int aster_mqtt_svc_open(uint64_t owner, const struct aster_mqtt_connect *options);
/* Returns positive operation identifier, not broker/business delivery. */
int aster_mqtt_svc_publish(uint64_t owner, uint32_t handle, const char *topic,
    const void *data, size_t length, unsigned qos, bool retain);
int aster_mqtt_svc_subscribe(uint64_t owner, uint32_t handle, const char *filter,
    unsigned qos, bool unsubscribe);
int aster_mqtt_svc_status(uint64_t owner, uint32_t handle, struct aster_mqtt_snapshot *out);
/* Query one of the last four completion records. EAGAIN=pending,
 * ENOENT=unknown/evicted; on success operation_error is private errno or 0.
 * Callers must inspect completion before issuing enough work to evict it. */
int aster_mqtt_svc_operation(uint64_t owner, uint32_t handle, uint32_t sequence,
                             int32_t *operation_error);
/* Returns 0 for a message including empty payload; EAGAIN means no message.
 * Whole message is copied before queue consumption; no short buffer API. */
int aster_mqtt_svc_recv(uint64_t owner, uint32_t handle, struct aster_mqtt_message *out);
int aster_mqtt_svc_cancel(uint64_t owner, uint32_t handle);
/* Handle invalid immediately. Graceful DISCONNECT gets a bounded drain;
 * unavailable links and timeout may still cause broker Will publication. */
int aster_mqtt_svc_close(uint64_t owner, uint32_t handle);
int aster_mqtt_svc_close_owner(uint64_t owner);
int aster_mqtt_svc_stats(struct aster_mqtt_stats *out);
/* Only shared bounded network worker calls tick; no shell/APP network waits. */
bool aster_mqtt_svc_tick(void);
#endif
