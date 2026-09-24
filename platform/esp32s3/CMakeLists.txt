target_sources(app PRIVATE memory.c)

	target_sources(app PRIVATE
		display.c
		input.c
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
