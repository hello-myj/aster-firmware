	target_sources(app PRIVATE memory.c)
	if(CONFIG_ASTER_PANEL_AXS15231B)
		target_sources(app PRIVATE display_axs15231b.c)
	endif()
	if(CONFIG_ASTER_AXS15231B_TOUCH)
		target_sources(app PRIVATE input_axs15231b.c)
	endif()
	if(CONFIG_ASTER_ESP32S3_TOUCH)
		target_sources(app PRIVATE input_ft6336.c)
	endif()

	target_sources(app PRIVATE
		display.c
		input.c
		touch_state.c
		touch_ui.c
		touch_protocol.c
		sensors.c
		hw_cmd.c
	)
	if(CONFIG_ASTER_CAMERA)
		target_sources(app PRIVATE camera.c)
	endif()
	if(CONFIG_ASTER_AUDIO)
		target_sources(app PRIVATE
			audio.c
			audio_lyrics.c
			minimp3_impl.c
		)
		target_include_directories(app PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/../../modules/minimp3)
		# minimp3 is verbatim upstream (CC0): its float-vs-double literals
		# stay as they are, we just don't police them. Same policy as the
		# vendored decoders in modules/imgdec.
		set_property(SOURCE minimp3_impl.c TARGET_DIRECTORY app
			PROPERTY COMPILE_OPTIONS "-Wno-double-promotion")
	endif()
	if(CONFIG_ASTER_WIFI)
		target_sources(app PRIVATE wifi.c)
	endif()
	# Port-private headers may live alongside the sources.
	target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
