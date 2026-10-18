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

if(CONFIG_ASTER_HTTP)
  target_sources(app PRIVATE aster_http_cmd.c)
endif()

if(CONFIG_ASTER_MQTT)
  target_sources(app PRIVATE aster_mqtt_cmd.c)
endif()

if(CONFIG_ASTER_WEBSOCKET)
  target_sources(app PRIVATE aster_ws_cmd.c)
endif()

if(CONFIG_ASTER_MDNS)
  target_sources(app PRIVATE aster_mdns_cmd.c)
endif()
