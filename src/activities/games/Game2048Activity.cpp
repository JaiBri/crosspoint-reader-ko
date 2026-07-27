#include "Game2048Activity.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <esp_random.h>

#include <algorithm>
#include <string>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* kStatePath = "/.crosspoint/2048.md";
constexpr int kFullRefreshEveryMoves = 12;  // de-ghost the e-ink periodically during play
}  // namespace

void Game2048Activity::onEnter() {
  Activity::onEnter();

  const String existing = Storage.readFile(kStatePath);
  state = game2048::parse(existing.isEmpty() ? std::string() : std::string(existing.c_str()));

  if (state.inProgress) {
    screen = Screen::Start;
    startSelection = 0;  // default to Continue
  } else {
    newGame();
  }
  forceFullRefresh = true;
  requestUpdate();
}

void Game2048Activity::newGame() {
  for (auto& cell : state.board) cell = 0;
  state.score = 0;
  state.inProgress = true;
  spawnRandomTile();
  spawnRandomTile();
  screen = Screen::Playing;
  movesSinceFull = 0;
  forceFullRefresh = true;
  save();
}

void Game2048Activity::spawnRandomTile() {
  int empties[game2048::kCells];
  int n = 0;
  for (int i = 0; i < game2048::kCells; i++) {
    if (state.board[i] == 0) empties[n++] = i;
  }
  if (n == 0) return;
  const int idx = empties[esp_random() % static_cast<uint32_t>(n)];
  state.board[idx] = (esp_random() % 10u == 0u) ? 4 : 2;  // 10% chance of a 4
}

void Game2048Activity::doMove(game2048::Dir dir) {
  uint32_t gained = 0;
  if (!game2048::applyMove(state.board, dir, gained)) return;  // illegal move: ignore

  state.score += gained;
  state.inProgress = true;
  spawnRandomTile();

  if (!game2048::hasMoves(state.board)) {
    game2048::recordScore(state.top, state.score);
    state.inProgress = false;  // nothing left to resume
    screen = Screen::GameOver;
    forceFullRefresh = true;
  } else if (++movesSinceFull >= kFullRefreshEveryMoves) {
    movesSinceFull = 0;
    forceFullRefresh = true;
  }

  save();
  requestUpdate();
}

void Game2048Activity::save() {
  Storage.mkdir("/.crosspoint");
  const std::string md = game2048::serialize(state);
  Storage.writeFile(kStatePath, String(md.c_str()));
}

void Game2048Activity::loop() {
  using Button = MappedInputManager::Button;

  if (screen == Screen::Playing) {
    if (mappedInput.wasPressed(Button::Up)) return doMove(game2048::Dir::Up);
    if (mappedInput.wasPressed(Button::Down)) return doMove(game2048::Dir::Down);
    if (mappedInput.wasPressed(Button::Left)) return doMove(game2048::Dir::Left);
    if (mappedInput.wasPressed(Button::Right)) return doMove(game2048::Dir::Right);
    if (mappedInput.wasPressed(Button::Back)) {
      save();  // keep the game resumable
      finish();
    }
    return;
  }

  if (screen == Screen::Start) {
    if (mappedInput.wasPressed(Button::Up) || mappedInput.wasPressed(Button::Down)) {
      startSelection ^= 1;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(Button::Confirm)) {
      if (startSelection == 0) {
        screen = Screen::Playing;  // Continue the loaded board
        forceFullRefresh = true;
        requestUpdate();
      } else {
        newGame();
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasPressed(Button::Back)) finish();
    return;
  }

  // GameOver
  if (mappedInput.wasPressed(Button::Confirm)) {
    newGame();
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(Button::Back)) finish();
}

void Game2048Activity::drawBoard() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineH = renderer.getLineHeight(UI_FONT_ID);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_2048));

  // Score line under the header.
  const int scoreY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const std::string scoreStr = std::string(tr(STR_GAME_2048_SCORE)) + ": " + std::to_string(state.score);
  renderer.drawCenteredText(UI_FONT_ID, scoreY, scoreStr.c_str());

  // Square board centered in the remaining space.
  const int margin = metrics.contentSidePadding;
  const int boardTop = scoreY + lineH + metrics.verticalSpacing * 2;
  const int bottomReserve = metrics.buttonHintsHeight + metrics.verticalSpacing * 2;
  int side = std::min(pageWidth - margin * 2, pageHeight - boardTop - bottomReserve);
  if (side < 0) side = 0;
  const int cell = side / game2048::kSize;
  side = cell * game2048::kSize;  // snap to a whole number of cells
  const int boardX = (pageWidth - side) / 2;
  const int boardY = boardTop;
  const int pad = std::max(2, cell / 16);  // gap between cells

  for (int r = 0; r < game2048::kSize; r++) {
    for (int c = 0; c < game2048::kSize; c++) {
      const int x = boardX + c * cell;
      const int y = boardY + r * cell;
      renderer.drawRoundedRect(x + pad, y + pad, cell - 2 * pad, cell - 2 * pad, 2, 6, true);

      const uint32_t v = state.board[r * game2048::kSize + c];
      if (v != 0) {
        const std::string num = std::to_string(v);
        const int tw = renderer.getTextWidth(UI_FONT_ID, num.c_str());
        const int tx = x + (cell - tw) / 2;
        const int ty = y + (cell - lineH) / 2;
        renderer.drawText(UI_FONT_ID, tx, ty, num.c_str());
      }
    }
  }
}

void Game2048Activity::renderStart() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineH = renderer.getLineHeight(UI_FONT_ID);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_2048));

  const char* opts[2] = {tr(STR_GAME_2048_CONTINUE), tr(STR_GAME_2048_NEW)};
  int cy = pageHeight / 2 - lineH;
  for (int i = 0; i < 2; i++) {
    const int tw = renderer.getTextWidth(UI_FONT_ID, opts[i]);
    const int boxW = tw + 48;
    const int boxH = lineH + 20;
    const int bx = (pageWidth - boxW) / 2;
    const int by = cy - 10;
    if (startSelection == i) {
      renderer.drawRoundedRect(bx, by, boxW, boxH, 2, 8, true);
    }
    renderer.drawCenteredText(UI_FONT_ID, cy, opts[i]);
    cy += boxH + metrics.verticalSpacing;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, ">", "<");
}

void Game2048Activity::renderPlaying() {
  drawBoard();

  // Front: Back / (unused) / Left / Right. Side (rotated 90° CW): ">" renders as
  // an up-chevron, "<" as a down-chevron — same convention as the keyboard.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, ">", "<");
}

void Game2048Activity::renderGameOver() {
  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineH = renderer.getLineHeight(UI_FONT_ID);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAME_2048_OVER));

  int cy = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 3;
  const std::string scoreStr = std::string(tr(STR_GAME_2048_SCORE)) + ": " + std::to_string(state.score);
  renderer.drawCenteredText(UI_FONT_ID, cy, scoreStr.c_str());
  cy += lineH + metrics.verticalSpacing * 2;

  renderer.drawCenteredText(UI_FONT_ID, cy, tr(STR_GAME_2048_BEST));
  cy += lineH + metrics.verticalSpacing;
  for (int i = 0; i < 3; i++) {
    const std::string line = std::to_string(i + 1) + ".   " + std::to_string(state.top[i]);
    renderer.drawCenteredText(UI_FONT_ID, cy, line.c_str());
    cy += lineH + metrics.verticalSpacing / 2;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_GAME_2048_NEW), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void Game2048Activity::render(RenderLock&&) {
  renderer.clearScreen();

  switch (screen) {
    case Screen::Start:
      renderStart();
      break;
    case Screen::Playing:
      renderPlaying();
      break;
    case Screen::GameOver:
      renderGameOver();
      break;
  }

  const auto mode = forceFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH;
  forceFullRefresh = false;
  renderer.displayBuffer(mode);
}
