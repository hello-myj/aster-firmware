/* SPDX-License-Identifier: MIT. Included after network and HTTP helpers. */
#if defined(CONFIG_ASTER_MQTT)
#include "aster_mqtt.h"
_Static_assert(sizeof(aster_mqtt_options_t) == 80, "MQTT options ABI");
_Static_assert(sizeof(aster_mqtt_status_t) == 64, "MQTT status ABI");
_Static_assert(sizeof(aster_mqtt_message_t) == 1176, "MQTT message ABI");
_Static_assert(ASTER_MQTT_READY == ASTER_MQTT_STATE_READY &&
    ASTER_MQTT_CANCELLED == ASTER_MQTT_STATE_CANCELLED, "MQTT states ABI");
static int32_t mqtt_error(int rc, const struct aster_mqtt_snapshot *s)
{
    if (rc >= 0) return rc;
    if (rc == -ENOENT) return ASTER_ENOENT;
    if (rc == -EPROTO) return ASTER_EPROTO;
    if (rc == -ENODATA) return ASTER_ECLOCK;
    if (rc == -EACCES)
        return s && http_certificate_flags(s->tls_verify) ? ASTER_ECERT : ASTER_EPERM;
    if (rc == -EIO && s && s->tls_error && !s->transport_error) return ASTER_ETLS;
    return network_error(rc);
}
static int32_t n_mqtt_connect(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    void *p = size >= sizeof(aster_mqtt_options_t)
        ? network_buf(env, off, sizeof(aster_mqtt_options_t)) : NULL;
    if (!p) return ASTER_EINVAL;
    aster_mqtt_options_t v; memcpy(&v, p, sizeof(v));
    if (v.size != sizeof(v) || v.version != 1 || v.reserved || (v.flags & ~7u))
        return ASTER_EINVAL;
    char host[254], client[65], user[129], topic[129];
    if (!network_host(env, v.host.offset, v.host.length, host) ||
        !http_string(env, v.client_id.offset, v.client_id.length, 64, client, false) ||
        !http_string(env, v.username.offset, v.username.length, 128, user, true) ||
        !http_string(env, v.will_topic.offset, v.will_topic.length, 128, topic, true) ||
        v.password.length > 128 || v.will_payload.length > 256 ||
        (!v.password.length && v.password.offset) ||
        (!v.will_payload.length && v.will_payload.offset)) return ASTER_EINVAL;
    void *password = v.password.length
        ? network_buf(env, v.password.offset, v.password.length) : NULL;
    void *will = v.will_payload.length
        ? network_buf(env, v.will_payload.offset, v.will_payload.length) : NULL;
    if ((v.password.length && !password) || (v.will_payload.length && !will))
        return ASTER_EINVAL;
    struct aster_mqtt_connect o = { .host=host, .client_id=client,
        .username=v.username.length ? user : NULL, .password=password,
        .password_length=v.password.length, .will_topic=v.will_topic.length ? topic : NULL,
        .will_payload=will, .will_length=v.will_payload.length,
        .port=v.port, .timeout_ms=v.timeout_ms, .keepalive_s=v.keepalive_s,
        .will_qos=v.will_qos, .tls=!!(v.flags & ASTER_MQTT_FLAG_TLS),
        .reconnect=!!(v.flags & ASTER_MQTT_FLAG_RECONNECT),
        .will_retain=!!(v.flags & ASTER_MQTT_FLAG_WILL_RETAIN) };
    return mqtt_error(aster_mqtt_svc_open(owner, &o), NULL);
}
static int32_t n_mqtt_status(wasm_exec_env_t env, uint32_t h, uint32_t off, uint32_t size)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    void *p = size >= sizeof(aster_mqtt_status_t)
        ? network_buf(env, off, sizeof(aster_mqtt_status_t)) : NULL;
    if (!p) return ASTER_EINVAL;
    struct aster_mqtt_snapshot s;
    rc = aster_mqtt_svc_status(owner, h, &s);
    if (rc) return mqtt_error(rc, NULL);
    aster_mqtt_status_t out = { .size=sizeof(out), .version=1, .state=s.state,
        .error=mqtt_error(s.error, &s), .reconnects=s.reconnects, .queued=s.queued,
        .pending=s.pending, .subscriptions=s.subscriptions, .received=s.received,
        .sent=s.sent, .connack=s.connack, .last_operation=s.last_operation,
        .last_operation_kind=s.last_operation_kind,
        .operation_error=mqtt_error(s.operation_error, NULL),
        .certificate_flags=http_certificate_flags(s.tls_verify) };
    memcpy(p, &out, sizeof(out)); return ASTER_OK;
}
static int32_t n_mqtt_publish(wasm_exec_env_t env, uint32_t h, uint32_t off,
    uint32_t n, uint32_t data_off, uint32_t length, uint32_t qos, uint32_t retain)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    char topic[129];
    if (length > ASTER_MQTT_PAYLOAD_LIMIT) return ASTER_EMSGSIZE;
    if (!http_string(env, off, n, 128, topic, false) || qos > 1 || retain > 1 ||
        (!length && data_off)) return ASTER_EINVAL;
    void *data = length ? network_buf(env, data_off, length) : NULL;
    if (length && !data) return ASTER_EINVAL;
    return mqtt_error(aster_mqtt_svc_publish(owner, h, topic, data, length, qos, retain), NULL);
}
static int32_t mqtt_subscription(wasm_exec_env_t env, uint32_t h,
    uint32_t off, uint32_t n, uint32_t qos, bool unsubscribe)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    char filter[129];
    if (qos > 1 || !http_string(env, off, n, 128, filter, false)) return ASTER_EINVAL;
    return mqtt_error(aster_mqtt_svc_subscribe(owner,h,filter,qos,unsubscribe),NULL);
}
static int32_t n_mqtt_subscribe(wasm_exec_env_t env,uint32_t h,uint32_t o,uint32_t n,uint32_t q)
{ return mqtt_subscription(env,h,o,n,q,false); }
static int32_t n_mqtt_unsubscribe(wasm_exec_env_t env,uint32_t h,uint32_t o,uint32_t n)
{ return mqtt_subscription(env,h,o,n,0,true); }
static int32_t n_mqtt_operation(wasm_exec_env_t env,uint32_t h,uint32_t sequence,uint32_t off)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    void *p = network_buf(env,off,4); if (!p) return ASTER_EINVAL;
    int32_t error;
    rc = aster_mqtt_svc_operation(owner,h,sequence,&error);
    if (rc) return mqtt_error(rc,NULL);
    error = mqtt_error(error,NULL); memcpy(p,&error,4); return ASTER_OK;
}
static int32_t n_mqtt_recv(wasm_exec_env_t env,uint32_t h,uint32_t off,uint32_t size)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    void *p = size >= sizeof(aster_mqtt_message_t)
        ? network_buf(env,off,sizeof(aster_mqtt_message_t)) : NULL;
    if (!p) return ASTER_EINVAL;
    struct aster_mqtt_message m;
    rc = aster_mqtt_svc_recv(owner,h,&m);
    if (rc) {
        if (rc == -EAGAIN) return ASTER_EAGAIN;
        struct aster_mqtt_snapshot s;
        int sr = aster_mqtt_svc_status(owner,h,&s);
        if (sr == -EAGAIN) return ASTER_EAGAIN;
        return mqtt_error(rc,sr ? NULL : &s);
    }
    aster_mqtt_message_t out = { .size=sizeof(out), .version=1, .length=m.length,
        .topic_length=strlen(m.topic), .qos=m.qos,
        .flags=(m.retain ? ASTER_MQTT_MESSAGE_RETAIN : 0) | (m.dup ? ASTER_MQTT_MESSAGE_DUP : 0) };
    memcpy(out.topic,m.topic,out.topic_length); memcpy(out.data,m.data,m.length);
    memcpy(p,&out,sizeof(out)); return ASTER_OK;
}
static int32_t n_mqtt_cancel(wasm_exec_env_t env,uint32_t h)
{
    uint64_t owner; int32_t rc=network_gate(env,&owner);
    return rc ? rc : mqtt_error(aster_mqtt_svc_cancel(owner,h),NULL);
}
static int32_t n_mqtt_close(wasm_exec_env_t env,uint32_t h)
{
    uint64_t owner; int32_t rc=network_gate(env,&owner);
    return rc ? rc : mqtt_error(aster_mqtt_svc_close(owner,h),NULL);
}
#else
#define MQTT_STUB(name,args) static int32_t name args { return ASTER_ENOTSUP; }
MQTT_STUB(n_mqtt_connect,(wasm_exec_env_t e,uint32_t o,uint32_t n))
MQTT_STUB(n_mqtt_status,(wasm_exec_env_t e,uint32_t h,uint32_t o,uint32_t n))
MQTT_STUB(n_mqtt_publish,(wasm_exec_env_t e,uint32_t h,uint32_t o,uint32_t n,uint32_t d,uint32_t l,uint32_t q,uint32_t r))
MQTT_STUB(n_mqtt_subscribe,(wasm_exec_env_t e,uint32_t h,uint32_t o,uint32_t n,uint32_t q))
MQTT_STUB(n_mqtt_unsubscribe,(wasm_exec_env_t e,uint32_t h,uint32_t o,uint32_t n))
MQTT_STUB(n_mqtt_operation,(wasm_exec_env_t e,uint32_t h,uint32_t s,uint32_t o))
MQTT_STUB(n_mqtt_recv,(wasm_exec_env_t e,uint32_t h,uint32_t o,uint32_t n))
MQTT_STUB(n_mqtt_cancel,(wasm_exec_env_t e,uint32_t h))
MQTT_STUB(n_mqtt_close,(wasm_exec_env_t e,uint32_t h))
#undef MQTT_STUB
#endif
