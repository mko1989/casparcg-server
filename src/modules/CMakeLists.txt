cmake_minimum_required (VERSION 3.16)
project("modules")

add_subdirectory(image)
add_subdirectory(ffmpeg)
add_subdirectory(oal)
add_subdirectory(portaudio)
add_subdirectory(decklink)
add_subdirectory(screen)
add_subdirectory(newtek)
add_subdirectory(artnet)
add_subdirectory(pixel)
add_subdirectory(shm)

if (ENABLE_HTML)
	add_subdirectory(html)
endif ()

if (MSVC)
	add_subdirectory(flash)
	add_subdirectory(bluefish)
endif()

