# Embeds lake's Docs section (docs/guide/*.md, lake#252) into the binary: one raw string per page,
# in the order GUIDE_SECTIONS lists, with the page's slug (its file name), its title (its first
# line) and the section the Docs navigation groups it under (lake#561).
if(NOT DEFINED INPUT_DIR OR NOT DEFINED OUTPUT_FILE)
  message(FATAL_ERROR "INPUT_DIR and OUTPUT_FILE are required")
endif()

# One entry per section, "<Section>|<slug> <slug> ...", in reading order. A slug ending in "?" is
# optional: it is embedded when its file exists and skipped otherwise (a page written on its own
# branch can take its slot before it lands).
set(GUIDE_SECTIONS
  "Get Started|index concepts workspaces account ask-orama phone"
  "Data|repositories repository-collaboration repository-settings drives sharing search load-data connections transcripts mcp-connections"
  "Design|design solutions"
  "Train|train agents experiments streams"
  "Ship|ship apps registry deployments services"
  "Network|network"
  "Organization|organizations access sso tokens secrets activity usage billing"
  "Developer|developer mcp cli"
  "Reference|security faq"
)

get_filename_component(OUTPUT_DIRECTORY "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
file(WRITE "${OUTPUT_FILE}" "#include \"lake/api/web_ui.hpp\"\n\nnamespace lake::api {\n\nconst std::vector<WebUiGuidePage>& web_ui_guide() {\n    static const std::vector<WebUiGuidePage> pages{\n")
foreach(ENTRY IN LISTS GUIDE_SECTIONS)
  string(FIND "${ENTRY}" "|" BAR)
  string(SUBSTRING "${ENTRY}" 0 ${BAR} SECTION)
  math(EXPR AFTER "${BAR} + 1")
  string(SUBSTRING "${ENTRY}" ${AFTER} -1 SLUGS)
  string(REPLACE " " ";" SLUGS "${SLUGS}")
  foreach(SLUG IN LISTS SLUGS)
    if(SLUG MATCHES "\\?$")
      string(REGEX REPLACE "\\?$" "" SLUG "${SLUG}")
      if(NOT EXISTS "${INPUT_DIR}/${SLUG}.md")
        continue()
      endif()
    endif()
    file(READ "${INPUT_DIR}/${SLUG}.md" BODY)
    string(REGEX MATCH "^# [^\n]*" TITLE_LINE "${BODY}")
    string(SUBSTRING "${TITLE_LINE}" 2 -1 TITLE)
    file(APPEND "${OUTPUT_FILE}" "        {\"${SLUG}\", R\"lakeguide(${TITLE})lakeguide\", R\"lakeguide(${BODY})lakeguide\", \"${SECTION}\"},\n")
  endforeach()
endforeach()
file(APPEND "${OUTPUT_FILE}" "    };\n    return pages;\n}\n\n} // namespace lake::api\n")
