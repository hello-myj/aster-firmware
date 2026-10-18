/* SPDX-License-Identifier: MIT */
#include "aster_mdns_cmd.h"
#include "aster_mdns.h"
#include <stdlib.h>
#include <string.h>
#define OWNER ASTER_MDNS_OWNER_CLI
static int mdns_reply(const struct shell *sh, int rc)
{ shell_print(sh, "MDNS result=%d", rc); return 0; }
static int cmd_on(const struct shell *sh, size_t argc, char **argv)
{ (void)argc; (void)argv; return mdns_reply(sh, aster_mdns_svc_acquire(OWNER)); }
static int cmd_off(const struct shell *sh, size_t argc, char **argv)
{ (void)argc; (void)argv; return mdns_reply(sh, aster_mdns_svc_release(OWNER)); }
static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    struct aster_mdns_snapshot s;
    int rc = aster_mdns_svc_status(&s);
    if (rc) return mdns_reply(sh, rc);
    shell_print(sh, "MDNS state=%u want=%u name=%s ip=%s rx=%u tx=%u drops=%u announce=%u conflicts=%u renames=%u error=%d",
                s.state, s.want, s.name[0] ? s.name : "-", s.ipv4[0] ? s.ipv4 : "-",
                s.rx, s.tx, s.drops, s.announce, s.conflicts, s.renames, s.error);
    return 0;
}
static const struct shell_static_entry mdns_entries[] = {
    SHELL_CMD(on, NULL, "hold diagnostic publish (UI may also hold it)", cmd_on),
    SHELL_CMD(off, NULL, "withdraw diagnostic publish", cmd_off),
    SHELL_CMD(status, NULL, "bounded mDNS publisher state", cmd_status),
    SHELL_SUBCMD_SET_END
};
const union shell_cmd_entry aster_mdns_cmds = { .entry = mdns_entries };
