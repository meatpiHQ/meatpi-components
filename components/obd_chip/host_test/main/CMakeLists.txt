# Host test: the pure parse/framing, fw-version and sleep-config modules, and
# (test_request.c) the production request engine, claim model and fan-out
# against a fake chip that stands in for the UART: no UART/GPIO deps.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_parse.c"
         "test_stslcs.c"
         "test_request.c"
         "test_kline.c"
         "test_settle.c"
         "${comp_dir}/obd_chip_cmd.c"
         "${comp_dir}/obd_chip_claim.c"
         "${comp_dir}/obd_chip_fanout.c"
         "${comp_dir}/obd_chip_parse.c"
         "${comp_dir}/obd_chip_class.c"
         "${comp_dir}/obd_chip_kline.c"
         "${comp_dir}/obd_chip_fw_version.c"
         "${comp_dir}/obd_chip_stslcs.c"
         "${comp_dir}/obd_chip_settle.c"
         "${comp_dir}/obd_chip_fault.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}" "${comp_dir}/../obd_gate/include"
    REQUIRES unity esp_timer
)

# OBD_CHIP_HOST_TEST: the driver's test-only hook (obd_cmd_test_new_boot, the
# per-boot state of the protocol save as a restart leaves it; standard 3).
target_compile_definitions(${COMPONENT_LIB} PRIVATE OBD_CHIP_HOST_TEST=1)

# Who can pull the chip's reset line (issue #1023: a reset inside a boot of
# the chip destroys it, so the line is the last resort and in no API). The
# component's sources are counted here, every time one of them changes, and
# test_reset_line_has_one_driver_and_two_last_resort_calls holds the counts:
#   - the reset pin is named in obd_chip_pins.c alone, and pulled low there
#     in one place
#   - the pulse is named in the pins file, the bring-up and its own header
#     only, that header is included by those two files only, and the
#     bring-up calls it twice (a call has an argument; a comment writes `()`)
#   - obd_chip.h offers no hardware reset
# The sources are the ones the component builds (its own CMakeLists.txt),
# not whatever lies in the folder: a copy of the tree on a bench can carry
# files of an old branch (obd_chip_sweep.c on the Pi, 2026-11-12).
set(comp_cmake "${comp_dir}/CMakeLists.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${comp_cmake}")
file(READ "${comp_cmake}" comp_cmake_text)
string(REGEX MATCHALL "\"obd_chip[a-z0-9_]*\\.c\"" built "${comp_cmake_text}")
file(GLOB obd_scan "${comp_dir}/*.h" "${comp_dir}/include/*.h")

foreach(quoted ${built})
    string(REPLACE "\"" "" plain "${quoted}")
    list(APPEND obd_scan "${comp_dir}/${plain}")
endforeach()

list(REMOVE_DUPLICATES obd_scan)
set(pin_outside 0)
set(pin_low 0)
set(pulse_elsewhere 0)
set(pulse_calls 0)
set(line_includes 0)
set(public_api 0)

foreach(src ${obd_scan})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${src}")
    get_filename_component(name "${src}" NAME)
    file(READ "${src}" text)

    string(REGEX MATCHALL "GPIO_NUM_41|OBD_RESET_PIN" hits "${text}")
    list(LENGTH hits n)
    if(n GREATER 0 AND NOT name STREQUAL "obd_chip_pins.c")
        math(EXPR pin_outside "${pin_outside} + 1")
    endif()

    if(name STREQUAL "obd_chip_pins.c")
        string(REGEX MATCHALL "gpio_set_level\\(OBD_RESET_PIN, 0\\)" hits "${text}")
        list(LENGTH hits pin_low)
    endif()

    string(REGEX MATCHALL "obd_pin_reset_pulse" hits "${text}")
    list(LENGTH hits n)
    if(n GREATER 0 AND NOT name MATCHES "^obd_chip_(pins|bringup)\\.c$"
       AND NOT name STREQUAL "obd_chip_reset_line.h")
        math(EXPR pulse_elsewhere "${pulse_elsewhere} + 1")
    endif()

    if(name STREQUAL "obd_chip_bringup.c")
        string(REGEX MATCHALL "obd_pin_reset_pulse\\([^)]" hits "${text}")
        list(LENGTH hits pulse_calls)
    endif()

    string(REGEX MATCHALL "#include \"obd_chip_reset_line\\.h\"" hits "${text}")
    list(LENGTH hits n)
    math(EXPR line_includes "${line_includes} + ${n}")

    if(name STREQUAL "obd_chip.h")
        set(api_re "obd_chip_hard_reset[ ]*\\(void\\)|obd_chip_reset_pulse|obd_pin_")
        string(REGEX MATCHALL "${api_re}" hits "${text}")
        list(LENGTH hits public_api)
    endif()
endforeach()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    OBD_SRC_RESET_PIN_OUTSIDE_PINS=${pin_outside}
    OBD_SRC_RESET_PIN_LOW_WRITES=${pin_low}
    OBD_SRC_PULSE_NAMED_ELSEWHERE=${pulse_elsewhere}
    OBD_SRC_PULSE_CALLS_IN_BRINGUP=${pulse_calls}
    OBD_SRC_RESET_LINE_INCLUDES=${line_includes}
    OBD_SRC_PUBLIC_RESET_PULSE_API=${public_api})
message(STATUS "obd_chip reset line: pin outside pins.c ${pin_outside}, low writes ${pin_low}, "
               "pulse named elsewhere ${pulse_elsewhere}, calls in the bring-up ${pulse_calls}, "
               "header includes ${line_includes}, public API ${public_api}")
