# Embeds lake's Docs section (docs/guide/*.md, lake#252) into the binary: one raw string per page,
# in the order GUIDE_FILES lists, with the page's slug (its file name) and title (its first line).
if(NOT DEFINED INPUT_DIR OR NOT DEFINED OUTPUT_FILE)
  message(FATAL_ERROR "INPUT_DIR and OUTPUT_FILE are required")
endif()

set(GUIDE_FILES index concepts load-data train ship agents developer)

get_filename_component(OUTPUT_DIRECTORY "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
file(WRITE "${OUTPUT_FILE}" "#include \"lake/api/web_ui.hpp\"\n\nnamespace lake::api {\n\nconst std::vector<WebUiGuidePage>& web_ui_guide() {\n    static const std::vector<WebUiGuidePage> pages{\n")
foreach(SLUG IN LISTS GUIDE_FILES)
  file(READ "${INPUT_DIR}/${SLUG}.md" BODY)
  string(REGEX MATCH "^# [^\n]*" TITLE_LINE "${BODY}")
  string(SUBSTRING "${TITLE_LINE}" 2 -1 TITLE)
  file(APPEND "${OUTPUT_FILE}" "        {\"${SLUG}\", R\"lakeguide(${TITLE})lakeguide\", R\"lakeguide(${BODY})lakeguide\"},\n")
endforeach()
file(APPEND "${OUTPUT_FILE}" "    };\n    return pages;\n}\n\n} // namespace lake::api\n")
