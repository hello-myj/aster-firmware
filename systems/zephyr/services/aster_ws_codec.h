/* SPDX-License-Identifier: MIT */
#ifndef ASTER_WS_CODEC_H
#define ASTER_WS_CODEC_H
#include "aster_http_codec.h"
#define ASTER_WS_MESSAGE_MAX 4096u
#define ASTER_WS_WIRE_MAX (ASTER_WS_MESSAGE_MAX + 10u)
#define ASTER_WS_HANDSHAKE_MAX 2048u
struct aster_ws_frame { uint8_t opcode; bool fin; const uint8_t *data; size_t length, wire_length; };
/* ws(s), IPv4/DNS, no userinfo/fragment. No extensions or subprotocols. */
int aster_ws_url_parse(const char *text, struct aster_http_url *out);
void aster_ws_key(const uint8_t random[16], char key[25], char accept[29]);
int aster_ws_request(const struct aster_http_url *url, const char key[25], char *out, size_t capacity);
/* Complete response header only, including CRLF CRLF. */
int aster_ws_response(const char *header, size_t length, const char accept[29]);
/* Server frames must be unmasked. 0=incomplete, 1=frame; negative errno.
 * Reject oversized lengths from the header without waiting for payload. */
int aster_ws_decode(const uint8_t *wire, size_t length, struct aster_ws_frame *out);
/* Each client frame uses a fresh caller-supplied cryptographic random mask. */
int aster_ws_encode(uint8_t *out, size_t capacity, unsigned opcode, bool fin,
                    const void *data, size_t length, const uint8_t mask[4]);
bool aster_ws_utf8(const void *data, size_t length);
int aster_ws_close_check(const void *data, size_t length);
#endif
