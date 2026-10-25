# Lets a configure use dependency sources fetched earlier instead of downloading them (CI restores them from its cache).
# BASALT_DEPS_SOURCE_DIR names a directory that scripts/ci-cache.sh filled with <name>-src copies of the pinned sources.
# It is an environment variable rather than a cache option so that the configures inside scripts/coverage.sh and
# scripts/fuzz.sh see it too.

# Points FetchContent at each <name>-src in BASALT_DEPS_SOURCE_DIR. The directory is used only while its stamp
# (pins.sha256) matches the hash of pinsFile, so it never supplies sources for other pins, and only for this configure:
# FetchContent caches the chosen paths, so the ones an earlier configure chose are cleared first.
function(basalt_use_dependency_sources pinsFile)
	foreach(name IN LISTS BASALT_DEPS_SOURCE_NAMES)
		unset(FETCHCONTENT_SOURCE_DIR_${name} CACHE)
	endforeach()

	set(names "")
	if(DEFINED ENV{BASALT_DEPS_SOURCE_DIR})
		file(TO_CMAKE_PATH "$ENV{BASALT_DEPS_SOURCE_DIR}" sourceDir)
		file(SHA256 "${pinsFile}" pins)
		set(stamp "")
		if(EXISTS "${sourceDir}/pins.sha256")
			file(READ "${sourceDir}/pins.sha256" stamp)
			string(STRIP "${stamp}" stamp)
		endif()

		if(stamp STREQUAL pins)
			file(GLOB sources LIST_DIRECTORIES true "${sourceDir}/*-src")
			foreach(source IN LISTS sources)
				if(IS_DIRECTORY "${source}")
					get_filename_component(name "${source}" NAME)
					string(REGEX REPLACE "-src$" "" name "${name}")
					string(TOUPPER "${name}" name)
					set(FETCHCONTENT_SOURCE_DIR_${name} "${source}" PARENT_SCOPE)
					list(APPEND names ${name})
				endif()
			endforeach()
			list(LENGTH names count)
			message(STATUS "Basalt: ${count} dependency sources from ${sourceDir}")
		elseif(EXISTS "${sourceDir}")
			# A missing directory is a CI cache miss; one with the wrong stamp holds sources for other pins.
			message(WARNING "Basalt: ignoring BASALT_DEPS_SOURCE_DIR (${sourceDir}): its sources were not fetched for "
				"these pins (scripts/ci-cache.sh fetch-deps)")
		endif()
	endif()
	set(BASALT_DEPS_SOURCE_NAMES "${names}" CACHE INTERNAL "Dependencies whose sources came from BASALT_DEPS_SOURCE_DIR")
endfunction()
