include_guard(GLOBAL)

set(STRUCTMAPPER_CMAKE_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")

# structmapper_generate_schema(
#     TARGET        <name>
#     CLASS_TYPE    <Fully::Qualified::Type>
#     HEADER        <path/to/type.hpp>
#     [OUTPUT       <path/to/out.json>]
#     [LIKE_TARGET  <existing_target>]   # reuse this target's include dirs /
#                                        # link libs / compile features
#                                        # instead of re-specifying them.
#                                        # Must already have its own
#                                        # target_link_libraries()/
#                                        # target_include_directories() calls
#                                        # made *before* this call.
#     [INCLUDE_DIRS <dir> ...]          # extra, on top of LIKE_TARGET if given
#     [LINK_LIBRARIES <lib> ...]        # extra, on top of LIKE_TARGET if given
# )
function(structmapper_generate_schema)
    set(oneValueArgs TARGET CLASS_TYPE HEADER OUTPUT LIKE_TARGET)
    set(multiValueArgs INCLUDE_DIRS LINK_LIBRARIES)
    cmake_parse_arguments(SMS "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SMS_TARGET)
        message(FATAL_ERROR "structmapper_generate_schema: TARGET is required")
    endif()
    if(NOT SMS_CLASS_TYPE)
        message(FATAL_ERROR "structmapper_generate_schema: CLASS_TYPE is required")
    endif()
    if(NOT SMS_HEADER)
        message(FATAL_ERROR "structmapper_generate_schema: HEADER is required")
    endif()
    if(NOT EXISTS "${SMS_HEADER}")
        message(FATAL_ERROR "structmapper_generate_schema: HEADER '${SMS_HEADER}' does not exist")
    endif()
    if(NOT SMS_OUTPUT)
        set(SMS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${SMS_TARGET}.json")
    endif()
    if(SMS_LIKE_TARGET AND NOT TARGET ${SMS_LIKE_TARGET})
        message(FATAL_ERROR
            "structmapper_generate_schema: LIKE_TARGET '${SMS_LIKE_TARGET}' is not "
            "a target. Call structmapper_generate_schema() AFTER that target's own "
            "add_executable()/target_link_libraries()/target_include_directories() "
            "calls, so its properties are populated.")
    endif()

    if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
        message(WARNING
            "structmapper_generate_schema(${SMS_TARGET}): cross-compiling without "
            "CMAKE_CROSSCOMPILING_EMULATOR set; the generator exe won't run on the "
            "host at build time.")
    endif()

    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/structmapper_generated")
    file(MAKE_DIRECTORY "${_gen_dir}")
    set(_gen_src "${_gen_dir}/${SMS_TARGET}_gen.cpp")

    set(STRUCTMAPPER_GEN_HEADER "${SMS_HEADER}")
    set(STRUCTMAPPER_GEN_CLASS_TYPE "${SMS_CLASS_TYPE}")
    configure_file(
        "${STRUCTMAPPER_CMAKE_MODULE_DIR}/templates/schema_gen.cpp.in"
        "${_gen_src}"
        @ONLY
    )

    set(_gen_exe "${SMS_TARGET}_gen_exe")
    add_executable(${_gen_exe} "${_gen_src}")
    set_target_properties(${_gen_exe} PROPERTIES EXCLUDE_FROM_ALL TRUE)

    if(SMS_LIKE_TARGET)
        # Reuse the existing target's build settings wholesale via
        # generator-expression property reads -- evaluated at build time,
        # so this works regardless of whether LIKE_TARGET's own libs are
        # imported CMake targets or bare -l flags/paths.
        target_include_directories(${_gen_exe} PRIVATE
            $<TARGET_PROPERTY:${SMS_LIKE_TARGET},INCLUDE_DIRECTORIES>)
        target_link_libraries(${_gen_exe} PRIVATE
            $<TARGET_PROPERTY:${SMS_LIKE_TARGET},LINK_LIBRARIES>)
        target_compile_definitions(${_gen_exe} PRIVATE
            $<TARGET_PROPERTY:${SMS_LIKE_TARGET},COMPILE_DEFINITIONS>)
        target_compile_features(${_gen_exe} PRIVATE
            $<TARGET_PROPERTY:${SMS_LIKE_TARGET},COMPILE_FEATURES>)
    else()
        target_include_directories(${_gen_exe} PRIVATE
            ${catkin_INCLUDE_DIRS} ${structmapper_INCLUDE_DIRS})
    endif()

    # Always allowed on top, LIKE_TARGET or not.
    if(SMS_INCLUDE_DIRS)
        target_include_directories(${_gen_exe} PRIVATE ${SMS_INCLUDE_DIRS})
    endif()
    if(SMS_LIKE_TARGET)
        target_link_libraries(${_gen_exe} PRIVATE ${SMS_LINK_LIBRARIES})
    endif()

    set(_run_cmd ${_gen_exe})
    if(CMAKE_CROSSCOMPILING_EMULATOR)
        set(_run_cmd ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:${_gen_exe}>)
    endif()

    get_filename_component(_out_dir "${SMS_OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_out_dir}")

    add_custom_command(
        OUTPUT "${SMS_OUTPUT}"
        COMMAND ${_run_cmd} "${SMS_OUTPUT}"
        DEPENDS ${_gen_exe} "${SMS_HEADER}"
        COMMENT "structmapper: generating schema for ${SMS_CLASS_TYPE} -> ${SMS_OUTPUT}"
        VERBATIM
    )

    add_custom_target(${SMS_TARGET} ALL DEPENDS "${SMS_OUTPUT}")
    set(${SMS_TARGET}_SCHEMA_OUTPUT "${SMS_OUTPUT}" PARENT_SCOPE)
endfunction()
