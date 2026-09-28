if(NOT DEFINED INPUT_DIR OR NOT DEFINED OUTPUT_FILE)
  message(FATAL_ERROR "INPUT_DIR and OUTPUT_FILE are required")
endif()

get_filename_component(OUTPUT_DIRECTORY "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
file(WRITE "${OUTPUT_FILE}" "#include \"lake/api/web_ui.hpp\"\n\nnamespace lake::api {\n")
file(APPEND "${OUTPUT_FILE}" "std::string_view web_ui_experiment_asset(std::string_view name) {\n")
foreach(ASSET uPlot.iife.js uPlot.min.css experiments.js experiments.css)
  file(READ "${INPUT_DIR}/${ASSET}" BODY)
  string(FIND "${BODY}" ")lake_asset\"" DELIMITER)
  if(NOT DELIMITER EQUAL -1)
    message(FATAL_ERROR "Reserved raw string delimiter in ${ASSET}")
  endif()
  file(APPEND "${OUTPUT_FILE}"
    "    if (name == \"${ASSET}\") {\n"
    "        return R\"lake_asset(${BODY})lake_asset\";\n"
    "    }\n")
endforeach()
file(APPEND "${OUTPUT_FILE}" "    return {};\n}\n} // namespace lake::api\n")
