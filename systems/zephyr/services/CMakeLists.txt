# SPDX-License-Identifier: MIT

target_sources(app PRIVATE
	aster_sysinfo.c
	aster_storage.c
	aster_versions.c
	aster_pkg.c
	aster_app_mgr.c
	aster_kv.c
	aster_logring.c
	aster_file_svc.c
	aster_sig.c
	aster_time.c
)

if(CONFIG_ASTER_NET)
  target_sources(app PRIVATE aster_net.c)
endif()

if(CONFIG_ASTER_NET_LOOPBACK_TEST)
  target_sources(app PRIVATE ${ASTER_ROOT}/tests/zephyr/net_loopback_fixture.c)
endif()
