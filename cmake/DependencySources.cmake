# Lets a configure take dependency sources fetched earlier instead of downloading them (CI restores them from its
# cache). BASALT_DEPS_SOURCE_DIR names a directory that scripts/ci-fetch-deps.sh filled with the pinned sources as
# <name>-src directories. It is an environment variable rather than a cache option so that the configures inside
# scripts/coverage.sh and scripts/fuzz.sh see it too.

# Points FetchContent at each <name>-src in BASALT_DEPS_SOURCE_DIR, but only while the directory's stamp (pins.sha256)
# equals the hash of pinsFile: any edit to that file, comments included, invalidates it. FetchContent caches the chosen
# paths, so those an earlier call chose (BASALT_DEPS_SOURCE_PATHS) are cleared first, and a configure without a valid
# directory downloads as usual. FETCHCONTENT_SOURCE_DIR_<NAME> entries set by hand, before or after, are left alone.
function(basalt_use_dependency_sources pinsFile)
	# An entry that no longer holds the path this function chose was set by hand since.
	foreach(entry IN LISTS BASALT_DEPS_SOURCE_PATHS)
		string(FIND "${entry}" "=" split)
		string(SUBSTRING "${entry}" 0 ${split} name)
		math(EXPR split "${split} + 1")
		string(SUBSTRING "${entry}" ${split} -1 path)
		if("$CACHE{FETCHCONTENT_SOURCE_DIR_${name}}" STREQUAL path)
			unset(FETCHCONTENT_SOURCE_DIR_${name} CACHE)
		endif()
	endforeach()
	set(BASALT_DEPS_SOURCE_PATHS "" CACHE INTERNAL "NAME=path of each source taken from BASALT_DEPS_SOURCE_DIR")
	if(NOT DEFINED ENV{BASALT_DEPS_SOURCE_DIR})
		return()
	endif()

	file(TO_CMAKE_PATH "$ENV{BASALT_DEPS_SOURCE_DIR}" sourceDir)
	file(SHA256 "${pinsFile}" pins)
	set(stamp "")
	if(EXISTS "${sourceDir}/pins.sha256")
		file(READ "${sourceDir}/pins.sha256" stamp)
		string(STRIP "${stamp}" stamp)
	endif()
	if(NOT stamp STREQUAL pins)
		# A missing directory is a cache miss in a workflow that does not fetch (the nightly fuzz run) or a local tree
		# without one; a directory with another stamp, or none, holds sources for other pins.
		if(EXISTS "${sourceDir}")
			message(WARNING "Basalt: ignoring BASALT_DEPS_SOURCE_DIR (${sourceDir}): its sources were not fetched for "
				"these pins (scripts/ci-fetch-deps.sh)")
		endif()
		return()
	endif()

	set(chosen "")
	file(GLOB sources LIST_DIRECTORIES true "${sourceDir}/*-src")
	foreach(source IN LISTS sources)
		get_filename_component(name "${source}" NAME)
		string(REGEX REPLACE "-src$" "" name "${name}")
		string(TOUPPER "${name}" name)
		if(IS_DIRECTORY "${source}" AND NOT FETCHCONTENT_SOURCE_DIR_${name})
			set(FETCHCONTENT_SOURCE_DIR_${name} "${source}" PARENT_SCOPE)
			list(APPEND chosen "${name}=${source}")
		endif()
	endforeach()
	list(LENGTH chosen count)
	if(count EQUAL 0)
		message(WARNING "Basalt: BASALT_DEPS_SOURCE_DIR (${sourceDir}) holds no dependency sources")
	else()
		message(STATUS "Basalt: ${count} dependency sources from ${sourceDir}")
	endif()
	set(BASALT_DEPS_SOURCE_PATHS "${chosen}" CACHE INTERNAL "NAME=path of each source taken from BASALT_DEPS_SOURCE_DIR")
endfunction()
