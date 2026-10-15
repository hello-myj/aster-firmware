/* SPDX-License-Identifier: MIT
 * Diagnostic only: public Zephyr context iteration; no socket or APP pointers
 * are retained. Maxima are observed samples, never claimed allocation peaks.
 */
#include <zephyr/kernel.h>
#include <zephyr/net/net_context.h>
#include <zephyr/shell/shell.h>
#include "aster_net.h"
#include <string.h>

struct pool_sample { uint32_t contexts, tcp, udp, ntp; };
struct pool_record {
    struct pool_sample current;
    uint32_t samples, peak, ntp_samples, overlap_samples, overlap_peak;
};
static struct pool_record record;
static struct k_spinlock record_lock;

static void count_context(struct net_context *context, void *data)
{
    struct pool_sample *s = data;
    ++s->contexts;
    uint16_t proto = net_context_get_proto(context);
    if (proto == NET_IPPROTO_TCP) ++s->tcp;
    if (proto == NET_IPPROTO_UDP) {
        ++s->udp;
        if (context->remote.sa_family == NET_AF_INET &&
            net_ntohs(net_sin(&context->remote)->sin_port) == 123) ++s->ntp;
    }
}
static void sample_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    for (;;) {
        struct pool_sample s = {0};
        struct aster_net_stats service;
        net_context_foreach(count_context, &s);
        /* Different locks: this is correlated sampling, not an atomic global
         * allocation trace. Dedicated test holds both APP sockets throughout. */
        bool overlap = s.ntp && aster_net_get_stats(&service) == 0 && service.active == 2;
        k_spinlock_key_t key = k_spin_lock(&record_lock);
        record.current = s;
        ++record.samples;
        if (s.contexts > record.peak) record.peak = s.contexts;
        if (s.ntp) ++record.ntp_samples;
        if (overlap) {
            ++record.overlap_samples;
            if (s.contexts > record.overlap_peak) record.overlap_peak = s.contexts;
        }
        k_spin_unlock(&record_lock, key);
        k_sleep(K_MSEC(5));
    }
}
K_THREAD_DEFINE(net_pool_probe, 2048, sample_thread, NULL, NULL, NULL, 9, 0, 1000);

static int cmd_pool(const struct shell *sh, size_t argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "reset")) return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&record_lock);
    if (argc == 2) memset(&record, 0, sizeof(record));
    struct pool_record s = record;
    k_spin_unlock(&record_lock, key);
    shell_print(sh, "NETPOOL samples=%u current=%u tcp=%u udp=%u observed_max=%u limit=%u nominal_ms=5 ntp_samples=%u app2_ntp_samples=%u app2_ntp_max=%u",
                s.samples, s.current.contexts, s.current.tcp, s.current.udp,
                s.peak, CONFIG_NET_MAX_CONTEXTS, s.ntp_samples,
                s.overlap_samples, s.overlap_peak);
    return 0;
}
SHELL_CMD_ARG_REGISTER(netpool, NULL, "Diagnostic sampled context maxima [reset]", cmd_pool, 1, 1);
