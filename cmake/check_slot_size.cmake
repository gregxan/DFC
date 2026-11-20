# Rev 2.25: fail the build if the firmware image does not fit one A/B slot.
# Called as: cmake -DBIN=<firmware.bin> -DLIMIT=<bytes> -P check_slot_size.cmake
file(SIZE "${BIN}" size)
if(size GREATER LIMIT)
    message(FATAL_ERROR "firmware.bin is ${size} bytes; an A/B slot holds ${LIMIT} (partition_table.json)")
endif()
math(EXPR pct "100 * ${size} / ${LIMIT}")
message(STATUS "firmware.bin ${size} bytes, ${pct}% of one A/B slot")
