// Native host test for src/util/Game2048Store.{h,cpp}.
// Build/run via test/run_game2048_store_test.sh (no PlatformIO needed).
//
// Covers: slide/merge rules (merge-once, scoring, no-op moves) in all four
// directions, game-over detection, top-3 insertion, and serialize->parse
// round-trips (in-progress game and top-scores-only), plus tolerance of an
// empty / garbled file.

#include <cstdio>
#include <cstring>
#include <string>

#include "util/Game2048Store.h"

using game2048::Dir;
using game2048::kCells;
using game2048::State;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

static void setBoard(uint32_t board[kCells], const uint32_t (&src)[kCells]) {
  std::memcpy(board, src, sizeof(uint32_t) * kCells);
}

static bool boardEq(const uint32_t a[kCells], const uint32_t (&b)[kCells]) {
  return std::memcmp(a, b, sizeof(uint32_t) * kCells) == 0;
}

static void testSlideMergeLeft() {
  uint32_t board[kCells];
  setBoard(board, {
                      2, 2, 0, 0,  //
                      4, 0, 4, 0,  //
                      0, 0, 0, 0,  //
                      2, 0, 0, 2,  //
                  });
  uint32_t gained = 999;
  const bool changed = game2048::applyMove(board, Dir::Left, gained);
  CHECK(changed);
  CHECK(gained == 16);  // row0: 2+2=4, row1: 4+4=8, row3: 2+2=4  => 16
  const uint32_t want[kCells] = {
      4, 0, 0, 0,  //
      8, 0, 0, 0,  //
      0, 0, 0, 0,  //
      4, 0, 0, 0,  //
  };
  CHECK(boardEq(board, want));
}

static void testMergeOncePerMove() {
  // Four equal tiles in a row merge into TWO tiles, not one.
  uint32_t board[kCells];
  setBoard(board, {
                      2, 2, 2, 2,  //
                      0, 0, 0, 0,  //
                      0, 0, 0, 0,  //
                      0, 0, 0, 0,  //
                  });
  uint32_t gained = 0;
  const bool changed = game2048::applyMove(board, Dir::Left, gained);
  CHECK(changed);
  CHECK(gained == 8);  // 2+2->4 and 2+2->4
  const uint32_t want[kCells] = {
      4, 4, 0, 0,  //
      0, 0, 0, 0,  //
      0, 0, 0, 0,  //
      0, 0, 0, 0,  //
  };
  CHECK(boardEq(board, want));
}

static void testNoOpMove() {
  uint32_t board[kCells];
  setBoard(board, {
                      2, 4, 8, 16,  //
                      0, 0, 0, 0,   //
                      0, 0, 0, 0,   //
                      0, 0, 0, 0,   //
                  });
  uint32_t gained = 7;
  const bool changed = game2048::applyMove(board, Dir::Left, gained);
  CHECK(!changed);
  CHECK(gained == 0);
}

static void testDirections() {
  // A single tile slides all the way to each edge.
  uint32_t board[kCells];
  uint32_t gained = 0;

  setBoard(board, {0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  game2048::applyMove(board, Dir::Right, gained);
  const uint32_t wantR[kCells] = {0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(boardEq(board, wantR));

  setBoard(board, {0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  game2048::applyMove(board, Dir::Up, gained);
  const uint32_t wantU[kCells] = {0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(boardEq(board, wantU));

  setBoard(board, {0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  game2048::applyMove(board, Dir::Down, gained);
  const uint32_t wantD[kCells] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0};
  CHECK(boardEq(board, wantD));
}

static void testHasMoves() {
  // Full board, checkerboard of distinct neighbours => no moves.
  uint32_t full[kCells] = {
      2, 4, 2, 4,  //
      4, 2, 4, 2,  //
      2, 4, 2, 4,  //
      4, 2, 4, 2,  //
  };
  CHECK(!game2048::hasMoves(full));

  // Same but with one empty cell => has a move.
  uint32_t withEmpty[kCells];
  std::memcpy(withEmpty, full, sizeof(full));
  withEmpty[5] = 0;
  CHECK(game2048::hasMoves(withEmpty));

  // Full but two equal horizontal neighbours => has a move.
  uint32_t mergeable[kCells];
  std::memcpy(mergeable, full, sizeof(full));
  mergeable[1] = 2;  // row0 becomes 2,2,2,4
  CHECK(game2048::hasMoves(mergeable));
}

static void testRecordScore() {
  uint32_t top[3] = {0, 0, 0};
  game2048::recordScore(top, 100);
  game2048::recordScore(top, 50);
  game2048::recordScore(top, 300);
  game2048::recordScore(top, 200);  // pushes 50 off
  CHECK(top[0] == 300);
  CHECK(top[1] == 200);
  CHECK(top[2] == 100);

  game2048::recordScore(top, 10);  // below third place, ignored
  CHECK(top[0] == 300 && top[1] == 200 && top[2] == 100);

  game2048::recordScore(top, 0);  // zero ignored
  CHECK(top[0] == 300 && top[1] == 200 && top[2] == 100);
}

static void testRoundTripInProgress() {
  State s;
  const uint32_t b[kCells] = {
      2, 0, 4, 0,    //
      0, 8, 16, 0,   //
      32, 0, 0, 2,   //
      0, 0, 64, 128,  //
  };
  setBoard(s.board, b);
  s.score = 1320;
  s.inProgress = true;
  s.top[0] = 4096;
  s.top[1] = 1320;
  s.top[2] = 256;

  const std::string md = game2048::serialize(s);
  // Human-readable bits present:
  CHECK(md.find("# 2048") != std::string::npos);
  CHECK(md.find("## Current game — score 1320") != std::string::npos);
  CHECK(md.find("## Top scores") != std::string::npos);
  // Machine source-of-truth present:
  CHECK(md.find("<!-- 2048 v1 score=1320 board=2,0,4,0,0,8,16,0,32,0,0,2,0,0,64,128 -->") != std::string::npos);
  CHECK(md.find("<!-- 2048-top v1 top=4096,1320,256 -->") != std::string::npos);

  const State back = game2048::parse(md);
  CHECK(back.inProgress);
  CHECK(back.score == 1320);
  CHECK(boardEq(back.board, b));
  CHECK(back.top[0] == 4096 && back.top[1] == 1320 && back.top[2] == 256);
}

static void testRoundTripTopOnly() {
  // Game over: no resumable game, only top scores persisted.
  State s;
  s.inProgress = false;
  s.top[0] = 512;
  s.top[1] = 256;
  s.top[2] = 0;

  const std::string md = game2048::serialize(s);
  CHECK(md.find("## Current game") == std::string::npos);  // no resumable game written
  CHECK(md.find("<!-- 2048 v1 ") == std::string::npos);
  CHECK(md.find("<!-- 2048-top v1 top=512,256,0 -->") != std::string::npos);

  const State back = game2048::parse(md);
  CHECK(!back.inProgress);
  CHECK(back.score == 0);
  CHECK(back.top[0] == 512 && back.top[1] == 256 && back.top[2] == 0);
  const uint32_t zero[kCells] = {0};
  CHECK(boardEq(back.board, zero));
}

static void testParseEmptyAndGarbage() {
  const State a = game2048::parse("");
  CHECK(!a.inProgress);
  CHECK(a.score == 0);

  const State b = game2048::parse("just some prose\n# 2048\nrandom <!-- not ours --> text\n");
  CHECK(!b.inProgress);
  CHECK(b.top[0] == 0);

  // An all-zero board comment must NOT be treated as a resumable game.
  const State c = game2048::parse("<!-- 2048 v1 score=0 board=0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 -->\n");
  CHECK(!c.inProgress);
}

int main() {
  testSlideMergeLeft();
  testMergeOncePerMove();
  testNoOpMove();
  testDirections();
  testHasMoves();
  testRecordScore();
  testRoundTripInProgress();
  testRoundTripTopOnly();
  testParseEmptyAndGarbage();

  if (g_failures == 0) {
    std::printf("Game2048Store: ALL TESTS PASSED\n");
    return 0;
  }
  std::printf("Game2048Store: %d CHECK(s) FAILED\n", g_failures);
  return 1;
}
