#include <Arduino.h>
#include <ModbusRTU.h>
#include <SensirionI2cScd4x.h>
#include <DFRobot_ENS160.h>
#include <LiquidCrystal_I2C.h>

#define LCD_LED_PIN 27
#define PUMP_PIN 32
#define BUTTON_PIN 33
#define GATEWAY_TIMEOUT 360000
#define NO_ERROR 0
#define SLAVE_ID 4
#define REQUEST_DATA_REGN 8
#define DATA_READY_REGN 9
#define TEMP_REGN 10
#define HUMI_REGN 11
#define CO2_REGN 12
#define AQI_REGN 13
#define TVOC_REGN 14
#define STATUS_SCD41_REGN 15
#define STATUS_ENS160_REGN 16
// Các thanh ghi STATUS biểu thị việc đọc cảm biến có thành công hay 0. 0 là thành công và ngc lại. Nếu việc khởi tạo thất bại thì chắc chắn luôn đọc lỗi nên thanh ghi luôn = 1.

SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t currentScreenMutex;

ModbusRTU mb;
HardwareSerial RS485Serial(2);

static SensirionI2cScd4x sensor;
static char errorMessage[64];
static int16_t error;

DFRobot_ENS160_I2C ens160(&Wire, 0x53);

enum LCDScreen {
    SCREEN_WELCOME = 0, 
    SCREEN_SYSTEM_STARTING,
    SCREEN_WAITING_REQUEST,
    SCREEN_PREPARE_DATA,
    SCREEN_TEMP_HUMI,
    SCREEN_CO2_TVOC,
    SCREEN_AQI,
    SCREEN_GATEWAY_OFFLINE
};

uint8_t currentScreen = SCREEN_WELCOME;

LiquidCrystal_I2C lcd(0x27, 16, 2);

bool isLCDOn = false;
uint8_t lastButtonState = LOW;

bool ens160Initialized = false;
bool errSCD41 = false;
float temperature = 0;
float humidity = 0;
uint16_t co2 = 0;
uint8_t aqi = 0;
uint16_t tvoc = 0;

void SCD41_Init();
bool SCD41_ReadData(uint16_t &co2, float &temperature, float &humidity);
bool ENS160_Init();
void ENS160_ReadData(uint8_t &aqi, uint16_t &tvoc, float temperature, float humidity);
void LCD_ShowScreen(uint8_t currentScreen);
void readSensors();
void Task_SystemMonitor(void *pvParameters);
void Task_Button(void *pvParameters);

void setup()
{
    Serial.begin(115200);
    pinMode(LCD_LED_PIN, OUTPUT);
    digitalWrite(LCD_LED_PIN, HIGH);
    pinMode(PUMP_PIN, OUTPUT);
    digitalWrite(PUMP_PIN, HIGH);
    pinMode(BUTTON_PIN, INPUT_PULLDOWN);

    i2cMutex = xSemaphoreCreateMutex();
    currentScreenMutex = xSemaphoreCreateMutex();

    Wire.begin(21, 22);
    delay(100);

    lcd.init();
    lastButtonState = digitalRead(BUTTON_PIN);

    if (lastButtonState == HIGH) {  
      digitalWrite(LCD_LED_PIN, LOW);
      lcd.display();
      lcd.backlight();
      isLCDOn = true;
    } else {
      digitalWrite(LCD_LED_PIN, HIGH);
      lcd.noDisplay();
      lcd.noBacklight();
      isLCDOn = false;
    }

    xTaskCreatePinnedToCore(Task_Button, "TaskButton", 4096, NULL, 1, NULL, 0);


    if (isLCDOn) {
      if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        LCD_ShowScreen(currentScreen);
        xSemaphoreGive(i2cMutex);
      }
    }

    delay(2000);

    if (xSemaphoreTake(currentScreenMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        currentScreen = SCREEN_SYSTEM_STARTING;
        xSemaphoreGive(currentScreenMutex);
    }

    if (isLCDOn) {
        if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
            LCD_ShowScreen(currentScreen);
            xSemaphoreGive(i2cMutex);
        }
    }

    delay(2000);

    RS485Serial.begin(9600, SERIAL_8N1, 26, 25);

    mb.begin(&RS485Serial);
    mb.slave(SLAVE_ID);

    // Thanh ghi điều khiển (Control registers)
    mb.addHreg(REQUEST_DATA_REGN, 0);
    mb.addHreg(DATA_READY_REGN, 0);

    // Thanh ghi dữ liệu cảm biến 
    mb.addHreg(TEMP_REGN);
    mb.addHreg(HUMI_REGN);
    mb.addHreg(CO2_REGN);
    mb.addHreg(AQI_REGN);
    mb.addHreg(TVOC_REGN);

    // Thanh ghi trạng thái lỗi 
    mb.addHreg(STATUS_SCD41_REGN, 0);
    mb.addHreg(STATUS_ENS160_REGN, 0);

    SCD41_Init();

    for (int i = 0; i < 5; i++)
    {
        if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
            ens160Initialized = ENS160_Init();
            xSemaphoreGive(i2cMutex);
        }
        if (ens160Initialized)
        {
            break;
        }
        if (i < 4)
        {
            delay(500);
        }
        else
        {
            mb.Hreg(STATUS_ENS160_REGN, 1);
        }
    }

    if (xSemaphoreTake(currentScreenMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        currentScreen = SCREEN_WAITING_REQUEST;
        xSemaphoreGive(currentScreenMutex);
    }

    if (isLCDOn) {
      if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        LCD_ShowScreen(currentScreen);
        xSemaphoreGive(i2cMutex);
      }
    }

    xTaskCreatePinnedToCore(Task_SystemMonitor, "TaskSystemMonitor", 4096, NULL, 1, NULL, 0);
}

void loop()
{
    mb.task(); // Handle Modbus requests
    yield();   // Avoid watchdog reset on ESP chips
}

void SCD41_Init()
{
    sensor.begin(Wire, SCD41_I2C_ADDR_62);
    delay(30);

    sensor.wakeUp();

    sensor.stopPeriodicMeasurement();

    sensor.reinit();
}

bool SCD41_ReadData(uint16_t &co2, float &temperature, float &humidity)
{
    error = sensor.wakeUp();
    if (error != NO_ERROR)
    {
        return false;
    }

    error = sensor.measureSingleShot();
    if (error != NO_ERROR)
    {
        return false;
    }

    uint32_t co2Sum = 0;
    float tempSum = 0;
    float humiSum = 0;
    int validReadings = 0;

    for (int i = 0; i < 3; i++)
    {
        uint16_t tempCO2;
        float tempTemp, tempHumi;

        error = sensor.measureAndReadSingleShot(tempCO2, tempTemp, tempHumi);
        if (error == NO_ERROR)
        {
            co2Sum += tempCO2;
            tempSum += tempTemp;
            humiSum += tempHumi;
            validReadings++;
        }

        if (i < 2)
        {
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
    }

    if (validReadings > 0)
    {
        co2 = co2Sum / validReadings;
        temperature = tempSum / validReadings;
        humidity = humiSum / validReadings;
        return true;
    }
    else
    {
        return false;
    }
}

bool ENS160_Init()
{
    if (ens160.begin() != NO_ERR)
    {
        Serial.println("ENS160 init failed, check connection!");
        return false;
    }

    ens160.setPWRMode(ENS160_STANDARD_MODE);
    return true;
}

void ENS160_ReadData(uint8_t &aqi, uint16_t &tvoc, float temperature, float humidity)
{
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
      ens160.setTempAndHum(temperature, humidity);
      xSemaphoreGive(i2cMutex);
    } else {
        return;
    }

    uint32_t aqiSum = 0;
    uint32_t tvocSum = 0;
    int validReadings = 0;

    for (int i = 0; i < 3; i++)
    {
        uint8_t tempAQI;
        uint16_t tempTVOC;
        if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
          tempAQI = ens160.getAQI();
          tempTVOC = ens160.getTVOC();
          xSemaphoreGive(i2cMutex);
        } else {
            continue;
        }

        aqiSum += tempAQI;
        tvocSum += tempTVOC;
        validReadings++;

        if (i < 2)
        {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }

    if (validReadings > 0)
    {
        aqi = aqiSum / validReadings;
        tvoc = tvocSum / validReadings;
    }
}

void LCD_ShowScreen(uint8_t currentScreen) {
    lcd.clear();
    switch (currentScreen) {
        case SCREEN_WELCOME:
            lcd.setCursor(0, 0);
            lcd.print(" IoT Air Quality ");
            lcd.setCursor(0, 1);
            lcd.print("   Monitor    ");
            break;
        case SCREEN_SYSTEM_STARTING:
            lcd.setCursor(0, 0);
            lcd.print("Node-");
            lcd.print(SLAVE_ID);
            lcd.print(" starting");
            break;
        case SCREEN_WAITING_REQUEST:
            lcd.setCursor(0, 0);
            lcd.print("Waiting for");
            lcd.setCursor(0, 1);
            lcd.print("request...");
            break;
        case SCREEN_PREPARE_DATA:
            lcd.setCursor(4, 0);
            lcd.print("[Node-");
            lcd.print(SLAVE_ID);
            lcd.print("]");
            lcd.setCursor(0, 1);
            lcd.print(" Preparing");
            lcd.print(" data");
            break;
        case SCREEN_TEMP_HUMI:
            lcd.setCursor(0, 0);
            lcd.print("Temp: ");
            if (errSCD41) {
                lcd.print("N/A");
            } else {
                lcd.print(temperature, 1);
                lcd.print((char)223);
                lcd.print("C");
            }
            lcd.setCursor(0, 1);
            lcd.print("Humi: ");
            if (errSCD41) {
                lcd.print("N/A");
            } else {
                lcd.print(humidity);
                lcd.print("%");
            }
            break;
        case SCREEN_CO2_TVOC:
            lcd.setCursor(0, 0);
            lcd.print("CO2:");
            if (errSCD41) {
                lcd.print("N/A");
            } else {
                lcd.print(co2);
                lcd.print(" ppm");
            }
            lcd.setCursor(0, 1);
            lcd.print("TVOC:");
            if (ens160Initialized) {
                lcd.print(tvoc);
                lcd.print(" ppb");
            } else {
                lcd.print("N/A");
            }
            break;
        case SCREEN_AQI:
            lcd.setCursor(0, 0);
            lcd.print("AQI:");
            if (ens160Initialized) {
                lcd.print(aqi);
            } else {
                lcd.print("N/A");
            }
            break;
        case SCREEN_GATEWAY_OFFLINE:
            lcd.setCursor(0, 0);
            lcd.print("Gateway offline!");
            break;
    }
}

void readSensors()
{
    if (SCD41_ReadData(co2, temperature, humidity))
    {
        errSCD41 = false;
        mb.Hreg(CO2_REGN, co2);
        mb.Hreg(TEMP_REGN, (uint16_t)(temperature * 100));
        mb.Hreg(HUMI_REGN, (uint16_t)(humidity * 100));
        mb.Hreg(STATUS_SCD41_REGN, 0);
    }
    else
    {
        errSCD41 = true;
        mb.Hreg(STATUS_SCD41_REGN, 1);
    }

    if (ens160Initialized)
    {
        ENS160_ReadData(aqi, tvoc, temperature, humidity);
        mb.Hreg(AQI_REGN, aqi);
        mb.Hreg(TVOC_REGN, tvoc);
    }
}

void Task_SystemMonitor(void *pvParameters)
{
    (void)pvParameters;
    bool isGatewayOnline = false;
    bool dataAvailable = false;
    unsigned long lastRequestTime = millis();
    uint32_t lastScreenChangeTime = millis();
    
    while (1)
    {
        uint16_t req = mb.Hreg(REQUEST_DATA_REGN);

        if (req == 1)
        {
            lastRequestTime = millis();
            if (xSemaphoreTake(currentScreenMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                currentScreen = SCREEN_PREPARE_DATA;
                xSemaphoreGive(currentScreenMutex);
            }

            if (isLCDOn) {
                if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                    LCD_ShowScreen(currentScreen);
                    xSemaphoreGive(i2cMutex);
                }
            }
            mb.Hreg(DATA_READY_REGN, 0);
            digitalWrite(PUMP_PIN, LOW);
            vTaskDelay(15000 / portTICK_PERIOD_MS);
            digitalWrite(PUMP_PIN, HIGH);
            vTaskDelay(60000 / portTICK_PERIOD_MS);
            readSensors();
            mb.Hreg(DATA_READY_REGN, 1);
            mb.Hreg(REQUEST_DATA_REGN, 0);
            isGatewayOnline = true;
            dataAvailable = true;
        } else if (millis() - lastRequestTime > GATEWAY_TIMEOUT && isGatewayOnline) {
            isGatewayOnline = false;
            dataAvailable = false;
            if (xSemaphoreTake(currentScreenMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                currentScreen = SCREEN_GATEWAY_OFFLINE;
                xSemaphoreGive(currentScreenMutex);
            }
            if (isLCDOn) {
                if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                    LCD_ShowScreen(currentScreen);
                    xSemaphoreGive(i2cMutex);
                }
            }
        } 
        
        if (millis() - lastScreenChangeTime >= 3000 && dataAvailable) {
            if (xSemaphoreTake(currentScreenMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                if (currentScreen == SCREEN_PREPARE_DATA) {
                    currentScreen = SCREEN_TEMP_HUMI;
                } else {
                    currentScreen = SCREEN_TEMP_HUMI + (currentScreen - SCREEN_TEMP_HUMI + 1) % 3;
                }
                xSemaphoreGive(currentScreenMutex);
            }

            lastScreenChangeTime = millis();
            
            if (isLCDOn) {
                if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                  LCD_ShowScreen(currentScreen);
                  xSemaphoreGive(i2cMutex);
                }
            }
            
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void Task_Button(void *pvParameters) {
    (void)pvParameters;
    while (1) {
      uint8_t currentButtonState = digitalRead(BUTTON_PIN);
  
      if (currentButtonState != lastButtonState) {
        vTaskDelay(150 / portTICK_PERIOD_MS); 
        currentButtonState = digitalRead(BUTTON_PIN);
        if (currentButtonState != lastButtonState) {
          lastButtonState = currentButtonState;
          if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (currentButtonState == HIGH) {
                digitalWrite(LCD_LED_PIN, LOW);
                lcd.display();
                lcd.backlight();
                if (xSemaphoreTake(currentScreenMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                    LCD_ShowScreen(currentScreen);
                    xSemaphoreGive(currentScreenMutex);
                }
                isLCDOn = true;
            } else if (currentButtonState == LOW) {
                digitalWrite(LCD_LED_PIN, HIGH);
                lcd.clear();
                lcd.noDisplay();
                lcd.noBacklight();
                isLCDOn = false;
            }
            xSemaphoreGive(i2cMutex);
          }
        }        
      }
      vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}
