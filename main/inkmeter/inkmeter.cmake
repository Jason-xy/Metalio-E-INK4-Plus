set(CHECKLIST_SRCS
    "display/screen/checklist_cache.cc"
    "display/screen/task_screen/task_screen.cc")

# ESP-IDF scans component requirements before loading the SDK configuration.
if(CMAKE_SCRIPT_MODE_FILE)
    return()
endif()

if(CONFIG_INKMETER_ENABLED)
    if(CONFIG_ESP_COREDUMP_ENABLE)
        message(FATAL_ERROR "InkMeter builds require CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y")
    endif()
    file(GLOB INKMETER_SRCS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/inkmeter/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/inkmeter/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/display/screen/inkmeter_screen/*.cc")
    set(INKMETER_WEB "inkmeter/web.html")

    set(WORKBUDDY_SRCS "")
    set(ASSISTANT_SCREEN_SRCS "")
    set(A2UI_SRCS "")
    set(CHECKLIST_SRCS "")
    list(FILTER SETTINGS_SCREEN_SRCS EXCLUDE REGEX "settings_conversation_tab\\.cc$")
    list(FILTER STANDBY_SCREEN_SRCS EXCLUDE REGEX "standby_classic_todos\\.cc$")
endif()

# Keep the optional icon out of upstream builds without changing its generator.
set(HOME_ICON_OVERRIDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/embedded")
if(NOT CONFIG_INKMETER_ENABLED)
    set(HOME_ICON_OVERRIDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/upstream-home-icons")
    file(MAKE_DIRECTORY "${HOME_ICON_OVERRIDE_DIR}")
    file(GLOB _profile_icon_inputs CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/embedded/ic_app_home_*.png")
    set(_profile_icon_names "")
    foreach(_icon IN LISTS _profile_icon_inputs)
        get_filename_component(_name "${_icon}" NAME)
        if(NOT _name STREQUAL "ic_app_home_inkmeter.png")
            configure_file("${_icon}" "${HOME_ICON_OVERRIDE_DIR}/${_name}" COPYONLY)
            list(APPEND _profile_icon_names "${_name}")
        endif()
    endforeach()
    file(GLOB _profile_icon_cached "${HOME_ICON_OVERRIDE_DIR}/ic_app_home_*.png")
    foreach(_icon IN LISTS _profile_icon_cached)
        get_filename_component(_name "${_icon}" NAME)
        if(NOT _name IN_LIST _profile_icon_names)
            file(REMOVE "${_icon}")
        endif()
    endforeach()
endif()
