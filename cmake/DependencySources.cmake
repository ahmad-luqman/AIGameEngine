# Lets a configure take dependency sources fetched earlier instead of downloading them (CI restores them from its
# cache). BRECCIA_DEPS_SOURCE_DIR names a directory that scripts/ci-fetch-deps.sh filled with the pinned sources as
# <name>-src directories. It is an environment variable rather than a cache option so that the configures inside
# scripts/coverage.sh and scripts/fuzz.sh see it too.

# Points FetchContent at each <name>-src in BRECCIA_DEPS_SOURCE_DIR, but only while the directory's stamp (pins.sha256)
# equals the hash of pinsFile: any edit to that file, comments included, invalidates it. FetchContent caches the chosen
# paths, so those an earlier call chose (BRECCIA_DEPS_SOURCE_PATHS) are cleared first, and a configure without a valid
# directory downloads as usual. FETCHCONTENT_SOURCE_DIR_<NAME> entries set by hand, before or after, are left alone.
function(breccia_use_dependency_sources pinsFile)
	# An entry that no longer holds the path this function chose was set by hand since.
	foreach(entry IN LISTS BRECCIA_DEPS_SOURCE_PATHS)
		string(FIND "${entry}" "=" split)
		string(SUBSTRING "${entry}" 0 ${split} name)
		math(EXPR split "${split} + 1")
		string(SUBSTRING "${entry}" ${split} -1 path)
		if("$CACHE{FETCHCONTENT_SOURCE_DIR_${name}}" STREQUAL path)
			unset(FETCHCONTENT_SOURCE_DIR_${name} CACHE)
		endif()
	endforeach()
	set(BRECCIA_DEPS_SOURCE_PATHS "" CACHE INTERNAL "NAME=path of each source taken from BRECCIA_DEPS_SOURCE_DIR")
	if(NOT DEFINED ENV{BRECCIA_DEPS_SOURCE_DIR})
		return()
	endif()

	file(TO_CMAKE_PATH "$ENV{BRECCIA_DEPS_SOURCE_DIR}" sourceDir)
	file(SHA256 "${pinsFile}" pins)
	set(stamp "")
	if(EXISTS "${sourceDir}/pins.sha256")
		file(READ "${sourceDir}/pins.sha256" stamp)
		string(STRIP "${stamp}" stamp)
	endif()
	if(NOT stamp STREQUAL pins)
		# A missing directory means nothing was fetched there yet (CI jobs fetch on a cache miss, through
		# .github/actions/setup-breccia); a directory with another stamp, or none, holds sources for other pins.
		if(EXISTS "${sourceDir}")
			message(WARNING "Breccia: ignoring BRECCIA_DEPS_SOURCE_DIR (${sourceDir}): its sources were not fetched for "
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
		message(WARNING "Breccia: BRECCIA_DEPS_SOURCE_DIR (${sourceDir}) holds no dependency sources")
	else()
		message(STATUS "Breccia: ${count} dependency sources from ${sourceDir}")
	endif()
	set(BRECCIA_DEPS_SOURCE_PATHS "${chosen}" CACHE INTERNAL "NAME=path of each source taken from BRECCIA_DEPS_SOURCE_DIR")
endfunction()
