/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Snake Game on STM32F429I-Discovery
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "ili9341.h"
#include "stmpe811.h"
#include <stdlib.h>
#include <stdio.h>
#include "../../Utilities/Fonts/fonts.h"

#include "../../Utilities/Fonts/font8.c"
#include "../../Utilities/Fonts/font12.c"
#include "../../Utilities/Fonts/font16.c"
#include "../../Utilities/Fonts/font20.c"
#include "../../Utilities/Fonts/font24.c"

/* Private define ------------------------------------------------------------*/
#define LCD_W 240
#define LCD_H 320
#define PIXEL_SIZE 10
#define MAX_GAME_X 24
#define MAX_GAME_Y 24
#define FRAME_BUFFER_ADDR ((uint32_t)0xD0000000)

/* Color definitions (RGB565) */
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_YELLOW  0xFFE0

#define TS_I2C_ADDRESS 0x82

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c3;
LTDC_HandleTypeDef hltdc;
SPI_HandleTypeDef hspi5;
SDRAM_HandleTypeDef hsdram1;
TIM_HandleTypeDef htim6;
ADC_HandleTypeDef hadc1; /* Joystick Y axis: PC3 */
ADC_HandleTypeDef hadc2; /* Joystick X axis: PA5 */

/* Joystick calibration (re-captured every time the Menu is entered) */
#define JOY_DEADZONE 700
static uint16_t joyXCenter = 2048;
static uint16_t joyYCenter = 2048;
volatile uint8_t requestJoyRecalibrate = 0;

/* Game state */
volatile int gameSpeed = 150;
volatile int snakeDir = 1;    /* 0:Up, 1:Right, 2:Down, 3:Left (requested; may change more than once per tick) */
volatile int lastAppliedDir = 1; /* direction actually used by the last moveSnake() - the real heading */
volatile int snakeLength = 3;
volatile int gameStatus = 0;  /* 0:Menu, 1:Playing, 2:Game Over, 3:Paused, 4:Help */
volatile int currentMap = 1;  /* 1 to 5 */
volatile int score = 0;
volatile int highScore = 0;
volatile uint8_t highScoreDirty = 0; /* set when highScore changes, cleared once saved to flash */
int snakeX[MAX_GAME_X * MAX_GAME_Y];
int snakeY[MAX_GAME_X * MAX_GAME_Y];
int tailX, tailY;

/* Multiple food pellets can be on the map at once, each with its own type,
   position and lifetime - so the player is never stuck waiting on a single
   pellet, and an uneaten pellet eventually expires and relocates on its own. */
#define MAX_FOOD 3
#define FOOD_LIFETIME_TICKS 40 /* ~10s at 250ms per game-task loop iteration */
int foodX[MAX_FOOD];
int foodY[MAX_FOOD];
volatile int foodType[MAX_FOOD];     /* 0 normal, 1 bonus(+5), 2 speed-boost, 3 shrink */
volatile uint8_t foodActive[MAX_FOOD];
volatile int foodTicksLeft[MAX_FOOD];
volatile int lastEatenFoodSlot = -1; /* set by checkCollision(), read right after */

/* Cells that must be blacked out because something vanished from them without
   the snake body or a new food happening to redraw over that cell this frame
   (shrink-food removing 2 segments at once, or a food pellet expiring). */
#define MAX_PENDING_CLEAR (MAX_FOOD + 2)
int pendingClearX[MAX_PENDING_CLEAR];
int pendingClearY[MAX_PENDING_CLEAR];
volatile int pendingClearCount = 0;

/* Difficulty presets (ms delay per game tick, lower = faster). Each level also
   has its own speed floor and ramp-up step, so they stay distinct all game
   long instead of converging to the same top speed after a bit of eating. */
#define DIFF_COUNT 3
static const int difficultySpeeds[DIFF_COUNT]    = {220, 150,  90}; /* starting speed */
static const int difficultyMinSpeeds[DIFF_COUNT] = {130,  70,  35}; /* speed floor */
static const int difficultyStepMs[DIFF_COUNT]    = {  3,   5,   8}; /* ms faster per food eaten */
static const char * const difficultyNames[DIFF_COUNT] = {"EASY  ", "NORMAL", "HARD  "};
volatile int difficulty = 1; /* 0:Easy, 1:Normal, 2:Hard */

#define COLOR_COUNT 5
#define COLOR_PINK 0xF81F
static const uint16_t snakeColors[COLOR_COUNT] = {COLOR_GREEN, COLOR_RED, COLOR_BLUE, COLOR_PINK, COLOR_WHITE};
static const char * const snakeColorNames[COLOR_COUNT] = {"GREEN", "RED  ", "BLUE ", "PINK ", "WHITE"};
volatile int currentColorIdx = 0;

volatile int speedBoostTicks = 0;
#define SPEED_BOOST_DURATION_TICKS 20
/* Boosted delay = gameSpeed * NUM/DEN (a proportional cut, not a flat ms
   subtraction) so the boost scales with whatever the current difficulty's
   speed happens to be, instead of a flat -60ms letting Easy's floor dip
   into Normal's speed range. */
#define SPEED_BOOST_FACTOR_NUM 7
#define SPEED_BOOST_FACTOR_DEN 10

osThreadId gameTaskHandle;
osThreadId displayTaskHandle;
osThreadId inputTaskHandle;
osMutexId gameMutexHandle;

typedef struct {
    uint16_t TouchDetected;
    uint16_t X;
    uint16_t Y;
    uint16_t Z;
} TS_StateTypeDef;
TS_StateTypeDef TS_State;

LCD_DrvTypeDef *LcdDrv;
TS_DrvTypeDef *TsDrv;
static uint16_t TsXBoundary, TsYBoundary;

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C3_Init(void);
static void MX_SPI5_Init(void);
static void MX_FMC_Init(void);
static void MX_LTDC_Init(void);
static void MX_ADC1_Init(void);
static void MX_ADC2_Init(void);
static void BSP_SDRAM_Initialization_Sequence(SDRAM_HandleTypeDef *hsdram, FMC_SDRAM_CommandTypeDef *Command);

/* Joystick helpers */
static uint16_t ADC_ReadChannel(ADC_HandleTypeDef *hadc);
static void JoystickCalibrateCenter(void);
static int ReadJoystickDirection(void);

/* High score persistence (internal Flash) */
static void Flash_LoadHighScore(void);
static void Flash_SaveHighScore(int value);

uint8_t BSP_TS_Init(uint16_t XSize, uint16_t YSize);
void    BSP_TS_GetState(TS_StateTypeDef* TsState);

/* FreeRTOS tasks */
void StartGameTask(void const * argument);
void StartDisplayTask(void const * argument);
void StartInputTask(void const * argument);

/* ---------- Direct framebuffer drawing helpers (RGB565) ---------- */
static inline void LCD_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
  if (x < LCD_W && y < LCD_H)
    *((volatile uint16_t*)(FRAME_BUFFER_ADDR + 2*(y * LCD_W + x))) = color;
}

static void LCD_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
  for (uint16_t j = y; j < y + h && j < LCD_H; j++)
    for (uint16_t i = x; i < x + w && i < LCD_W; i++)
      LCD_DrawPixel(i, j, color);
}

static void LCD_Clear(uint16_t color)
{
  volatile uint16_t *fb = (volatile uint16_t*)FRAME_BUFFER_ADDR;
  for (uint32_t i = 0; i < LCD_W * LCD_H; i++)
    fb[i] = color;
}

#define COLOR_GRAY      0x5AEB
#define COLOR_DARKGRAY  0x18C3
#define COLOR_BLUE      0x001F
#define COLOR_CYAN      0x07FF
#define COLOR_ORANGE    0xFD20

void LCD_DrawChar(uint16_t x, uint16_t y, char c, sFONT *font, uint16_t color, uint16_t bg_color)
{
  uint32_t i = 0, j = 0, char_index = 0;
  if (c < ' ' || c > '~') c = ' ';
  uint32_t num_bytes = (font->Width + 7) / 8;
  char_index = (c - ' ') * font->Height * num_bytes;
  
  for (i = 0; i < font->Height; i++)
  {
    uint32_t line = 0;
    for (j = 0; j < num_bytes; j++)
    {
      line = (line << 8) | font->table[char_index + i * num_bytes + j];
    }
    
    for (j = 0; j < font->Width; j++)
    {
      if (line & (1UL << (num_bytes * 8 - 1 - j)))
      {
        LCD_DrawPixel(x + j, y + i, color);
      }
      else if (bg_color != color)
      {
        LCD_DrawPixel(x + j, y + i, bg_color);
      }
    }
  }
}

void LCD_DrawString(uint16_t x, uint16_t y, const char *str, sFONT *font, uint16_t color, uint16_t bg_color)
{
  while (*str)
  {
    LCD_DrawChar(x, y, *str, font, color, bg_color);
    x += font->Width;
    str++;
  }
}

int isObstacle(uint8_t x, uint8_t y, uint8_t map_id)
{
  // Outer walls boundary
  if (x == 0 || x == MAX_GAME_X - 1 || y == 0 || y == MAX_GAME_Y - 1)
  {
    return 1;
  }
  
  if (map_id == 1) // Map 1: Empty (Classic)
  {
    return 0;
  }
  else if (map_id == 2) // Map 2: Center Box
  {
    // Solid square in the middle: rows/cols 8 to 15 (filled, not just the
    // outline - a hollow outline traps unreachable cells inside it, and food
    // could spawn there with no way for the snake to ever reach it)
    if (x >= 8 && x <= 15 && y >= 8 && y <= 15)
    {
      return 1;
    }
  }
  else if (map_id == 3) // Map 3: Four Corners
  {
    // Obstacles at corners
    if (((x >= 4 && x <= 6) || (x >= 17 && x <= 19)) &&
        (((y >= 4 && y <= 6) || (y >= 17 && y <= 19))))
    {
      return 1;
    }
  }
  else if (map_id == 4) // Map 4: Vertical Pillars
  {
    // Two vertical walls: col 6 and col 17, from row 5 to 18
    if ((x == 6 || x == 17) && (y >= 5 && y <= 18))
    {
      return 1;
    }
  }
  else if (map_id == 5) // Map 5: Cross/Maze
  {
    // A cross in the center
    if ((x == 11 && y >= 4 && y <= 19) || (y == 11 && x >= 4 && x <= 19))
    {
      return 1;
    }
  }
  return 0;
}

void drawMap(uint8_t map_id)
{
  LCD_FillRect(0, 0, 240, 240, COLOR_BLACK);
  
  for (uint16_t x = 0; x < MAX_GAME_X; x++)
  {
    for (uint16_t y = 0; y < MAX_GAME_Y; y++)
    {
      if (isObstacle(x, y, map_id))
      {
        uint16_t wallColor = (x == 0 || x == MAX_GAME_X-1 || y == 0 || y == MAX_GAME_Y-1) ? COLOR_GRAY : COLOR_ORANGE;
        LCD_FillRect(x * PIXEL_SIZE, y * PIXEL_SIZE, PIXEL_SIZE, PIXEL_SIZE, wallColor);
        for (int b = 0; b < PIXEL_SIZE; b++)
        {
          LCD_DrawPixel(x * PIXEL_SIZE + b, y * PIXEL_SIZE, COLOR_BLACK);
          LCD_DrawPixel(x * PIXEL_SIZE, y * PIXEL_SIZE + b, COLOR_BLACK);
        }
      }
    }
  }
}

void drawControlPanel(void)
{
  LCD_FillRect(0, 240, 240, 80, COLOR_DARKGRAY);
  
  for(int i=0; i<240; i++) {
    LCD_DrawPixel(i, 240, COLOR_WHITE);
  }
  
  // D-PAD Buttons (Up: 45-75, 245-265; Down: 45-75, 295-315; Left: 10-40, 270-290; Right: 80-110, 270-290)
  LCD_FillRect(45, 245, 30, 20, COLOR_GRAY);
  LCD_DrawString(56, 247, "^", &Font16, COLOR_WHITE, COLOR_GRAY);
  
  LCD_FillRect(45, 295, 30, 20, COLOR_GRAY);
  LCD_DrawString(56, 297, "v", &Font16, COLOR_WHITE, COLOR_GRAY);
  
  LCD_FillRect(10, 270, 30, 20, COLOR_GRAY);
  LCD_DrawString(20, 272, "<", &Font16, COLOR_WHITE, COLOR_GRAY);
  
  LCD_FillRect(80, 270, 30, 20, COLOR_GRAY);
  LCD_DrawString(91, 272, ">", &Font16, COLOR_WHITE, COLOR_GRAY);

  LCD_FillRect(45, 270, 30, 20, COLOR_GRAY); // Center core

  // INFO PANEL
  char scoreBuf[16];
  sprintf(scoreBuf, "%02d", score);
  LCD_DrawString(125, 248, "SCORE:", &Font12, COLOR_CYAN, COLOR_DARKGRAY);
  LCD_DrawString(180, 248, scoreBuf, &Font12, COLOR_WHITE, COLOR_DARKGRAY);
  
  char hiBuf[16];
  sprintf(hiBuf, "%02d", highScore);
  LCD_DrawString(125, 268, "HIGH :", &Font12, COLOR_YELLOW, COLOR_DARKGRAY);
  LCD_DrawString(180, 268, hiBuf, &Font12, COLOR_WHITE, COLOR_DARKGRAY);
  
  char mapBuf[8];
  sprintf(mapBuf, "%02d", currentMap);
  LCD_DrawString(125, 288, "MAP  :", &Font12, COLOR_GREEN, COLOR_DARKGRAY);
  LCD_DrawString(180, 288, mapBuf, &Font12, COLOR_WHITE, COLOR_DARKGRAY);
  
  // SPEED buttons
  LCD_FillRect(125, 303, 15, 15, COLOR_GRAY);
  LCD_DrawString(129, 305, "-", &Font12, COLOR_WHITE, COLOR_GRAY);
  LCD_FillRect(145, 303, 15, 15, COLOR_GRAY);
  LCD_DrawString(148, 305, "+", &Font12, COLOR_WHITE, COLOR_GRAY);
  char spdBuf[8];
  sprintf(spdBuf, "%d", gameSpeed);
  LCD_DrawString(165, 305, "SPD:", &Font12, COLOR_CYAN, COLOR_DARKGRAY);
  LCD_DrawString(195, 305, spdBuf, &Font12, COLOR_WHITE, COLOR_DARKGRAY);
}

void drawMenuScreen(void)
{
  LCD_FillRect(0, 0, 240, 240, COLOR_BLACK);
  
  LCD_DrawString(40, 50, "SNAKE GAME", &Font24, COLOR_YELLOW, COLOR_BLACK);
  LCD_DrawString(65, 105, "SELECT MAP", &Font16, COLOR_WHITE, COLOR_BLACK);
  
  char mapBuf[16];
  sprintf(mapBuf, "< MAP %d >", currentMap);
  LCD_DrawString(70, 130, mapBuf, &Font16, COLOR_GREEN, COLOR_BLACK);

  char diffBuf[24];
  sprintf(diffBuf, "SPEED: %s", difficultyNames[difficulty]);
  LCD_DrawString(60, 150, diffBuf, &Font12, COLOR_CYAN, COLOR_BLACK);

  char colBuf[24];
  sprintf(colBuf, "COLOR: %s", snakeColorNames[currentColorIdx]);
  LCD_DrawString(60, 162, colBuf, &Font12, COLOR_PINK, COLOR_BLACK);

  // Start Button: X: 70-170, Y: 170-200
  LCD_FillRect(70, 170, 100, 30, COLOR_CYAN);
  for(int b=0; b<100; b++) {
    LCD_DrawPixel(70 + b, 170, COLOR_WHITE);
    LCD_DrawPixel(70 + b, 200, COLOR_WHITE);
  }
  for(int b=0; b<30; b++) {
    LCD_DrawPixel(70, 170 + b, COLOR_WHITE);
    LCD_DrawPixel(170, 170 + b, COLOR_WHITE);
  }
  LCD_DrawString(92, 177, "START", &Font16, COLOR_BLACK, COLOR_CYAN);

  // Help Button: X: 70-170, Y: 207-232
  LCD_FillRect(70, 207, 100, 25, COLOR_GRAY);
  for(int b=0; b<100; b++) {
    LCD_DrawPixel(70 + b, 207, COLOR_WHITE);
    LCD_DrawPixel(70 + b, 232, COLOR_WHITE);
  }
  for(int b=0; b<25; b++) {
    LCD_DrawPixel(70, 207 + b, COLOR_WHITE);
    LCD_DrawPixel(170, 207 + b, COLOR_WHITE);
  }
  LCD_DrawString(95, 212, "HELP", &Font16, COLOR_WHITE, COLOR_GRAY);
}

void drawHelpScreen(void)
{
  LCD_FillRect(0, 0, 240, 320, COLOR_BLACK);

  LCD_DrawString(45, 10, "HOW TO PLAY", &Font20, COLOR_YELLOW, COLOR_BLACK);

  LCD_DrawString(10, 50,  "MOVE: Joystick/D-pad", &Font12, COLOR_WHITE, COLOR_BLACK);
  LCD_DrawString(10, 68,  "PA0 BUTTON:", &Font12, COLOR_CYAN, COLOR_BLACK);
  LCD_DrawString(10, 84,  "Start/Pause/Resume/Back", &Font12, COLOR_WHITE, COLOR_BLACK);
  LCD_DrawString(10, 110, "IN MENU:", &Font12, COLOR_CYAN, COLOR_BLACK);
  LCD_DrawString(10, 126, "L/R: Map   U/D: Difficulty", &Font12, COLOR_WHITE, COLOR_BLACK);

  LCD_DrawString(10, 152, "FOOD COLORS:", &Font12, COLOR_CYAN, COLOR_BLACK);

  LCD_FillRect(10, 172, 10, 10, COLOR_RED);
  LCD_DrawString(28, 171, "Normal", &Font12, COLOR_WHITE, COLOR_BLACK);

  LCD_FillRect(10, 190, 10, 10, COLOR_ORANGE);
  LCD_DrawString(28, 189, "Bonus +5 pts", &Font12, COLOR_WHITE, COLOR_BLACK);

  LCD_FillRect(10, 208, 10, 10, COLOR_CYAN);
  LCD_DrawString(28, 207, "Speed boost", &Font12, COLOR_WHITE, COLOR_BLACK);

  LCD_FillRect(10, 226, 10, 10, COLOR_BLUE);
  LCD_DrawString(28, 225, "Shrink snake", &Font12, COLOR_WHITE, COLOR_BLACK);

  LCD_DrawString(35, 270, "TAP TO GO BACK", &Font16, COLOR_YELLOW, COLOR_BLACK);
}

void drawPausedOverlay(void)
{
  LCD_FillRect(45, 100, 150, 40, COLOR_DARKGRAY);
  for (int b = 0; b < 150; b++) {
    LCD_DrawPixel(45 + b, 100, COLOR_WHITE);
    LCD_DrawPixel(45 + b, 140, COLOR_WHITE);
  }
  for (int b = 0; b < 40; b++) {
    LCD_DrawPixel(45, 100 + b, COLOR_WHITE);
    LCD_DrawPixel(195, 100 + b, COLOR_WHITE);
  }
  LCD_DrawString(75, 113, "PAUSED", &Font16, COLOR_YELLOW, COLOR_DARKGRAY);
}

void drawGameOverScreen(void)
{
  LCD_FillRect(0, 0, 240, 240, COLOR_BLACK);
  
  LCD_DrawString(52, 60, "GAME OVER", &Font24, COLOR_RED, COLOR_BLACK);
  
  char scoreBuf[32];
  sprintf(scoreBuf, "YOUR SCORE: %d", score);
  LCD_DrawString(60, 105, scoreBuf, &Font16, COLOR_WHITE, COLOR_BLACK);
  
  if (score >= highScore && score > 0)
  {
    LCD_DrawString(45, 130, "NEW HIGH SCORE!", &Font16, COLOR_YELLOW, COLOR_BLACK);
  }
  
  // Menu Button: X: 70-170, Y: 170-200
  LCD_FillRect(70, 170, 100, 30, COLOR_ORANGE);
  for(int b=0; b<100; b++) {
    LCD_DrawPixel(70 + b, 170, COLOR_WHITE);
    LCD_DrawPixel(70 + b, 200, COLOR_WHITE);
  }
  for(int b=0; b<30; b++) {
    LCD_DrawPixel(70, 170 + b, COLOR_WHITE);
    LCD_DrawPixel(170, 170 + b, COLOR_WHITE);
  }
  LCD_DrawString(98, 177, "MENU", &Font16, COLOR_BLACK, COLOR_ORANGE);
}

/* ---------- LCD IO & IOE (Bypassing BSP) ---------- */
void LCD_IO_Init(void) {
  GPIOC->BSRR = (1UL << 18); /* PC2 LOW */
  GPIOC->BSRR = (1UL << 2);  /* PC2 HIGH */
}
void LCD_IO_WriteData(uint16_t RegValue) {
  GPIOD->BSRR = (1UL << 13); /* PD13 HIGH */
  GPIOC->BSRR = (1UL << 18); /* PC2 LOW */
  HAL_SPI_Transmit(&hspi5, (uint8_t*)&RegValue, 1, 1000);
  GPIOC->BSRR = (1UL << 2);  /* PC2 HIGH */
}
void LCD_IO_WriteReg(uint8_t Reg) {
  GPIOD->BSRR = (1UL << 29); /* PD13 LOW */
  GPIOC->BSRR = (1UL << 18); /* PC2 LOW */
  uint16_t r = Reg;
  HAL_SPI_Transmit(&hspi5, (uint8_t*)&r, 1, 1000);
  GPIOC->BSRR = (1UL << 2);  /* PC2 HIGH */
}
uint32_t LCD_IO_ReadData(uint16_t RegValue, uint8_t ReadSize) {
  uint32_t readvalue = 0;
  GPIOC->BSRR = (1UL << 18);
  GPIOD->BSRR = (1UL << 29);
  HAL_SPI_Transmit(&hspi5, (uint8_t*)&RegValue, 1, 1000);
  HAL_SPI_Receive(&hspi5, (uint8_t*)&readvalue, ReadSize, 1000);
  GPIOD->BSRR = (1UL << 13);
  GPIOC->BSRR = (1UL << 2);
  return readvalue;
}
void LCD_Delay(uint32_t Delay) { HAL_Delay(Delay); }

void IOE_Init(void) {}
void IOE_ITConfig(void) {}
void IOE_Delay(uint32_t Delay) { HAL_Delay(Delay); }
void IOE_Write(uint8_t Addr, uint8_t Reg, uint8_t Value) {
  HAL_I2C_Mem_Write(&hi2c3, Addr, (uint16_t)Reg, I2C_MEMADD_SIZE_8BIT, &Value, 1, 0x1000);
}
uint8_t IOE_Read(uint8_t Addr, uint8_t Reg) {
  uint8_t value = 0;
  HAL_I2C_Mem_Read(&hi2c3, Addr, Reg, I2C_MEMADD_SIZE_8BIT, &value, 1, 0x1000);
  return value;
}
uint16_t IOE_ReadMultiple(uint8_t Addr, uint8_t Reg, uint8_t *pBuffer, uint16_t Length) {
  if(HAL_I2C_Mem_Read(&hi2c3, Addr, (uint16_t)Reg, I2C_MEMADD_SIZE_8BIT, pBuffer, Length, 0x1000) == HAL_OK) return 0;
  return 1;
}

/* ---------- Game logic ---------- */
static void QueueClear(int x, int y)
{
  if (pendingClearCount < MAX_PENDING_CLEAR) {
    pendingClearX[pendingClearCount] = x;
    pendingClearY[pendingClearCount] = y;
    pendingClearCount++;
  }
}

/* Spawns a new pellet into food slot `slot`, avoiding the snake body,
   obstacles, and every OTHER currently-active food slot. */
void spawnFoodAt(int slot)
{
  int ranX, ranY, flag;
  do {
    flag = 1;
    ranX = rand() % (MAX_GAME_X - 2) + 1;
    ranY = rand() % (MAX_GAME_Y - 2) + 1;
    for (int i = 0; i < snakeLength; i++) {
      if (ranX == snakeX[i] && ranY == snakeY[i]) { flag = 0; break; }
    }
    if (flag && isObstacle(ranX, ranY, currentMap)) {
      flag = 0;
    }
    if (flag) {
      for (int f = 0; f < MAX_FOOD; f++) {
        if (f != slot && foodActive[f] && foodX[f] == ranX && foodY[f] == ranY) {
          flag = 0;
          break;
        }
      }
    }
  } while (!flag);
  foodX[slot] = ranX;
  foodY[slot] = ranY;
  foodTicksLeft[slot] = FOOD_LIFETIME_TICKS;
  foodActive[slot] = 1;

  int r = rand() % 100;
  if (r < 12)      foodType[slot] = 1; /* 12%: bonus (+5 pts) */
  else if (r < 22) foodType[slot] = 2; /* 10%: speed boost (temporary) */
  else if (r < 30) foodType[slot] = 3; /*  8%: shrink (-2 length, min 3) */
  else             foodType[slot] = 0; /* 70%: normal */
}

void moveSnake(void)
{
  tailX = snakeX[snakeLength - 1];
  tailY = snakeY[snakeLength - 1];

  for (int i = snakeLength - 1; i > 0; i--) {
    snakeX[i] = snakeX[i-1];
    snakeY[i] = snakeY[i-1];
  }
  if (snakeDir == 0) snakeY[0] = (snakeY[0] - 1 + MAX_GAME_Y) % MAX_GAME_Y;
  else if (snakeDir == 1) snakeX[0] = (snakeX[0] + 1) % MAX_GAME_X;
  else if (snakeDir == 2) snakeY[0] = (snakeY[0] + 1) % MAX_GAME_Y;
  else if (snakeDir == 3) snakeX[0] = (snakeX[0] - 1 + MAX_GAME_X) % MAX_GAME_X;
  lastAppliedDir = snakeDir; /* direction actually reflected in the body layout */
}

void resetGame(void)
{
  snakeLength = 3;
  snakeDir = 1;
  lastAppliedDir = 1;
  for (int i = 0; i < snakeLength; i++) {
    snakeX[i] = 5 - i;
    snakeY[i] = 3;
  }
  score = 0;
  gameSpeed = difficultySpeeds[difficulty];
  speedBoostTicks = 0;
  pendingClearCount = 0;
  for (int f = 0; f < MAX_FOOD; f++) {
    foodActive[f] = 0;
  }
  for (int f = 0; f < MAX_FOOD; f++) {
    spawnFoodAt(f);
  }
  drawMap(currentMap);
  drawControlPanel();
}

int checkCollision(void)
{
  if (isObstacle(snakeX[0], snakeY[0], currentMap)) {
    return 0; /* Collided with wall */
  }
  for (int i = 1; i < snakeLength; i++) {
    if (snakeX[0] == snakeX[i] && snakeY[0] == snakeY[i]) return 0; /* Bit itself */
  }
  for (int f = 0; f < MAX_FOOD; f++) {
    if (foodActive[f] && snakeX[0] == foodX[f] && snakeY[0] == foodY[f]) {
      lastEatenFoodSlot = f;
      return 2; /* Ate food */
    }
  }
  return 1; /* Normal move */
}

/**
  * @brief  The application entry point.
  */
int main(void)
{
  HAL_Init();
  MX_GPIO_Init();

  SystemClock_Config();

  MX_I2C3_Init();
  MX_SPI5_Init();
  MX_FMC_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  JoystickCalibrateCenter();
  Flash_LoadHighScore();

  /* Initialize SDRAM */
  FMC_SDRAM_CommandTypeDef command;
  BSP_SDRAM_Initialization_Sequence(&hsdram1, &command);

  /* Initialize LCD via LTDC and ILI9341 */
  MX_LTDC_Init();

  /* Initialize touch screen */
  BSP_TS_Init(LCD_W, LCD_H);

  // Clear screen
  LCD_Clear(COLOR_BLACK);

  // Show startup menu screen
  drawMenuScreen();
  drawControlPanel();

  srand(HAL_GetTick());
  gameStatus = 0; /* Start in Menu state */

  /* Create Mutex */
  osMutexDef(gameMutex);
  gameMutexHandle = osMutexCreate(osMutex(gameMutex));

  /* Create Tasks */
  osThreadDef(gameTask, StartGameTask, osPriorityNormal, 0, 512);
  gameTaskHandle = osThreadCreate(osThread(gameTask), NULL);

  osThreadDef(displayTask, StartDisplayTask, osPriorityNormal, 0, 512);
  displayTaskHandle = osThreadCreate(osThread(displayTask), NULL);

  osThreadDef(inputTask, StartInputTask, osPriorityAboveNormal, 0, 512);
  inputTaskHandle = osThreadCreate(osThread(inputTask), NULL);

  osKernelStart();

  while (1) {}
}

/* ---------- FreeRTOS Tasks ---------- */
void StartGameTask(void const * argument)
{
  uint32_t lastTick = osKernelSysTick();
  for(;;)
  {
    HAL_GPIO_TogglePin(GPIOG, GPIO_PIN_14); /* Toggle Red LED for alive status */
    osDelay(250);
    
    if (gameStatus == 1) {
      /* Expire stale pellets so no single pellet is mandatory - one that
         sits uneaten for too long vanishes and relocates on its own. */
      osMutexWait(gameMutexHandle, osWaitForever);
      for (int f = 0; f < MAX_FOOD; f++) {
        if (foodActive[f]) {
          foodTicksLeft[f]--;
          if (foodTicksLeft[f] <= 0) {
            QueueClear(foodX[f], foodY[f]);
            foodActive[f] = 0;
            spawnFoodAt(f);
          }
        }
      }
      osMutexRelease(gameMutexHandle);

      int effectiveDelay = gameSpeed;
      if (speedBoostTicks > 0) {
        effectiveDelay = (gameSpeed * SPEED_BOOST_FACTOR_NUM) / SPEED_BOOST_FACTOR_DEN;
        if (effectiveDelay < 30) effectiveDelay = 30;
      }

      if (osKernelSysTick() - lastTick >= (uint32_t)effectiveDelay) {
        lastTick = osKernelSysTick();

        osMutexWait(gameMutexHandle, osWaitForever);
        moveSnake();
        if (speedBoostTicks > 0) speedBoostTicks--;
        int collision = checkCollision();
        uint8_t needSave = 0;
        if (collision == 0) {
          gameStatus = 2; /* Game over */
          if (highScoreDirty) { needSave = 1; highScoreDirty = 0; }
        } else if (collision == 2) {
          int slot = lastEatenFoodSlot;
          int eatenType = foodType[slot];
          switch (eatenType) {
            case 1: /* bonus */
              score += 5;
              snakeX[snakeLength] = tailX; snakeY[snakeLength] = tailY;
              snakeLength++;
              break;
            case 2: /* speed boost */
              score++;
              snakeX[snakeLength] = tailX; snakeY[snakeLength] = tailY;
              snakeLength++;
              speedBoostTicks = SPEED_BOOST_DURATION_TICKS;
              break;
            case 3: /* shrink */
              score++;
              {
                int newLen = snakeLength - 2;
                if (newLen < 3) newLen = 3;
                int removed = snakeLength - newLen;
                for (int k = 0; k < removed; k++) {
                  QueueClear(snakeX[snakeLength - 1 - k], snakeY[snakeLength - 1 - k]);
                }
                snakeLength = newLen;
              }
              break;
            default: /* normal */
              score++;
              snakeX[snakeLength] = tailX; snakeY[snakeLength] = tailY;
              snakeLength++;
              break;
          }
          if (snakeLength > MAX_GAME_X * MAX_GAME_Y) snakeLength = MAX_GAME_X * MAX_GAME_Y;
          if (score > highScore) {
            highScore = score;
            highScoreDirty = 1;
          }
          foodActive[slot] = 0;
          spawnFoodAt(slot);
          if (gameSpeed > difficultyMinSpeeds[difficulty]) {
            gameSpeed -= difficultyStepMs[difficulty];
            if (gameSpeed < difficultyMinSpeeds[difficulty]) gameSpeed = difficultyMinSpeeds[difficulty];
          }
        }
        osMutexRelease(gameMutexHandle);

        if (needSave) {
          Flash_SaveHighScore(highScore);
        }
      }
    }
  }
}

void StartDisplayTask(void const * argument)
{
  int lastStatus = -1;
  int lastScore = -1;
  int lastMap = -1;
  int lastDifficulty = -1;
  int lastColorIdx = -1;
  int lastGameSpeed = -1;

  for(;;)
  {
    osMutexWait(gameMutexHandle, osWaitForever);
    int currentStatus = gameStatus;
    osMutexRelease(gameMutexHandle);

    if (currentStatus != lastStatus)
    {
      lastStatus = currentStatus;
      if (currentStatus == 0) // Menu
      {
        drawMenuScreen();
        drawControlPanel();
        lastMap = currentMap;
        lastDifficulty = difficulty;
        lastColorIdx = currentColorIdx;
        requestJoyRecalibrate = 1; /* re-center joystick fresh each time Menu is shown */
      }
      else if (currentStatus == 2) // Game Over
      {
        drawGameOverScreen();
        drawControlPanel();
      }
      else if (currentStatus == 1) // Just started playing, or resumed from Pause
      {
        drawMap(currentMap);
        drawControlPanel();
        lastScore = score;
        lastGameSpeed = gameSpeed;
      }
      else if (currentStatus == 3) // Paused
      {
        drawPausedOverlay();
      }
      else if (currentStatus == 4) // Help
      {
        drawHelpScreen();
      }
    }
    
    if (currentStatus == 1)
    {
      osMutexWait(gameMutexHandle, osWaitForever);
      
      // Clear old tail
      if (tailX * PIXEL_SIZE < 240 && tailY * PIXEL_SIZE < 240)
      {
        LCD_FillRect(tailX * PIXEL_SIZE, tailY * PIXEL_SIZE, PIXEL_SIZE, PIXEL_SIZE, COLOR_BLACK);
      }

      // Clear cells vacated by shrink-food or an expired pellet - nothing
      // else will get redrawn over these this frame, so they need this
      // explicit blackout or a "ghost" square would linger on screen.
      for (int c = 0; c < pendingClearCount; c++)
      {
        LCD_FillRect(pendingClearX[c] * PIXEL_SIZE, pendingClearY[c] * PIXEL_SIZE, PIXEL_SIZE, PIXEL_SIZE, COLOR_BLACK);
      }
      pendingClearCount = 0;

      // Draw snake
      for (int i = 0; i < snakeLength; i++) {
        uint16_t color = (i == 0) ? COLOR_YELLOW : snakeColors[currentColorIdx];
        LCD_FillRect(snakeX[i] * PIXEL_SIZE, snakeY[i] * PIXEL_SIZE, PIXEL_SIZE, PIXEL_SIZE, color);
      }

      // Draw all active food pellets (color indicates type: normal/bonus/speed/shrink)
      for (int f = 0; f < MAX_FOOD; f++) {
        if (foodActive[f]) {
          uint16_t foodColor = (foodType[f] == 1) ? COLOR_ORANGE :
                               (foodType[f] == 2) ? COLOR_CYAN :
                               (foodType[f] == 3) ? COLOR_BLUE : COLOR_RED;
          LCD_FillRect(foodX[f] * PIXEL_SIZE, foodY[f] * PIXEL_SIZE, PIXEL_SIZE, PIXEL_SIZE, foodColor);
        }
      }
      
      // Update score if it changed
      if (score != lastScore)
      {
        lastScore = score;
        char scoreBuf[16];
        sprintf(scoreBuf, "%02d", score);
        LCD_DrawString(180, 248, scoreBuf, &Font12, COLOR_WHITE, COLOR_DARKGRAY);
      }

      // Update speed if it changed
      if (gameSpeed != lastGameSpeed)
      {
        lastGameSpeed = gameSpeed;
        char spdBuf[8];
        sprintf(spdBuf, "%-3d", gameSpeed); // pad with spaces in case it goes from 100 to 90
        LCD_DrawString(195, 305, spdBuf, &Font12, COLOR_WHITE, COLOR_DARKGRAY);
      }
      
      osMutexRelease(gameMutexHandle);
    }
    else if (currentStatus == 0)
    {
      // In menu: update map number if it changed
      if (currentMap != lastMap)
      {
        lastMap = currentMap;
        char mapBuf[16];
        sprintf(mapBuf, "< MAP %d >", currentMap);
        LCD_DrawString(70, 130, mapBuf, &Font16, COLOR_GREEN, COLOR_BLACK);
        
        // Also update control panel map number
        sprintf(mapBuf, "%02d", currentMap);
        LCD_DrawString(180, 288, mapBuf, &Font12, COLOR_WHITE, COLOR_DARKGRAY);
      }
      if (difficulty != lastDifficulty)
      {
        lastDifficulty = difficulty;
        char diffBuf[24];
        sprintf(diffBuf, "SPEED: %s", difficultyNames[difficulty]);
        LCD_DrawString(60, 150, diffBuf, &Font12, COLOR_CYAN, COLOR_BLACK);
      }
      if (currentColorIdx != lastColorIdx)
      {
        lastColorIdx = currentColorIdx;
        char colBuf[24];
        sprintf(colBuf, "COLOR: %s", snakeColorNames[currentColorIdx]);
        LCD_DrawString(60, 162, colBuf, &Font12, COLOR_PINK, COLOR_BLACK);
      }
    }
    
    osDelay(33);
  }
}

void StartInputTask(void const * argument)
{
  uint8_t lastButtonState = 0;

  for(;;)
  {
    if (requestJoyRecalibrate)
    {
      requestJoyRecalibrate = 0;
      JoystickCalibrateCenter();
    }

    BSP_TS_GetState(&TS_State);
    uint8_t inputDetected = 0;
    uint16_t x = 0, y = 0;
    if (TS_State.TouchDetected) {
      x = TS_State.X;
      y = TS_State.Y;
      inputDetected = 1;
    }

    int joyDir = ReadJoystickDirection();
    uint8_t buttonState = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_SET);
    uint8_t buttonPressed = (buttonState && !lastButtonState); /* rising edge */
    lastButtonState = buttonState;

    if (gameStatus == 1) // Playing state
    {
      osMutexWait(gameMutexHandle, osWaitForever);
      if (inputDetected && y >= 240)
      {
        if (x >= 35 && x <= 75 && y >= 240 && y <= 268) {
          if (lastAppliedDir != 2) snakeDir = 0;
        }
        else if (x >= 35 && x <= 75 && y >= 292 && y <= 320) {
          if (lastAppliedDir != 0) snakeDir = 2;
        }
        else if (x >= 10 && x <= 38 && y >= 265 && y <= 295) {
          if (lastAppliedDir != 1) snakeDir = 3;
        }
        else if (x >= 72 && x <= 100 && y >= 265 && y <= 295) {
          if (lastAppliedDir != 3) snakeDir = 1;
        }
        else if (x >= 120 && x <= 140 && y >= 300 && y <= 320) {
          static uint32_t lastSpeedDec = 0;
          if (HAL_GetTick() - lastSpeedDec > 150) {
            if (gameSpeed < 500) gameSpeed += 10;
            lastSpeedDec = HAL_GetTick();
          }
        }
        else if (x >= 140 && x <= 160 && y >= 300 && y <= 320) {
          static uint32_t lastSpeedInc = 0;
          if (HAL_GetTick() - lastSpeedInc > 150) {
            if (gameSpeed > 20) gameSpeed -= 10;
            lastSpeedInc = HAL_GetTick();
          }
        }
      }
      // Joystick direction (Up/Right/Down/Left), blocked from reversing onto the
      // snake's actual current heading (not the possibly-already-changed snakeDir,
      // which could be flipped twice within one movement tick otherwise)
      if (joyDir == 0 && lastAppliedDir != 2) snakeDir = 0;
      else if (joyDir == 1 && lastAppliedDir != 3) snakeDir = 1;
      else if (joyDir == 2 && lastAppliedDir != 0) snakeDir = 2;
      else if (joyDir == 3 && lastAppliedDir != 1) snakeDir = 3;
      if (buttonPressed) gameStatus = 3; /* Pause */
      osMutexRelease(gameMutexHandle);
      if (buttonPressed) osDelay(200);
    }
    else if (gameStatus == 3) // Paused state
    {
      if (buttonPressed)
      {
        osMutexWait(gameMutexHandle, osWaitForever);
        gameStatus = 1; /* Resume */
        osMutexRelease(gameMutexHandle);
        osDelay(200);
      }
    }
    else if (gameStatus == 0) // Menu state
    {
      if (inputDetected)
      {
        // Touch on START button
        if (x >= 70 && x <= 170 && y >= 170 && y <= 200)
        {
          osMutexWait(gameMutexHandle, osWaitForever);
          resetGame();
          gameStatus = 1;
          osMutexRelease(gameMutexHandle);
        }
        // Touch on D-pad LEFT to cycle map down
        else if (x >= 10 && x <= 38 && y >= 265 && y <= 295)
        {
          osMutexWait(gameMutexHandle, osWaitForever);
          currentMap--;
          if (currentMap < 1) currentMap = 5;
          osMutexRelease(gameMutexHandle);
          osDelay(200);
        }
        // Touch on D-pad RIGHT to cycle map up
        else if (x >= 72 && x <= 100 && y >= 265 && y <= 295)
        {
          osMutexWait(gameMutexHandle, osWaitForever);
          currentMap++;
          if (currentMap > 5) currentMap = 1;
          osMutexRelease(gameMutexHandle);
          osDelay(200);
        }
        // Touch on D-pad UP to increase difficulty
        else if (x >= 35 && x <= 75 && y >= 240 && y <= 268)
        {
          osMutexWait(gameMutexHandle, osWaitForever);
          if (difficulty < DIFF_COUNT - 1) difficulty++;
          osMutexRelease(gameMutexHandle);
          osDelay(200);
        }
        // Touch on D-pad DOWN to decrease difficulty
        else if (x >= 35 && x <= 75 && y >= 292 && y <= 320)
        {
          osMutexWait(gameMutexHandle, osWaitForever);
          if (difficulty > 0) difficulty--;
          osMutexRelease(gameMutexHandle);
          osDelay(200);
        }
        // Touch on HELP button
        else if (x >= 70 && x <= 170 && y >= 207 && y <= 232)
        {
          osMutexWait(gameMutexHandle, osWaitForever);
          gameStatus = 4;
          osMutexRelease(gameMutexHandle);
        }
        // Touch on COLOR text
        else if (x >= 40 && x <= 200 && y >= 155 && y <= 175)
        {
          osMutexWait(gameMutexHandle, osWaitForever);
          currentColorIdx = (currentColorIdx + 1) % COLOR_COUNT;
          osMutexRelease(gameMutexHandle);
          osDelay(200);
        }
      }
      // Joystick Left/Right cycles map, Up/Down cycles difficulty, button (PA0) starts the game
      if (joyDir == 3)
      {
        osMutexWait(gameMutexHandle, osWaitForever);
        currentMap--;
        if (currentMap < 1) currentMap = 5;
        osMutexRelease(gameMutexHandle);
        osDelay(200);
      }
      else if (joyDir == 1)
      {
        osMutexWait(gameMutexHandle, osWaitForever);
        currentMap++;
        if (currentMap > 5) currentMap = 1;
        osMutexRelease(gameMutexHandle);
        osDelay(200);
      }
      else if (joyDir == 0)
      {
        osMutexWait(gameMutexHandle, osWaitForever);
        if (difficulty < DIFF_COUNT - 1) difficulty++;
        osMutexRelease(gameMutexHandle);
        osDelay(200);
      }
      else if (joyDir == 2)
      {
        osMutexWait(gameMutexHandle, osWaitForever);
        if (difficulty > 0) difficulty--;
        osMutexRelease(gameMutexHandle);
        osDelay(200);
      }
      else if (buttonPressed)
      {
        osMutexWait(gameMutexHandle, osWaitForever);
        resetGame();
        gameStatus = 1;
        osMutexRelease(gameMutexHandle);
      }
    }
    else if (gameStatus == 2) // Game Over state
    {
      if (inputDetected)
      {
        // Touch on MENU button
        if (x >= 70 && x <= 170 && y >= 170 && y <= 200)
        {
          osMutexWait(gameMutexHandle, osWaitForever);
          gameStatus = 0;
          osMutexRelease(gameMutexHandle);
          osDelay(200);
        }
      }
      // Joystick button (PA0) returns to menu
      if (buttonPressed)
      {
        osMutexWait(gameMutexHandle, osWaitForever);
        gameStatus = 0;
        osMutexRelease(gameMutexHandle);
        osDelay(200);
      }
    }
    else if (gameStatus == 4) // Help screen
    {
      // Any tap or the joystick button returns to Menu
      if (inputDetected || buttonPressed)
      {
        osMutexWait(gameMutexHandle, osWaitForever);
        gameStatus = 0;
        osMutexRelease(gameMutexHandle);
        osDelay(200);
      }
    }
    osDelay(20);
  }
}

/* ---------- System Clock ---------- */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 360;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_PWREx_ActivateOverDrive() != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* ---------- Peripheral Inits ---------- */
static void MX_I2C3_Init(void)
{
  hi2c3.Instance = I2C3;
  hi2c3.Init.ClockSpeed = 100000;
  hi2c3.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c3.Init.OwnAddress1 = 0;
  hi2c3.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  HAL_I2C_Init(&hi2c3);
}

static void MX_SPI5_Init(void)
{
  hspi5.Instance = SPI5;
  hspi5.Init.Mode = SPI_MODE_MASTER;
  hspi5.Init.Direction = SPI_DIRECTION_2LINES;
  hspi5.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi5.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi5.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi5.Init.NSS = SPI_NSS_SOFT;
  hspi5.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
  hspi5.Init.FirstBit = SPI_FIRSTBIT_MSB;
  HAL_SPI_Init(&hspi5);
}

static void MX_LTDC_Init(void)
{
  LTDC_LayerCfgTypeDef pLayerCfg = {0};
  hltdc.Instance = LTDC;
  hltdc.Init.HSPolarity = LTDC_HSPOLARITY_AL;
  hltdc.Init.VSPolarity = LTDC_VSPOLARITY_AL;
  hltdc.Init.DEPolarity = LTDC_DEPOLARITY_AL;
  hltdc.Init.PCPolarity = LTDC_PCPOLARITY_IPC;
  hltdc.Init.HorizontalSync = 9;
  hltdc.Init.VerticalSync = 1;
  hltdc.Init.AccumulatedHBP = 29;
  hltdc.Init.AccumulatedVBP = 3;
  hltdc.Init.AccumulatedActiveW = 269;
  hltdc.Init.AccumulatedActiveH = 323;
  hltdc.Init.TotalWidth = 279;
  hltdc.Init.TotalHeigh = 327;
  hltdc.Init.Backcolor.Blue = 0;
  hltdc.Init.Backcolor.Green = 0;
  hltdc.Init.Backcolor.Red = 0;
  HAL_LTDC_Init(&hltdc);

  /* Configure Layer 0 with RGB565 */
  pLayerCfg.WindowX0 = 0;
  pLayerCfg.WindowX1 = LCD_W - 1;
  pLayerCfg.WindowY0 = 0;
  pLayerCfg.WindowY1 = LCD_H - 1;
  pLayerCfg.PixelFormat = LTDC_PIXEL_FORMAT_RGB565;
  pLayerCfg.Alpha = 255;
  pLayerCfg.Alpha0 = 0;
  pLayerCfg.BlendingFactor1 = LTDC_BLENDING_FACTOR1_CA;
  pLayerCfg.BlendingFactor2 = LTDC_BLENDING_FACTOR2_CA;
  pLayerCfg.FBStartAdress = FRAME_BUFFER_ADDR;
  pLayerCfg.ImageWidth = LCD_W;
  pLayerCfg.ImageHeight = LCD_H;
  HAL_LTDC_ConfigLayer(&hltdc, &pLayerCfg, 0);

  LcdDrv = &ili9341_drv;
  LcdDrv->Init();
  LcdDrv->DisplayOn();
}

static void MX_ADC1_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};

  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  sConfig.Channel = ADC_CHANNEL_13; /* PC3 -> joystick Y (VRy) */
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_15CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_ADC2_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};

  hadc2.Instance = ADC2;
  hadc2.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc2.Init.Resolution = ADC_RESOLUTION_12B;
  hadc2.Init.ScanConvMode = DISABLE;
  hadc2.Init.ContinuousConvMode = DISABLE;
  hadc2.Init.DiscontinuousConvMode = DISABLE;
  hadc2.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc2.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc2.Init.NbrOfConversion = 1;
  hadc2.Init.DMAContinuousRequests = DISABLE;
  hadc2.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc2) != HAL_OK)
  {
    Error_Handler();
  }

  sConfig.Channel = ADC_CHANNEL_5; /* PA5 -> joystick X (VRx) */
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_15CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_FMC_Init(void)
{
  FMC_SDRAM_TimingTypeDef SdramTiming = {0};
  hsdram1.Instance = FMC_SDRAM_DEVICE;
  hsdram1.Init.SDBank = FMC_SDRAM_BANK2;
  hsdram1.Init.ColumnBitsNumber = FMC_SDRAM_COLUMN_BITS_NUM_8;
  hsdram1.Init.RowBitsNumber = FMC_SDRAM_ROW_BITS_NUM_12;
  hsdram1.Init.MemoryDataWidth = FMC_SDRAM_MEM_BUS_WIDTH_16;
  hsdram1.Init.InternalBankNumber = FMC_SDRAM_INTERN_BANKS_NUM_4;
  hsdram1.Init.CASLatency = FMC_SDRAM_CAS_LATENCY_3;
  hsdram1.Init.WriteProtection = FMC_SDRAM_WRITE_PROTECTION_DISABLE;
  hsdram1.Init.SDClockPeriod = FMC_SDRAM_CLOCK_PERIOD_2;
  hsdram1.Init.ReadBurst = FMC_SDRAM_RBURST_DISABLE;
  hsdram1.Init.ReadPipeDelay = FMC_SDRAM_RPIPE_DELAY_1;

  SdramTiming.LoadToActiveDelay = 2;
  SdramTiming.ExitSelfRefreshDelay = 7;
  SdramTiming.SelfRefreshTime = 4;
  SdramTiming.RowCycleDelay = 7;
  SdramTiming.WriteRecoveryTime = 3;
  SdramTiming.RPDelay = 2;
  SdramTiming.RCDDelay = 2;
  HAL_SDRAM_Init(&hsdram1, &SdramTiming);
}

static void BSP_SDRAM_Initialization_Sequence(SDRAM_HandleTypeDef *hsdram, FMC_SDRAM_CommandTypeDef *Command)
{
  Command->CommandMode             = FMC_SDRAM_CMD_CLK_ENABLE;
  Command->CommandTarget           = FMC_SDRAM_CMD_TARGET_BANK2;
  Command->AutoRefreshNumber       = 1;
  Command->ModeRegisterDefinition  = 0;
  HAL_SDRAM_SendCommand(hsdram, Command, 0x1000);
  HAL_Delay(1);

  Command->CommandMode             = FMC_SDRAM_CMD_PALL;
  Command->CommandTarget           = FMC_SDRAM_CMD_TARGET_BANK2;
  Command->AutoRefreshNumber       = 1;
  Command->ModeRegisterDefinition  = 0;
  HAL_SDRAM_SendCommand(hsdram, Command, 0x1000);

  Command->CommandMode             = FMC_SDRAM_CMD_AUTOREFRESH_MODE;
  Command->CommandTarget           = FMC_SDRAM_CMD_TARGET_BANK2;
  Command->AutoRefreshNumber       = 4;
  Command->ModeRegisterDefinition  = 0;
  HAL_SDRAM_SendCommand(hsdram, Command, 0x1000);

  uint32_t tmpmrd = (uint32_t)0x0230;
  Command->CommandMode             = FMC_SDRAM_CMD_LOAD_MODE;
  Command->CommandTarget           = FMC_SDRAM_CMD_TARGET_BANK2;
  Command->AutoRefreshNumber       = 1;
  Command->ModeRegisterDefinition  = tmpmrd;
  HAL_SDRAM_SendCommand(hsdram, Command, 0x1000);
  HAL_SDRAM_ProgramRefreshRate(hsdram, 683); /* REFRESH_COUNT */
}

/* ---------- Touch Screen (from SimpleRacing) ---------- */
uint8_t BSP_TS_Init(uint16_t XSize, uint16_t YSize)
{
  uint8_t ret = 1; /* ERROR */
  TsXBoundary = XSize;
  TsYBoundary = YSize;

  if (stmpe811_ts_drv.ReadID(TS_I2C_ADDRESS) == STMPE811_ID)
  {
      TsDrv = &stmpe811_ts_drv;
      ret = 0; /* OK */
  }

  if (ret == 0)
  {
      TsDrv->Init(TS_I2C_ADDRESS);
      TsDrv->Start(TS_I2C_ADDRESS);
  }
  return ret;
}

void BSP_TS_GetState(TS_StateTypeDef* TsState)
{
  static uint32_t _x = 0, _y = 0;
  uint16_t xDiff, yDiff, x, y, xr, yr;

  if (TsDrv == NULL)
  {
      TsState->TouchDetected = 0;
      return;
  }

  TsState->TouchDetected = TsDrv->DetectTouch(TS_I2C_ADDRESS);

  if (TsState->TouchDetected)
  {
      TsDrv->GetXY(TS_I2C_ADDRESS, &x, &y);

      if (y > 3700) y = 3700;
      else if (y < 180) y = 180;

      y -= 180;
      y = 3520 - y;
      yr = y / 11;

      if (yr <= 0) yr = 0;
      else if (yr > TsYBoundary) yr = TsYBoundary - 1;
      y = yr;

      if (x <= 3000) x = 3870 - x;
      else x = 3800 - x;

      xr = x / 15;

      if (xr <= 0) xr = 0;
      else if (xr > TsXBoundary) xr = TsXBoundary - 1;

      x = xr;
      xDiff = x > _x ? (x - _x) : (_x - x);
      yDiff = y > _y ? (y - _y) : (_y - y);

      if (xDiff + yDiff > 5)
      {
          _x = x;
          _y = y;
      }

      TsState->X = _x;
      TsState->Y = _y;
  }
}

/* ---------- High score persistence (internal Flash, Bank2 Sector 12) ----------
   Sector 12 is a 16KB sector at the start of Bank2 (0x08100000), well outside
   the program's own Bank1 code/data, so it's safe to erase/rewrite. */
#define HS_FLASH_ADDR   0x08100000U
#define HS_FLASH_SECTOR FLASH_SECTOR_12
#define HS_FLASH_MAGIC  0x534E414BU /* 'SNAK' */

typedef struct {
  uint32_t magic;
  uint32_t highScore;
} HighScoreRecord;

static void Flash_LoadHighScore(void)
{
  const HighScoreRecord *rec = (const HighScoreRecord *)HS_FLASH_ADDR;
  highScore = (rec->magic == HS_FLASH_MAGIC) ? (int)rec->highScore : 0;
}

static void Flash_SaveHighScore(int value)
{
  FLASH_EraseInitTypeDef eraseInit;
  uint32_t sectorError = 0;

  HAL_FLASH_Unlock();

  eraseInit.TypeErase = FLASH_TYPEERASE_SECTORS;
  eraseInit.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  eraseInit.Sector = HS_FLASH_SECTOR;
  eraseInit.NbSectors = 1;
  eraseInit.Banks = FLASH_BANK_2;
  HAL_FLASHEx_Erase(&eraseInit, &sectorError);

  HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, HS_FLASH_ADDR, HS_FLASH_MAGIC);
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, HS_FLASH_ADDR + 4, (uint32_t)value);

  HAL_FLASH_Lock();
}

/* ---------- Joystick (analog VRx/VRy + onboard PA0 button) ---------- */
static uint16_t ADC_ReadChannel(ADC_HandleTypeDef *hadc)
{
  uint16_t value = 0;
  HAL_ADC_Start(hadc);
  if (HAL_ADC_PollForConversion(hadc, 10) == HAL_OK)
  {
    value = (uint16_t)HAL_ADC_GetValue(hadc);
  }
  HAL_ADC_Stop(hadc);
  return value;
}

/* Capture the resting (centered) stick position so mechanical offset
   between units doesn't bias direction detection. */
static void JoystickCalibrateCenter(void)
{
  joyXCenter = ADC_ReadChannel(&hadc2);
  joyYCenter = ADC_ReadChannel(&hadc1);
}

/* Returns 0:Up, 1:Right, 2:Down, 3:Left, -1:Neutral (within deadzone) */
static int ReadJoystickDirection(void)
{
  int16_t dx = (int16_t)ADC_ReadChannel(&hadc2) - (int16_t)joyXCenter;
  int16_t dy = (int16_t)ADC_ReadChannel(&hadc1) - (int16_t)joyYCenter;

  if (dx > JOY_DEADZONE)       return 1; /* Right */
  else if (dx < -JOY_DEADZONE) return 3; /* Left  */
  else if (dy > JOY_DEADZONE)  return 2; /* Down  */
  else if (dy < -JOY_DEADZONE) return 0; /* Up    */
  return -1;
}

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /* LCD NCS (PC2) */
  GPIOC->BSRR = (1UL << 2);
  GPIO_InitStruct.Pin = GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /* LCD WRX (PD13), RDX (PD12) */
  GPIOD->BSRR = (1UL << 12) | (1UL << 13);
  GPIO_InitStruct.Pin = GPIO_PIN_12 | GPIO_PIN_13;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /* LED (PG14) */
  GPIO_InitStruct.Pin = GPIO_PIN_14;
  HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

  /* Joystick VRx (PA5)/VRy (PC3) analog GPIO is configured by HAL_ADC_MspInit()
     (auto-generated in stm32f4xx_hal_msp.c) when MX_ADC1_Init/MX_ADC2_Init run. */

  /* Joystick Select button - reuse onboard User button (PA0) */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

void Error_Handler(void)
{
  __disable_irq();
  while (1) {
    HAL_GPIO_TogglePin(GPIOG, GPIO_PIN_14);
    for(volatile uint32_t i = 0; i < 500000; i++);
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif


