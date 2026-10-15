/* SPDX-License-Identifier: MIT */
#include "aster_http_tls.h"
#include "aster_crypto.h"
#include "aster_mem.h"
#include "aster_net.h"
#include "aster_time.h"
#include <errno.h>
#include <mbedtls/memory_buffer_alloc.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <string.h>
struct aster_http_tls {
    uint64_t owner;
    uint32_t handle;
    int io_error;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
};
static const char root_ca[] =
#include "aster_http_roots.inc"
    ;
const char *aster_http_tls_default_ca(size_t *length)
{
    *length = sizeof(root_ca);
    return root_ca;
}
static int bio_send(void *ctx, const unsigned char *data, size_t length)
{
    struct aster_http_tls *t = ctx;
    int rc = aster_net_send(t->owner, t->handle, data, length, NULL, 0);
    if (rc < 0 && rc != -EAGAIN)
        t->io_error = rc;
    return rc == -EAGAIN ? MBEDTLS_ERR_SSL_WANT_WRITE
           : rc < 0      ? MBEDTLS_ERR_SSL_INTERNAL_ERROR
                         : rc;
}
static int bio_recv(void *ctx, unsigned char *data, size_t length)
{
    struct aster_http_tls *t = ctx;
    size_t required;
    int rc = aster_net_recv(t->owner, t->handle, data, length, &required, NULL,
                            NULL);
    if (rc < 0 && rc != -EAGAIN)
        t->io_error = rc;
    return rc == -EAGAIN ? MBEDTLS_ERR_SSL_WANT_READ
           : rc < 0      ? MBEDTLS_ERR_SSL_INTERNAL_ERROR
                         : rc;
}
static int result(struct aster_http_tls *t, int rc,
                  struct aster_http_tls_report *report)
{
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
        return -EAGAIN;
    if (rc >= 0)
        return rc;
    /* Authenticated EOF is normal for an EOF-delimited HTTP response. */
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        return 0;
    report->error = rc;
    report->transport_error = t->io_error;
    report->verify = mbedtls_ssl_get_verify_result(&t->ssl);
    if (t->io_error)
        return t->io_error;
    return report->verify && report->verify != UINT32_MAX ? -EACCES : -EIO;
}
static void release(struct aster_http_tls *t)
{
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->config);
    mbedtls_x509_crt_free(&t->ca);
    aster_psram_free(t);
}
int aster_http_tls_new(struct aster_http_tls **out, uint64_t owner,
                       uint32_t handle, const char *host, const char *pem,
                       size_t pem_length, struct aster_http_tls_report *report)
{
    aster_time_info_t now;
    int rc = aster_time_read(&now);
    if (rc || !(now.flags & ASTER_TIME_FLAG_SYNCED))
        return -ENODATA;
    if (k_mutex_lock(&aster_crypto_lock, K_NO_WAIT))
        return -EAGAIN;
    struct aster_http_tls *t = aster_psram_alloc(sizeof(*t));
    if (!t) {
        k_mutex_unlock(&aster_crypto_lock);
        return -ENOMEM;
    }
    memset(t, 0, sizeof(*t));
    t->owner = owner;
    t->handle = handle;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->config);
    mbedtls_x509_crt_init(&t->ca);
    if (psa_crypto_init() != PSA_SUCCESS) {
        rc = -EIO;
        goto done;
    }
    rc = mbedtls_x509_crt_parse(&t->ca, (const unsigned char *)pem, pem_length);
    /* A positive result means some roots failed: reject partial acceptance. */
    if (rc) {
        report->error = rc;
        rc = -EINVAL;
        goto done;
    }
    rc = mbedtls_ssl_config_defaults(&t->config, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc) {
        report->error = rc;
        rc = -EIO;
        goto done;
    }
    mbedtls_ssl_conf_authmode(&t->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t->config, &t->ca, NULL);
    rc = mbedtls_ssl_setup(&t->ssl, &t->config);
    if (!rc)
        rc = mbedtls_ssl_set_hostname(&t->ssl, host);
    if (rc) {
        report->error = rc;
        rc = -EIO;
        goto done;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
    *out = t;
done:
    if (rc)
        release(t);
    k_mutex_unlock(&aster_crypto_lock);
    return rc;
}
int aster_http_tls_handshake(struct aster_http_tls *t,
                             struct aster_http_tls_report *report)
{
    if (k_mutex_lock(&aster_crypto_lock, K_NO_WAIT))
        return -EAGAIN;
    int raw = mbedtls_ssl_handshake_step(&t->ssl);
    int rc = result(t, raw, report);
    if (!raw && !mbedtls_ssl_is_handshake_over(&t->ssl)) rc = -EAGAIN;
    if (raw == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        report->error = raw;
        rc = -EPROTO;
    }
    k_mutex_unlock(&aster_crypto_lock);
    return rc;
}
int aster_http_tls_send(struct aster_http_tls *t, const void *data,
                        size_t length, struct aster_http_tls_report *report)
{
    if (k_mutex_lock(&aster_crypto_lock, K_NO_WAIT))
        return -EAGAIN;
    int raw = mbedtls_ssl_write(&t->ssl, data, length);
    int rc = result(t, raw, report);
    if (!rc)
        rc = -EPIPE;
    k_mutex_unlock(&aster_crypto_lock);
    return rc;
}
int aster_http_tls_recv(struct aster_http_tls *t, void *data, size_t capacity,
                        struct aster_http_tls_report *report)
{
    if (k_mutex_lock(&aster_crypto_lock, K_NO_WAIT))
        return -EAGAIN;
    int raw = mbedtls_ssl_read(&t->ssl, data, capacity);
    int rc = result(t, raw, report);
    if (!raw)
        rc = -EPROTO;
    k_mutex_unlock(&aster_crypto_lock);
    return rc;
}
int aster_http_tls_free(struct aster_http_tls **tls)
{
    if (!*tls)
        return 0;
    if (k_mutex_lock(&aster_crypto_lock, K_NO_WAIT))
        return -EAGAIN;
    release(*tls);
    *tls = NULL;
    k_mutex_unlock(&aster_crypto_lock);
    return 0;
}
size_t aster_http_tls_context_bytes(void)
{
    return sizeof(struct aster_http_tls);
}
void aster_http_tls_heap(size_t *current, size_t *peak)
{
    *current = *peak = 0;
#if defined(CONFIG_MBEDTLS_MEMORY_DEBUG)
    if (k_mutex_lock(&aster_crypto_lock, K_NO_WAIT))
        return;
    size_t blocks;
    mbedtls_memory_buffer_alloc_cur_get(current, &blocks);
    mbedtls_memory_buffer_alloc_max_get(peak, &blocks);
    k_mutex_unlock(&aster_crypto_lock);
#endif
}
