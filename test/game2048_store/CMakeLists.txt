# Own int main(), no gtest — see test/highlight_store/CMakeLists.txt.
add_executable(Game2048StoreTest
  Game2048StoreTest.cpp
  ${REPO_ROOT}/src/util/Game2048Store.cpp
)

target_include_directories(Game2048StoreTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(Game2048StoreTest PRIVATE
  crosspoint_test_common
)

add_test(NAME Game2048Store COMMAND Game2048StoreTest)
