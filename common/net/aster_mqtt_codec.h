/* SPDX-License-Identifier: MIT */
#ifndef ASTER_MQTT_CODEC_H
#define ASTER_MQTT_CODEC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ASTER_MQTT_TOPIC_MAX 128
#define ASTER_MQTT_PAYLOAD_MAX 1024
#define ASTER_MQTT_PACKET_MAX 1536
#define ASTER_MQTT_TOPICS 4
#define ASTER_MQTT_DEPTH 2
struct aster_mqtt_connect {
    const char *host, *client_id, *username;
    const void *password;
    size_t password_length;
    const char *will_topic;
    const void *will_payload;
    size_t will_length;
    uint32_t port, timeout_ms, keepalive_s, will_qos;
    bool tls, reconnect, will_retain;
};
struct aster_mqtt_packet {
    unsigned type, qos;
    bool retain, dup;
    uint16_t id;
    const uint8_t *body, *topic, *payload;
    size_t body_length, topic_length, payload_length, wire_length;
};
/* Strict bounded MQTT 3.1.1 UTF-8/topic validation; wire errors are EPROTO. */
int aster_mqtt_topic(const char *topic, bool filter);
int aster_mqtt_connect_check(const struct aster_mqtt_connect *o);
int aster_mqtt_encode_connect(uint8_t *out, size_t cap,
                               const struct aster_mqtt_connect *o);
int aster_mqtt_encode_publish(uint8_t *out, size_t cap, const char *topic,
                               const void *data, size_t length, unsigned qos,
                               bool retain, uint16_t id);
int aster_mqtt_encode_sub(uint8_t *out, size_t cap, const char *topic,
                           unsigned qos, uint16_t id, bool unsubscribe);
/* 0 means incomplete (not EOF), positive full packet length, negative errno.
 * Does not consume input; a full application queue may leave it pending. */
int aster_mqtt_decode(const uint8_t *data, size_t size,
                       struct aster_mqtt_packet *packet);
#endif
