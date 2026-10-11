# Embeds the vendored workspace engine input schemas (src/api/engine_openapi.json, lake#650) into
# the binary as one raw string, read by register_engine_capabilities.
if(NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE)
  message(FATAL_ERROR "INPUT_FILE and OUTPUT_FILE are required")
endif()
get_filename_component(OUTPUT_DIRECTORY "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
file(READ "${INPUT_FILE}" BODY)
string(FIND "${BODY}" ")lakeengine\"" CLASH)
if(NOT CLASH EQUAL -1)
  message(FATAL_ERROR "${INPUT_FILE} contains the raw string delimiter")
endif()
file(WRITE "${OUTPUT_FILE}" "#include <string_view>\n\nnamespace lake::api {\n\nstd::string_view engine_openapi_snapshot() {\n    static constexpr std::string_view snapshot = R\"lakeengine(${BODY})lakeengine\";\n    return snapshot;\n}\n\n} // namespace lake::api\n")
