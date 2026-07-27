#include "Game2048Store.h"

#include <cstdlib>
#include <vector>

namespace game2048 {

namespace {

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// Split `s` on `sep`. Empty fields are preserved.
std::vector<std::string> splitChar(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t pos = s.find(sep, start);
    if (pos == std::string::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

// Parse a non-negative integer. Returns false on any non-numeric content.
bool parseUInt(const std::string& s, long& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (end != s.c_str() + s.size() || v < 0) return false;
  out = v;
  return true;
}

// Map a (direction, line, position-from-the-target-edge) to a board index.
// `line` is the row (Left/Right) or column (Up/Down); `i` counts from the edge
// the tiles slide toward, so slideLine can always compact toward index 0.
int cellIndex(Dir dir, int line, int i) {
  switch (dir) {
    case Dir::Left:
      return line * kSize + i;
    case Dir::Right:
      return line * kSize + (kSize - 1 - i);
    case Dir::Up:
      return i * kSize + line;
    case Dir::Down:
      return (kSize - 1 - i) * kSize + line;
  }
  return 0;
}

// Compact non-zero cells toward index 0, then merge equal neighbours once.
// Adds merged values to `gained`; returns true if the line changed.
bool slideLine(uint32_t line[kSize], uint32_t& gained) {
  uint32_t comp[kSize] = {0};
  int n = 0;
  for (int i = 0; i < kSize; i++) {
    if (line[i] != 0) comp[n++] = line[i];
  }

  uint32_t out[kSize] = {0};
  int o = 0;
  for (int i = 0; i < n;) {
    if (i + 1 < n && comp[i] == comp[i + 1]) {
      const uint32_t merged = comp[i] * 2;
      out[o++] = merged;
      gained += merged;
      i += 2;
    } else {
      out[o++] = comp[i];
      i += 1;
    }
  }

  bool changed = false;
  for (int i = 0; i < kSize; i++) {
    if (out[i] != line[i]) changed = true;
    line[i] = out[i];
  }
  return changed;
}

}  // namespace

bool applyMove(uint32_t board[kCells], Dir dir, uint32_t& gained) {
  gained = 0;
  bool changed = false;
  for (int line = 0; line < kSize; line++) {
    uint32_t buf[kSize];
    for (int i = 0; i < kSize; i++) buf[i] = board[cellIndex(dir, line, i)];
    if (slideLine(buf, gained)) changed = true;
    for (int i = 0; i < kSize; i++) board[cellIndex(dir, line, i)] = buf[i];
  }
  return changed;
}

bool hasMoves(const uint32_t board[kCells]) {
  for (int i = 0; i < kCells; i++) {
    if (board[i] == 0) return true;
  }
  for (int r = 0; r < kSize; r++) {
    for (int c = 0; c < kSize; c++) {
      const uint32_t v = board[r * kSize + c];
      if (c + 1 < kSize && board[r * kSize + (c + 1)] == v) return true;
      if (r + 1 < kSize && board[(r + 1) * kSize + c] == v) return true;
    }
  }
  return false;
}

void recordScore(uint32_t top[3], uint32_t score) {
  if (score == 0) return;
  for (int i = 0; i < 3; i++) {
    if (score > top[i]) {
      for (int j = 2; j > i; j--) top[j] = top[j - 1];
      top[i] = score;
      return;
    }
  }
}

std::string serialize(const State& s) {
  std::string out = "# 2048\n\n";

  if (s.inProgress) {
    out += "## Current game — score " + std::to_string(s.score) + "\n\n";
    for (int r = 0; r < kSize; r++) {
      out += "|";
      for (int c = 0; c < kSize; c++) {
        const uint32_t v = s.board[r * kSize + c];
        std::string cell = v ? std::to_string(v) : ".";
        while (cell.size() < 5) cell = " " + cell;  // right-align to a fixed width
        out += cell + " |";
      }
      out += "\n";
    }
    out += "\n<!-- 2048 v1 score=" + std::to_string(s.score) + " board=";
    for (int i = 0; i < kCells; i++) {
      if (i) out += ",";
      out += std::to_string(s.board[i]);
    }
    out += " -->\n\n";
  }

  if (s.top[0] || s.top[1] || s.top[2]) {
    out += "## Top scores\n\n";
    for (int i = 0; i < 3; i++) {
      out += std::to_string(i + 1) + ". " + std::to_string(s.top[i]) + "\n";
    }
    out += "\n<!-- 2048-top v1 top=" + std::to_string(s.top[0]) + "," + std::to_string(s.top[1]) + "," +
           std::to_string(s.top[2]) + " -->\n";
  }

  return out;
}

State parse(const std::string& markdown) {
  State s;
  for (auto line : splitChar(markdown, '\n')) {
    if (!line.empty() && line.back() == '\r') line.pop_back();

    if (startsWith(line, "<!-- 2048 v1 ")) {
      for (const auto& tok : splitChar(line, ' ')) {
        if (startsWith(tok, "score=")) {
          long v;
          if (parseUInt(tok.substr(6), v)) s.score = static_cast<uint32_t>(v);
        } else if (startsWith(tok, "board=")) {
          const auto cells = splitChar(tok.substr(6), ',');
          if (cells.size() == static_cast<size_t>(kCells)) {
            uint32_t tmp[kCells];
            bool ok = true;
            for (int i = 0; i < kCells; i++) {
              long v;
              if (!parseUInt(cells[i], v)) {
                ok = false;
                break;
              }
              tmp[i] = static_cast<uint32_t>(v);
            }
            if (ok) {
              bool any = false;
              for (int i = 0; i < kCells; i++) {
                s.board[i] = tmp[i];
                if (tmp[i]) any = true;
              }
              s.inProgress = any;  // an all-zero board is treated as "no game"
            }
          }
        }
      }
    } else if (startsWith(line, "<!-- 2048-top v1 ")) {
      for (const auto& tok : splitChar(line, ' ')) {
        if (startsWith(tok, "top=")) {
          const auto vals = splitChar(tok.substr(4), ',');
          for (int i = 0; i < 3 && i < static_cast<int>(vals.size()); i++) {
            long v;
            if (parseUInt(vals[i], v)) s.top[i] = static_cast<uint32_t>(v);
          }
        }
      }
    }
  }
  return s;
}

}  // namespace game2048
