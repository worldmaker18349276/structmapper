include_guard(GLOBAL)

set(STRUCTMAPPER_CMAKE_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")

# structmapper_generate_schemas(
#     TARGET       <name>
#     TYPES        <FullyQualified::Type> ...
#     HEADERS      <path> ...
#     [OUTPUT      <path/to/output_dir>]
#     [LIKE_TARGET  <existing_target>]   # reuse this target's include dirs /
#                                        # link libs / compile features
#                                        # instead of re-specifying them.
#                                        # Must already have its own
#                                        # target_link_libraries()/
#                                        # target_include_directories() calls
#                                        # made *before* this call.
#     [INCLUDE_DIRS <dir> ...]           # extra, on top of LIKE_TARGET if given
#     [LINK_LIBRARIES <lib> ...]         # extra, on top of LIKE_TARGET if given
# )
function(structmapper_generate_schemas)
    set(oneValueArgs TARGET OUTPUT LIKE_TARGET)
    set(multiValueArgs TYPES HEADERS INCLUDE_DIRS LINK_LIBRARIES)
    cmake_parse_arguments(SMS "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SMS_TARGET)
        message(FATAL_ERROR "structmapper_generate_schemas: TARGET is required")
    endif()
    if(NOT SMS_TYPES)
        message(FATAL_ERROR "structmapper_generate_schemas: TYPES is required")
    endif()
    if(NOT SMS_HEADERS)
        message(FATAL_ERROR "structmapper_generate_schemas: HEADERS is required")
    endif()
    if(NOT SMS_OUTPUT)
        set(SMS_OUTPUT "${CATKIN_DEVEL_PREFIX}/${CATKIN_PACKAGE_SHARE_DESTINATION}/structmapper_schema")
    endif()
    foreach(_h IN LISTS SMS_HEADERS)
        if(NOT EXISTS "${_h}")
            message(FATAL_ERROR "structmapper_generate_schemas: header '${_h}' does not exist")
        endif()
    endforeach()
    if(SMS_LIKE_TARGET AND NOT TARGET ${SMS_LIKE_TARGET})
        message(FATAL_ERROR
            "structmapper_generate_schemas: LIKE_TARGET '${SMS_LIKE_TARGET}' is not a target. "
            "Call this after that target's own add_executable()/target_link_libraries() calls.")
    endif()
    if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
        message(WARNING
            "structmapper_generate_schemas(${SMS_TARGET}): cross-compiling without "
            "CMAKE_CROSSCOMPILING_EMULATOR set; the generator will not run at build time.")
    endif()

    set(_headers_dedup "${SMS_HEADERS}")
    list(REMOVE_DUPLICATES _headers_dedup)

    set(_includes "")
    foreach(_h IN LISTS _headers_dedup)
        string(APPEND _includes "#include \"${_h}\"\n")
    endforeach()

    # Comma-joined type list, passed straight through as template arguments
    # to structmapper::generate_schemas<Types...>().
    set(_types_list "")
    set(_first TRUE)
    foreach(_t IN LISTS SMS_TYPES)
        if(_first)
            set(_types_list "${_t}")
            set(_first FALSE)
        else()
            set(_types_list "${_types_list}, ${_t}")
        endif()
    endforeach()

    set(STRUCTMAPPER_GEN_INCLUDES "${_includes}")
    set(STRUCTMAPPER_GEN_TYPES "${_types_list}")

    set(_gen_src "${CMAKE_CURRENT_BINARY_DIR}/structmapper_generated/${SMS_TARGET}_gen.cpp")
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
        target_include_directories(${_gen_exe} PRIVATE ${catkin_INCLUDE_DIRS})
    endif()

    if(SMS_INCLUDE_DIRS)
        target_include_directories(${_gen_exe} PRIVATE ${SMS_INCLUDE_DIRS})
    endif()
    if(SMS_LINK_LIBRARIES)
        target_link_libraries(${_gen_exe} PRIVATE ${SMS_LINK_LIBRARIES})
    endif()

    foreach(_h IN LISTS _headers_dedup)
        get_filename_component(_hdir "${_h}" DIRECTORY)
        target_include_directories(${_gen_exe} PRIVATE "${_hdir}")
    endforeach()

    set(_run_cmd ${_gen_exe})
    if(CMAKE_CROSSCOMPILING_EMULATOR)
        set(_run_cmd ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:${_gen_exe}>)
    endif()

    set(_manifest "${SMS_OUTPUT}/manifest.json")
    add_custom_command(
        OUTPUT "${_manifest}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SMS_OUTPUT}"
        COMMAND ${_run_cmd} "${SMS_OUTPUT}"
        DEPENDS ${_gen_exe} ${SMS_HEADERS}
        COMMENT "structmapper: generating schemas for package ${PROJECT_NAME}"
        VERBATIM
    )
    add_custom_target(${SMS_TARGET} ALL DEPENDS "${_manifest}")
    set(${SMS_TARGET}_SCHEMA_OUTPUT "${SMS_OUTPUT}" PARENT_SCOPE)
endfunction()
