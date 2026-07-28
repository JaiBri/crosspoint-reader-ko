# HighlightStoreTest has its own int main() and does not link gtest, so it is
# registered with add_test() rather than gtest_discover_tests(). It still runs
# under the same `ctest` invocation CI uses.
add_executable(HighlightStoreTest
  HighlightStoreTest.cpp
  ${REPO_ROOT}/src/util/HighlightStore.cpp
)

target_include_directories(HighlightStoreTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(HighlightStoreTest PRIVATE
  crosspoint_test_common
)

add_test(NAME HighlightStore COMMAND HighlightStoreTest)
