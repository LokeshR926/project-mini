file(
  GLOB_RECURSE
  CHAMPSIM_REPLACEMENT_POLICY_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/replacement/srrip/*.cc
)

list(APPEND CHAMPSIM_BRANCH_PREDICTOR_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/replacement/base_replacement.cc)
