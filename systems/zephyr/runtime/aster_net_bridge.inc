/* SPDX-License-Identifier: MIT
 * ABI 4.0 unified network imports. Included after the protocol bridges so
 * their checked helpers stay the only place that touches guest bytes.
 */
#if IS_ENABLED(CONFIG_ASTER_NET)
#include <aster/net.h>
_Static_assert(sizeof(aster_net_open_hdr_t) == 40, "net open header");
_Static_assert(sizeof(aster_net_http_open_t) == 24, "net http tail");
_Static_assert(sizeof(aster_net_mqtt_open_t) == 48, "net mqtt tail");
_Static_assert(sizeof(aster_net_info_t) == 44, "net info");
_Static_assert(sizeof(aster_net_status_hdr_t) == 40, "net status header");
_Static_assert(sizeof(aster_net_tcp_status_t) == 32, "net tcp status");
_Static_assert(sizeof(aster_net_http_status_t) == 32, "net http status");
_Static_assert(sizeof(aster_net_mqtt_status_t) == 32, "net mqtt status");
_Static_assert(sizeof(aster_net_ws_status_t) == 16, "net ws status");
_Static_assert(sizeof(aster_net_io_t) == 56, "net io");

static int net_take(wasm_exec_env_t env, uint32_t off, uint32_t size, void *dst, uint32_t need)
{
	if (!need || size < need) return 0;
	uint8_t *p = network_buf(env, off, need);
	if (!p) return 0;
	memcpy(dst, p, need);
	return 1;
}
static uint32_t net_class_span(uint32_t state, uint32_t ready, uint32_t closing, uint32_t terminal)
{
	if (state >= terminal) return ASTER_NET_CLASS_TERMINAL;
	if (closing && state >= closing) return ASTER_NET_CLASS_CLOSING;
	if (state >= ready) return ASTER_NET_CLASS_READY;
	return ASTER_NET_CLASS_CONNECTING;
}
static int32_t net_abandon(wasm_exec_env_t env, uint32_t proto, uint32_t internal)
{
	if (proto == ASTER_NET_PROTO_TCP || proto == ASTER_NET_PROTO_UDP)
		return n_network_close(env, internal);
#if defined(CONFIG_ASTER_HTTP)
	if (proto == ASTER_NET_PROTO_HTTP) return n_http_close(env, internal);
#endif
#if defined(CONFIG_ASTER_MQTT)
	if (proto == ASTER_NET_PROTO_MQTT) return n_mqtt_close(env, internal);
#endif
#if defined(CONFIG_ASTER_WEBSOCKET)
	if (proto == ASTER_NET_PROTO_WS) return n_ws_close(env, internal);
#endif
	return ASTER_ENOTSUP;
}
#if defined(CONFIG_ASTER_MQTT)
static int32_t net_mqtt_open(wasm_exec_env_t env, uint64_t owner,
	const aster_net_open_hdr_t *hdr, const aster_net_mqtt_open_t *tail)
{
	if (hdr->flags & ~7u) return ASTER_EINVAL;
	char host[254], client[65], user[129], topic[129];
	if (!network_host(env, hdr->target.offset, hdr->target.length, host) ||
	    !http_string(env, tail->client_id.offset, tail->client_id.length, 64, client, false) ||
	    !http_string(env, tail->username.offset, tail->username.length, 128, user, true) ||
	    !http_string(env, tail->will_topic.offset, tail->will_topic.length, 128, topic, true) ||
	    tail->password.length > 128 || tail->will_payload.length > 256 ||
	    (!tail->password.length && tail->password.offset) ||
	    (!tail->will_payload.length && tail->will_payload.offset)) return ASTER_EINVAL;
	void *password = tail->password.length ? network_buf(env, tail->password.offset, tail->password.length) : NULL;
	void *will = tail->will_payload.length ? network_buf(env, tail->will_payload.offset, tail->will_payload.length) : NULL;
	if ((tail->password.length && !password) || (tail->will_payload.length && !will)) return ASTER_EINVAL;
	struct aster_mqtt_connect o = { .host = host, .client_id = client,
		.username = tail->username.length ? user : NULL, .password = password,
		.password_length = tail->password.length,
		.will_topic = tail->will_topic.length ? topic : NULL,
		.will_payload = will, .will_length = tail->will_payload.length,
		.port = hdr->port, .timeout_ms = hdr->timeout_ms, .keepalive_s = tail->keepalive_s,
		.will_qos = tail->will_qos, .tls = !!(hdr->flags & ASTER_MQTT_FLAG_TLS),
		.reconnect = !!(hdr->flags & ASTER_MQTT_FLAG_RECONNECT),
		.will_retain = !!(hdr->flags & ASTER_MQTT_FLAG_WILL_RETAIN) };
	return mqtt_error(aster_mqtt_svc_open(owner, &o), NULL);
}
#endif
static int32_t n_net_info(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
	return n_network_get_info(env, off, size);
}
static int32_t n_net_open(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
	uint64_t owner; int32_t rc = network_gate(env, &owner);
	if (rc) return rc;
	aster_net_open_hdr_t hdr;
	if (!net_take(env, off, size, &hdr, sizeof(hdr)) || hdr.size != size ||
	    hdr.version != ASTER_NET_ABI_VERSION || hdr.reserved0 || hdr.reserved1)
		return ASTER_EINVAL;
	uint32_t expect = sizeof(hdr);
	if (hdr.proto == ASTER_NET_PROTO_HTTP) expect += sizeof(aster_net_http_open_t);
	else if (hdr.proto == ASTER_NET_PROTO_MQTT) expect += sizeof(aster_net_mqtt_open_t);
	else if (hdr.proto != ASTER_NET_PROTO_TCP && hdr.proto != ASTER_NET_PROTO_UDP &&
		 hdr.proto != ASTER_NET_PROTO_WS) return ASTER_ENOTSUP;
	if (size != expect) return ASTER_EINVAL;
	int32_t internal = ASTER_ENOTSUP;
	if (hdr.proto == ASTER_NET_PROTO_TCP || hdr.proto == ASTER_NET_PROTO_UDP) {
		if (hdr.flags) return ASTER_EINVAL;
		internal = n_network_open(env, hdr.proto, hdr.target.offset, hdr.target.length, hdr.port, hdr.timeout_ms);
	}
#if defined(CONFIG_ASTER_WEBSOCKET)
	else if (hdr.proto == ASTER_NET_PROTO_WS) {
		if (hdr.flags || hdr.port) return ASTER_EINVAL;
		internal = n_ws_connect(env, hdr.target.offset, hdr.target.length, hdr.timeout_ms);
	}
#endif
#if defined(CONFIG_ASTER_HTTP)
	else if (hdr.proto == ASTER_NET_PROTO_HTTP) {
		aster_net_http_open_t tail;
		if (!net_take(env, off + sizeof(hdr), sizeof(tail), &tail, sizeof(tail))) return ASTER_EINVAL;
		if (hdr.flags || hdr.port) return ASTER_EINVAL;
		internal = n_http_request(env, tail.method, hdr.target.offset, hdr.target.length,
			tail.headers.offset, tail.headers.length, tail.body.offset, tail.body.length,
			hdr.timeout_ms, tail.body_limit);
	}
#endif
#if defined(CONFIG_ASTER_MQTT)
	else if (hdr.proto == ASTER_NET_PROTO_MQTT) {
		aster_net_mqtt_open_t tail;
		if (!net_take(env, off + sizeof(hdr), sizeof(tail), &tail, sizeof(tail))) return ASTER_EINVAL;
		internal = net_mqtt_open(env, owner, &hdr, &tail);
	}
#endif
	else return ASTER_ENOTSUP;
	if (internal <= 0) return internal;
	int32_t pub = net_pub_add(owner, hdr.proto, internal);
	if (pub < 0) net_abandon(env, hdr.proto, (uint32_t)internal);
	return pub;
}
static int32_t net_resolve(wasm_exec_env_t env, uint32_t handle, uint32_t *proto, uint32_t *internal)
{
	uint64_t owner; int32_t rc = network_gate(env, &owner);
	if (rc) return rc;
	if (!net_pub_get(owner, handle, proto, internal)) return ASTER_EBADF;
	return ASTER_OK;
}
static uint32_t net_status_bytes(uint32_t proto)
{
	if (proto == ASTER_NET_PROTO_TCP || proto == ASTER_NET_PROTO_UDP)
		return sizeof(aster_net_status_hdr_t) + sizeof(aster_net_tcp_status_t);
	if (proto == ASTER_NET_PROTO_HTTP)
		return sizeof(aster_net_status_hdr_t) + sizeof(aster_net_http_status_t);
	if (proto == ASTER_NET_PROTO_MQTT)
		return sizeof(aster_net_status_hdr_t) + sizeof(aster_net_mqtt_status_t);
	if (proto == ASTER_NET_PROTO_WS)
		return sizeof(aster_net_status_hdr_t) + sizeof(aster_net_ws_status_t);
	return 0;
}
static int32_t n_net_status(wasm_exec_env_t env, uint32_t handle, uint32_t off, uint32_t size)
{
	uint64_t owner; int32_t rc = network_gate(env, &owner);
	if (rc) return rc;
	uint32_t proto, internal;
	if (!net_pub_get(owner, handle, &proto, &internal)) return ASTER_EBADF;
	uint32_t need = net_status_bytes(proto);
	uint8_t *p = need && size >= need ? network_buf(env, off, need) : NULL;
	uint8_t raw[sizeof(aster_net_status_hdr_t) + 32];
	if (!p || need > sizeof(raw)) return ASTER_EINVAL;
	memset(raw, 0, need);
	aster_net_status_hdr_t *h = (aster_net_status_hdr_t *)raw;
	h->size = need; h->version = ASTER_NET_ABI_VERSION; h->proto = proto;
	if (proto == ASTER_NET_PROTO_TCP || proto == ASTER_NET_PROTO_UDP) {
		struct aster_net_snapshot s;
		rc = aster_net_snapshot(owner, internal, &s);
		if (rc) return network_error(rc);
		h->proto_state = s.state; h->error = network_error(s.error);
		h->tx_queued = s.tx_queued; h->rx_queued = s.rx_queued;
		h->state_class = net_class_span(s.state, ASTER_NETWORK_READY, 0, ASTER_NETWORK_FAILED);
		aster_net_tcp_status_t *t = (aster_net_tcp_status_t *)(raw + sizeof(*h));
		t->kind = s.kind; t->phase = s.phase; t->dropped = s.dropped; t->local_port = s.local_port;
		t->tx_bytes = s.tx_bytes; t->rx_bytes = s.rx_bytes;
		memcpy(p, raw, need); return ASTER_OK;
	}
#if defined(CONFIG_ASTER_HTTP)
	if (proto == ASTER_NET_PROTO_HTTP) {
		struct aster_http_snapshot s;
		rc = aster_http_svc_status(owner, internal, &s);
		if (rc) return http_error(rc, NULL);
		h->proto_state = s.state; h->error = http_error(s.error, &s);
		h->rx_queued = s.queued; h->certificate_flags = http_certificate_flags(s.tls_verify);
		h->state_class = net_class_span(s.state, ASTER_HTTP_STATE_HEADERS, 0, ASTER_HTTP_STATE_DONE);
		aster_net_http_status_t *t = (aster_net_http_status_t *)(raw + sizeof(*h));
		t->http_status = s.status; t->redirects = s.redirects;
		t->flags = (s.headers_ready ? ASTER_HTTP_FLAG_HEADERS : 0) |
			(s.has_length ? ASTER_HTTP_FLAG_LENGTH : 0) | (s.tls ? ASTER_HTTP_FLAG_TLS : 0);
		t->received = s.received; t->content_length = s.content_length;
		memcpy(p, raw, need); return ASTER_OK;
	}
#endif
#if defined(CONFIG_ASTER_MQTT)
	if (proto == ASTER_NET_PROTO_MQTT) {
		struct aster_mqtt_snapshot s;
		rc = aster_mqtt_svc_status(owner, internal, &s);
		if (rc) return mqtt_error(rc, NULL);
		h->proto_state = s.state; h->error = mqtt_error(s.error, &s);
		h->tx_queued = s.queued; h->rx_queued = s.pending;
		h->certificate_flags = http_certificate_flags(s.tls_verify);
		h->state_class = s.state == ASTER_MQTT_STATE_READY ? ASTER_NET_CLASS_READY :
			s.state >= ASTER_MQTT_STATE_FAILED ? ASTER_NET_CLASS_TERMINAL : ASTER_NET_CLASS_CONNECTING;
		aster_net_mqtt_status_t *t = (aster_net_mqtt_status_t *)(raw + sizeof(*h));
		t->reconnects = s.reconnects; t->subscriptions = s.subscriptions;
		t->received = s.received; t->sent = s.sent; t->connack = s.connack;
		t->last_operation = s.last_operation; t->last_operation_kind = s.last_operation_kind;
		t->operation_error = mqtt_error(s.operation_error, NULL);
		memcpy(p, raw, need); return ASTER_OK;
	}
#endif
#if defined(CONFIG_ASTER_WEBSOCKET)
	if (proto == ASTER_NET_PROTO_WS) {
		struct aster_ws_snapshot s;
		rc = aster_ws_svc_status(owner, internal, &s);
		if (rc) return ws_error(rc, NULL);
		h->proto_state = s.state; h->error = ws_error(s.error, &s);
		h->tx_queued = s.pending; h->rx_queued = s.queued;
		h->certificate_flags = http_certificate_flags(s.tls_verify);
		h->state_class = net_class_span(s.state, ASTER_WS_STATE_READY, ASTER_WS_STATE_CLOSING, ASTER_WS_STATE_CLOSED);
		aster_net_ws_status_t *t = (aster_net_ws_status_t *)(raw + sizeof(*h));
		t->received = s.received; t->sent = s.sent; t->close_code = s.close_code;
		memcpy(p, raw, need); return ASTER_OK;
	}
#endif
	return ASTER_ENOTSUP;
}
static int32_t n_net_io(wasm_exec_env_t env, uint32_t handle, uint32_t op, uint32_t off, uint32_t size)
{
	uint32_t proto, internal; int32_t rc = net_resolve(env, handle, &proto, &internal);
	if (rc) return rc;
	aster_net_io_t io;
	if (!net_take(env, off, size, &io, sizeof(io)) || io.size < sizeof(io) ||
	    io.version != ASTER_NET_ABI_VERSION) return ASTER_EINVAL;
	uint8_t *back = network_buf(env, off, sizeof(io));
	if (!back) return ASTER_EINVAL;
	if (proto == ASTER_NET_PROTO_TCP || proto == ASTER_NET_PROTO_UDP) {
		if (op == ASTER_NET_OP_SEND)
			return n_network_send(env, internal, io.data.offset, io.data.length, io.name.offset, io.name.length, io.port);
		if (op != ASTER_NET_OP_RECV) return ASTER_ENOTSUP;
		uint64_t owner; rc = network_gate(env, &owner); if (rc) return rc;
		if (io.data.length > ASTER_NETWORK_IO_MAX) return ASTER_EINVAL;
		uint8_t *data = io.data.length ? network_buf(env, io.data.offset, io.data.length) : NULL;
		if (io.data.length && !data) return ASTER_EINVAL;
		if (io.data.length && (uint64_t)io.data.offset < (uint64_t)off + sizeof(io) &&
		    (uint64_t)off < (uint64_t)io.data.offset + io.data.length) return ASTER_EINVAL;
		size_t required = 0; uint32_t ip = 0; uint16_t port = 0;
		rc = aster_net_recv(owner, internal, data, io.data.length, &required, &ip, &port);
		io.required = (uint32_t)required; io.port = port;
		if (rc >= 0 && port) zsock_inet_ntop(AF_INET, &ip, io.ipv4, sizeof(io.ipv4));
		memcpy(back, &io, sizeof(io));
		return network_error(rc);
	}
#if defined(CONFIG_ASTER_HTTP)
	if (proto == ASTER_NET_PROTO_HTTP) {
		if (op == ASTER_NET_OP_RECV) {
			rc = n_http_read(env, internal, io.data.offset, io.data.length);
			if (rc >= 0) { io.required = (uint32_t)rc; memcpy(back, &io, sizeof(io)); }
			return rc;
		}
		if (op != ASTER_NET_OP_HEADER) return ASTER_ENOTSUP;
		return n_http_header(env, internal, io.name.offset, io.name.length, io.aux,
			io.data.offset, io.data.length, off + offsetof(aster_net_io_t, required));
	}
#endif
#if defined(CONFIG_ASTER_MQTT)
	if (proto == ASTER_NET_PROTO_MQTT) {
		uint32_t qos = io.flags & 3u, retain = (io.flags >> 2) & 1u;
		if (op == ASTER_NET_OP_SEND)
			return n_mqtt_publish(env, internal, io.name.offset, io.name.length, io.data.offset, io.data.length, qos, retain);
		if (op == ASTER_NET_OP_SUBSCRIBE) return n_mqtt_subscribe(env, internal, io.name.offset, io.name.length, qos);
		if (op == ASTER_NET_OP_UNSUBSCRIBE) return n_mqtt_unsubscribe(env, internal, io.name.offset, io.name.length);
		if (op == ASTER_NET_OP_OPERATION)
			return n_mqtt_operation(env, internal, io.aux, off + offsetof(aster_net_io_t, required));
		if (op != ASTER_NET_OP_RECV) return ASTER_ENOTSUP;
		uint64_t owner; rc = network_gate(env, &owner); if (rc) return rc;
		struct aster_mqtt_message m;
		rc = aster_mqtt_svc_recv(owner, internal, &m);
		if (rc) {
			if (rc == -EAGAIN) return ASTER_EAGAIN;
			struct aster_mqtt_snapshot s; int sr = aster_mqtt_svc_status(owner, internal, &s);
			if (sr == -EAGAIN) return ASTER_EAGAIN;
			return mqtt_error(rc, sr ? NULL : &s);
		}
		uint8_t *topic = io.name.length ? network_buf(env, io.name.offset, io.name.length) : NULL;
		uint8_t *data = io.data.length ? network_buf(env, io.data.offset, io.data.length) : NULL;
		uint32_t tlen = (uint32_t)strlen(m.topic);
		if (!topic || io.name.length < tlen || (m.length && (!data || io.data.length < m.length))) {
			io.required = m.length; memcpy(back, &io, sizeof(io)); return ASTER_EMSGSIZE;
		}
		memcpy(topic, m.topic, tlen); if (m.length) memcpy(data, m.data, m.length);
		io.required = m.length; io.port = tlen; io.flags = m.qos |
			(m.retain ? 4u : 0) | (m.dup ? 8u : 0);
		memcpy(back, &io, sizeof(io)); return ASTER_OK;
	}
#endif
#if defined(CONFIG_ASTER_WEBSOCKET)
	if (proto == ASTER_NET_PROTO_WS) {
		if (op == ASTER_NET_OP_SEND)
			return n_ws_send(env, internal, (io.flags >> 8) & 0xffu, io.data.offset, io.data.length);
		if (op != ASTER_NET_OP_RECV) return ASTER_ENOTSUP;
		uint64_t owner; rc = network_gate(env, &owner); if (rc) return rc;
		uint8_t *data = io.data.length ? network_buf(env, io.data.offset, io.data.length) : NULL;
		if (io.data.length && !data) return ASTER_EINVAL;
		size_t n = 0; unsigned opcode = 0;
		rc = aster_ws_svc_recv(owner, internal, data, io.data.length, &n, &opcode);
		if (rc) {
			if (rc == -EAGAIN) return ASTER_EAGAIN;
			struct aster_ws_snapshot s; int sr = aster_ws_svc_status(owner, internal, &s);
			return ws_error(rc, sr ? NULL : &s);
		}
		io.required = (uint32_t)n; io.flags = (io.flags & 0xffu) | (opcode << 8);
		memcpy(back, &io, sizeof(io)); return ASTER_OK;
	}
#endif
	return ASTER_ENOTSUP;
}
static int32_t n_net_cancel(wasm_exec_env_t env, uint32_t handle)
{
	uint32_t proto, internal; int32_t rc = net_resolve(env, handle, &proto, &internal);
	if (rc) return rc;
	if (proto == ASTER_NET_PROTO_TCP || proto == ASTER_NET_PROTO_UDP) return n_network_cancel(env, internal);
#if defined(CONFIG_ASTER_HTTP)
	if (proto == ASTER_NET_PROTO_HTTP) return n_http_cancel(env, internal);
#endif
#if defined(CONFIG_ASTER_MQTT)
	if (proto == ASTER_NET_PROTO_MQTT) return n_mqtt_cancel(env, internal);
#endif
#if defined(CONFIG_ASTER_WEBSOCKET)
	if (proto == ASTER_NET_PROTO_WS) return n_ws_cancel(env, internal);
#endif
	return ASTER_ENOTSUP;
}
static int32_t n_net_close(wasm_exec_env_t env, uint32_t handle)
{
	uint32_t proto, internal; int32_t rc = net_resolve(env, handle, &proto, &internal);
	if (rc) return rc;
	rc = net_abandon(env, proto, internal);
	if (!rc) net_pub_del(handle);
	return rc;
}
#else
#define NET4_STUB(name, args) static int32_t name args { return ASTER_ENOTSUP; }
NET4_STUB(n_net_info, (wasm_exec_env_t e, uint32_t o, uint32_t n))
NET4_STUB(n_net_open, (wasm_exec_env_t e, uint32_t o, uint32_t n))
NET4_STUB(n_net_io, (wasm_exec_env_t e, uint32_t h, uint32_t op, uint32_t o, uint32_t n))
NET4_STUB(n_net_status, (wasm_exec_env_t e, uint32_t h, uint32_t o, uint32_t n))
NET4_STUB(n_net_cancel, (wasm_exec_env_t e, uint32_t h))
NET4_STUB(n_net_close, (wasm_exec_env_t e, uint32_t h))
#undef NET4_STUB
#endif
