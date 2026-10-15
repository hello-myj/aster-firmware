# SPDX-License-Identifier: MIT
#
# System shell commands only. Board-specific `hw` debug commands now live with
# their platform port under firmware/platform/<port>/hw_cmd.c.

target_sources(app PRIVATE
	aster_cmd.c
)

if(CONFIG_ASTER_NET)
  target_sources(app PRIVATE aster_net_cmd.c)
endif()
