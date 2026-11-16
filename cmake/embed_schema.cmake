# SPDX-License-Identifier: MIT

# Embeds db/schema.sql into the binary as one raw string (lake#1119), so `lake migrate` applies the
# schema the image was built with and the operator vendors no copy (orama-operator#936).
if(NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE)
  message(FATAL_ERROR "INPUT_FILE and OUTPUT_FILE are required")
endif()
get_filename_component(OUTPUT_DIRECTORY "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
file(READ "${INPUT_FILE}" BODY)
string(FIND "${BODY}" ")lakeschema\"" CLASH)
if(NOT CLASH EQUAL -1)
  message(FATAL_ERROR "${INPUT_FILE} contains the raw string delimiter")
endif()
file(WRITE "${OUTPUT_FILE}" "#include <string_view>\n\n#include \"lake/control/schema.hpp\"\n\nnamespace lake::control {\n\nstd::string_view embedded_schema_sql() {\n    static constexpr std::string_view sql = R\"lakeschema(${BODY})lakeschema\";\n    return sql;\n}\n\n} // namespace lake::control\n")
