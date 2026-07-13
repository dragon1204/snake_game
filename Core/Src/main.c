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

/* Private define ------------------------------------------------------------*/
#define LCD_W 240
#define LCD_H 320
#define PIXEL_SIZE 10
#define MAX_GAME_X (LCD_W / PIXEL_SIZE)   /* 24 */
#define MAX_GAME_Y (LCD_H / PIXEL_SIZE)   /* 32 */
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

/* Game state */
volatile int gameSpeed = 150;
volatile int snakeDir = 1;    /* 0:Up, 1:Right, 2:Down, 3:Left */
volatile int snakeLength = 3;
volatile int gameStatus = 0;  /* 0:waiting, 1:playing */
int snakeX[MAX_GAME_X * MAX_GAME_Y];
int snakeY[MAX_GAME_X * MAX_GAME_Y];
int foodX, foodY;
int tailX, tailY;

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
static void BSP_SDRAM_Initialization_Sequence(SDRAM_HandleTypeDef *hsdram, FMC_SDRAM_CommandTypeDef *Command);

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
void spawnFood(void)
{
  int ranX, ranY, flag;
  do {
    flag = 1;
    ranX = rand() % MAX_GAME_X;
    ranY = rand() % MAX_GAME_Y;
    for (int i = 0; i < snakeLength; i++) {
      if (ranX == snakeX[i] && ranY == snakeY[i]) { flag = 0; break; }
    }
  } while (!flag);
  foodX = ranX;
  foodY = ranY;
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
}

void resetGame(void)
{
  snakeLength = 3;
  snakeDir = 1;
  for (int i = 0; i < snakeLength; i++) {
    snakeX[i] = 12 - i;
    snakeY[i] = 16;
  }
  spawnFood();
  LCD_Clear(COLOR_BLACK);
}

int checkCollision(void)
{
  if (snakeX[0] == foodX && snakeY[0] == foodY) return 2; /* Ate food */
  for (int i = 1; i < snakeLength; i++) {
    if (snakeX[0] == snakeX[i] && snakeY[0] == snakeY[i]) return 0; /* Bit itself */
  }
  return 1; /* Normal move */
}

/**
  * @brief  The application entry point.
  */
int main(void)
{
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_I2C3_Init();
  MX_SPI5_Init();
  MX_FMC_Init();

  /* Initialize SDRAM */
  FMC_SDRAM_CommandTypeDef command;
  BSP_SDRAM_Initialization_Sequence(&hsdram1, &command);

  /* Initialize LCD via LTDC and ILI9341 */
  MX_LTDC_Init();

  /* Initialize touch screen */
  BSP_TS_Init(LCD_W, LCD_H);

  /* Clear screen to black initially */
  LCD_Clear(COLOR_BLACK);

  /* Draw a simple "SNAKE" border */
  for (int i = 0; i < LCD_W; i++) { LCD_DrawPixel(i, 0, COLOR_GREEN); LCD_DrawPixel(i, LCD_H-1, COLOR_GREEN); }
  for (int i = 0; i < LCD_H; i++) { LCD_DrawPixel(0, i, COLOR_GREEN); LCD_DrawPixel(LCD_W-1, i, COLOR_GREEN); }

  HAL_Delay(1000);

  srand(HAL_GetTick());
  resetGame();
  gameStatus = 1;

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
      if (osKernelSysTick() - lastTick >= gameSpeed) {
        lastTick = osKernelSysTick();
        
        osMutexWait(gameMutexHandle, osWaitForever);
        moveSnake();
        int collision = checkCollision();
        if (collision == 0) {
          gameStatus = 0; /* Game over */
        } else if (collision == 2) {
          snakeLength++;
          if (snakeLength > MAX_GAME_X * MAX_GAME_Y) snakeLength = MAX_GAME_X * MAX_GAME_Y;
          spawnFood();
          if (gameSpeed > 50) gameSpeed -= 5;
        }
        osMutexRelease(gameMutexHandle);
      }
    }
  }
}

void StartDisplayTask(void const * argument)
{
  for(;;)
  {
    if (gameStatus != 1) {
      osDelay(50);
      continue;
    }

    osMutexWait(gameMutexHandle, osWaitForever);

    /* Anti-Flickering: clear old tail */
    LCD_FillRect(tailX * PIXEL_SIZE, tailY * PIXEL_SIZE, PIXEL_SIZE, PIXEL_SIZE, COLOR_BLACK);

    /* Draw snake */
    for (int i = 0; i < snakeLength; i++) {
      uint16_t color = (i == 0) ? COLOR_YELLOW : COLOR_GREEN;
      LCD_FillRect(snakeX[i] * PIXEL_SIZE, snakeY[i] * PIXEL_SIZE, PIXEL_SIZE, PIXEL_SIZE, color);
    }

    /* Draw food */
    LCD_FillRect(foodX * PIXEL_SIZE, foodY * PIXEL_SIZE, PIXEL_SIZE, PIXEL_SIZE, COLOR_RED);

    osMutexRelease(gameMutexHandle);

    osDelay(33);
  }
}

void StartInputTask(void const * argument)
{
  for(;;)
  {
    BSP_TS_GetState(&TS_State);
    if (TS_State.TouchDetected) {
      uint16_t x = TS_State.X;
      uint16_t y = TS_State.Y;

      int dx = (int)x - (LCD_W / 2);
      int dy = (int)y - (LCD_H / 2);

      osMutexWait(gameMutexHandle, osWaitForever);
      if (abs(dx) > abs(dy)) {
        if (dx > 0 && snakeDir != 3) snakeDir = 1;
        else if (dx < 0 && snakeDir != 1) snakeDir = 3;
      } else {
        if (dy > 0 && snakeDir != 0) snakeDir = 2;
        else if (dy < 0 && snakeDir != 2) snakeDir = 0;
      }
      osMutexRelease(gameMutexHandle);

      osDelay(200);
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

  HAL_PWREx_EnableOverDrive();

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
  pLayerCfg.WindowX1 = LCD_W;
  pLayerCfg.WindowY0 = 0;
  pLayerCfg.WindowY1 = LCD_H;
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
