#pragma once
#include <cstdint>

#include "activities/Activity.h"
#include "util/Game2048Store.h"

// Game2048Activity — a self-contained 2048 game screen.
//
// Controls mirror the keyboard/highlight modes: the two SIDE buttons (Up/Down)
// make vertical moves, the two FRONT buttons (Left/Right) make horizontal moves;
// Back leaves to the home menu. The game state (current board + score) and the
// all-time top-3 scores persist to "/.crosspoint/2048.md" via Game2048Store, so
// the player can Continue a game and read their progress on a PC.
//
// Internal state machine (no extra Activity classes): a Start screen offers
// Continue / New game when a resumable game exists; Playing is the board; and
// GameOver shows the final score + top-3 with a New game / Back choice.
class Game2048Activity final : public Activity {
  enum class Screen { Start, Playing, GameOver };

  Screen screen = Screen::Playing;
  game2048::State state;
  int startSelection = 0;       // Start screen: 0 = Continue, 1 = New game
  bool forceFullRefresh = true;  // do a full e-ink refresh on the next render (de-ghost on transitions)
  int movesSinceFull = 0;        // periodic full refresh during play to clear ghosting

  void newGame();                  // reset board + score (keeps top scores), spawn two tiles, -> Playing
  void doMove(game2048::Dir dir);  // apply a move, spawn, score, save, detect game over
  void spawnRandomTile();          // place a 2 (90%) or 4 (10%) in a random empty cell
  void save();                     // serialize + write the sidecar

  void drawBoard();   // header + score + 4x4 grid (shared by Playing)
  void renderStart();
  void renderPlaying();
  void renderGameOver();

 public:
  explicit Game2048Activity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Game2048", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
