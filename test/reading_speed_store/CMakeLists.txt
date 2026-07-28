# Own int main(), no gtest — see test/highlight_store/CMakeLists.txt.
add_executable(ReadingSpeedStoreTest
  ReadingSpeedStoreTest.cpp
  ${REPO_ROOT}/src/util/ReadingSpeedStore.cpp
)

target_include_directories(ReadingSpeedStoreTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ReadingSpeedStoreTest PRIVATE
  crosspoint_test_common
)

add_test(NAME ReadingSpeedStore COMMAND ReadingSpeedStoreTest)
