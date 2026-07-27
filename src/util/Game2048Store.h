#pragma once

#include <cstdint>
#include <string>

// Game2048Store — pure model + Markdown (de)serialization for the 2048 game.
// Deliberately free of any HAL/Storage/Arduino dependency so it can be
// unit-tested natively on the host (see test/game2048_store/). The actual
// SD-card read/write happens at the call site via Storage.readFile/writeFile;
// this module only turns the game State <-> Markdown text and owns the rules.
//
// Persistence is GLOBAL (one file, not per-book): "/.crosspoint/2048.md". The
// file holds the current (resumable) game — board + score — plus the all-time
// top-3 scores. The `<!-- 2048 ... -->` HTML-comment lines are the machine
// source of truth; the human-readable board table / score list above them is
// decorative and ignored on parse, so the file stays nice to read on a PC.

namespace game2048 {

constexpr int kSize = 4;            // 4x4 board
constexpr int kCells = kSize * kSize;

enum class Dir { Up, Down, Left, Right };

struct State {
  uint32_t board[kCells] = {};  // row-major; 0 = empty, else the tile value (2,4,8,…)
  uint32_t score = 0;           // current game score
  bool inProgress = false;      // a resumable game exists (board has tiles)
  uint32_t top[3] = {0, 0, 0};  // all-time top-3 scores, descending
};

// Slide + merge the whole board one step in `dir`. Each tile merges at most
// once per move. Adds the value of every merge to `gained`. Returns true iff
// the board changed (i.e. the move was legal and should spawn a new tile).
bool applyMove(uint32_t board[kCells], Dir dir, uint32_t& gained);

// True while at least one move is possible (an empty cell, or two equal
// orthogonal neighbours). False == game over.
bool hasMoves(const uint32_t board[kCells]);

// Insert `score` into the descending top-3 list in place (no-op for 0 or for a
// score that doesn't beat the current third place).
void recordScore(uint32_t top[3], uint32_t score);

// Render the state to the Markdown sidecar text (current game written only when
// inProgress; top scores written whenever any are non-zero).
std::string serialize(const State& s);

// Parse the sidecar text back into a State. Tolerant of hand-edits and a
// missing/garbled file (returns a default State with inProgress=false).
State parse(const std::string& markdown);

}  // namespace game2048
