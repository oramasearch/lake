if(NOT DEFINED INPUT_DIR OR NOT DEFINED OUTPUT_FILE)
  message(FATAL_ERROR "INPUT_DIR and OUTPUT_FILE are required")
endif()

get_filename_component(OUTPUT_DIRECTORY "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
file(WRITE "${OUTPUT_FILE}" "#include \"lake/api/web_ui.hpp\"\n\n#include <string_view>\n\nnamespace lake::api {\n\nnamespace {\n")

set(FONT_FILES
  ibm-plex-sans-400.woff2
  ibm-plex-sans-500.woff2
  ibm-plex-sans-600.woff2
  ibm-plex-mono-400.woff2
  ibm-plex-mono-500.woff2)

foreach(FONT_FILE IN LISTS FONT_FILES)
  string(REPLACE "." "_" FONT_SYMBOL "${FONT_FILE}")
  string(REPLACE "-" "_" FONT_SYMBOL "${FONT_SYMBOL}")
  file(READ "${INPUT_DIR}/${FONT_FILE}" FONT_HEX HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," FONT_BYTES "${FONT_HEX}")
  file(APPEND "${OUTPUT_FILE}"
    "constexpr unsigned char ${FONT_SYMBOL}[] = {${FONT_BYTES}};\n")
endforeach()

file(APPEND "${OUTPUT_FILE}" "\n} // namespace\n\nstd::string_view web_ui_font(std::string_view name) {\n")
foreach(FONT_FILE IN LISTS FONT_FILES)
  string(REPLACE "." "_" FONT_SYMBOL "${FONT_FILE}")
  string(REPLACE "-" "_" FONT_SYMBOL "${FONT_SYMBOL}")
  file(APPEND "${OUTPUT_FILE}"
    "    if (name == \"${FONT_FILE}\") {\n"
    "        return {reinterpret_cast<const char*>(${FONT_SYMBOL}), sizeof(${FONT_SYMBOL})};\n"
    "    }\n")
endforeach()
file(APPEND "${OUTPUT_FILE}" "    return {};\n}\n\n} // namespace lake::api\n")
