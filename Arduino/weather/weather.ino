#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <TimeLib.h> // https://github.com/PaulStoffregen/Time
#include <ESP8266HTTPClient.h>
#include <stdint.h>
#include <BME280I2C.h> // https://github.com/finitespace/BME280
#include "Wire.h"
#include <ArduinoOTA.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266WiFiMulti.h>

extern "C"
{
  #include "user_interface.h"  // needed for light sleep (not really working yet)
}

#include "settings.h"

// Loop & delay variables
unsigned long loopstart = 0;
unsigned long loopend = 0;
unsigned long realdelay = 0;
unsigned long worktime = 0;
bool justRunOnce = true;

// Buffer 
time_t timeBuffer[BUFFER_SIZE] = {0};
float tempBuffer[BUFFER_SIZE] = {0.0};
float pressureBuffer[BUFFER_SIZE] = {0.0};
float humidityBuffer[BUFFER_SIZE] = {0.0};
float voltageBuffer[BUFFER_SIZE] = {0.0};
int bufferposition = 0;

// Include functions after declaring other variables they might use
#include "myfunctions.h"
#include "otaUpdate.h"
#include "ntp.h"
#include "serial.h"
#include "sensor.h"
#include "wifi.h"

void setup()
{
  if (DEEP_SLEEP)
  {
    // In case of deep sleep, initialization is also part of the "loop" what we want to know how long it takes with the actual measurement altogether
    loopstart = millis();
    
    // Connect D0 to RST to wake up
    pinMode(D0, WAKEUP_PULLUP);
  }
  
  delay(1000);  // to make sure Arduino serial monitor is ready to receive messages
  
  // Enable watchdog - https://folk.uio.no/jeanra/Microelectronics/ArduinoWatchdog.html
  wdt_enable(WDTO_8S);
  
  // Initialize buffers - Is this really needed???
  for (int i = 0; i++; i < BUFFER_SIZE)
  {
    timeBuffer[i] = 0;
    tempBuffer[i] = 0.0;
    pressureBuffer[i] = 0.0;
    humidityBuffer[i] = 0.0; 
    voltageBuffer[i] = 0.0;
    
    wdt_reset();
  }

  // initialize digital pin LED_BUILTIN as an output.
  pinMode(LED_BUILTIN, OUTPUT);

  #ifdef SERIAL
    setupSerial();
  #endif

  mlog(S_DEBUG, "Reset reason: " + ESP.getResetReason());

  // Check if DEEP SLEEP mode is disabled by jamper between D5-D6 port
  // (if D5-D6 are connected deep sleep is disabled)
  pinMode(D5, OUTPUT);
  digitalWrite(D5, LOW);
  pinMode(D6, INPUT);
  unsigned int d6 = digitalRead(D6);
  if (d6 == 0)
  {
    DEEP_SLEEP = false;
    mlog(S_WARNING, "Deep sleep is disabled by jamper between D5 and D6 ports!");
  }

  setupWiFi();
  setupNtp();
  setupSensor();
  
  #ifdef OTA
    setupOta();
  #endif

  mlog(S_DEBUG, "Collector URL: " + String(DATA_REST_ENDPOINT));
  if (DEEP_SLEEP)
  {
    mlog(S_DEBUG, "Deep sleep is truned ON");
  }
  else
  {
    mlog(S_DEBUG, "Deep sleep is turned OFF");
  }
  
  mlog(S_INFO, "Initialization done");
}

void loop()
{
  mlog(S_INFO, "Current firmware version: " + String(FW_VERSION));
    
  wdt_reset();
  
  digitalWrite(LED_BUILTIN, LOW); // turn led on to show we are working
  if (!DEEP_SLEEP)
  {
    loopstart = millis();
  }
  
  if (justRunOnce == true)
  {
    justRunOnce = false;
    // Code that should just run once after start comes here
    // This is to avoid WDT issues in the setup
    delay(500); // sensor needs about 2ms to start up
  }

  #ifdef OTA
    ArduinoOTA.handle();
  #endif
  
  // Read sensor data into the buffer
  if (now() >= 100000000) // Make sure we received the time via NTP, no use reading sensor data without proper timestamp
  {
    for (int i = 0; i <= BUFFER_SIZE ; i++)
    {
      wdt_reset();
      
      mlog(S_DEBUG, "i = " + String(i) + " BUFFER_SIZE = " + String(BUFFER_SIZE) + " bufferposition = " + String(bufferposition));
      
      if (timeBuffer[bufferposition] == 0)
      {
        break; // Exit the for-loop when we are at an empty buffer position
      }
      else
      {
        bufferposition = (bufferposition + 1) % BUFFER_SIZE; // Try the next position. Automagically rolls over if larger than the buffer
     }
    }
    
    timeBuffer[bufferposition] = now();
    
    float temp(NAN), hum(NAN), pres(NAN);
    BME280::TempUnit tempUnit(BME280::TempUnit_Celsius);
    BME280::PresUnit presUnit(BME280::PresUnit_Pa);
    
    mySensor.read(pres, temp, hum, tempUnit, presUnit);   
    
    pressureBuffer[bufferposition] = pres;
    tempBuffer[bufferposition] = temp;
    humidityBuffer[bufferposition] = hum;    
    voltageBuffer[bufferposition] = analogRead(A0) * VOLTAGE_MULTIPLIER; 
  
    mlog(S_INFO, "Pressure: " + String(pressureBuffer[bufferposition]) + " Temperature: " + String(tempBuffer[bufferposition]) + " Humiditiy: " + String(humidityBuffer[bufferposition]) + " Voltage: " + voltageBuffer[bufferposition]);
    mlog(S_INFO, String(bufferposition) + " data point are waiting for sending");
  }

  int wifiRetryNumber = 5;
  for (int wifiCounter = 0; wifiCounter < wifiRetryNumber; wifiCounter++) // retry if sending was failed in any way
  {
    if (wifiMulti.run() == WL_CONNECTED)
    {
      httpUpdate();
      
      if (timeStatus() != timeSet || now() <= 100000000)
      {
        getNtpTime(); // Update NTP time when necessary
      }
      else
      {
        if (sendSensorData("0")) // Test if we can connect to the php page and send data if we can
        {
          mlog(S_DEBUG, "Connection test is okay, send data!");
          
          sendSensorData(sensorData()); // sending up to 30 datasets at once (we can only send about 3000 characters in the POST request)
          
          mlog(S_INFO, "All data sent.");
        }
        else
        {
          mlog(S_ERROR, "Failed to connect, try again!");
        }
      }
      
      break;
    }
    else
    {
      mlog(S_ERROR, "WiFi not connected, try again!");
    }
  }
  
  // Go to sleep for the specified time (minus the time we needed for all the stuff we did in the loop)
  loopend = millis();
  worktime = loopend - loopstart;
  unsigned long calculatedDelay = calculateDelayTime(analogRead(A0) * VOLTAGE_MULTIPLIER, SLEEP_DELAY);
  
  /*
  realdelay = calculatedDelay - worktime;
  
  if (realdelay <= 0 || realdelay >= 1 * 60 * 60 * 1000) // 1 hour
  {
    mlog(S_ERROR, "Real delay was adjusted. Worktime was too small or too big! (value=" + String(realdelay) + "ms)");
    realdelay = 120 * 1000; // exactly 2 mins might be good to show something went wrong
  }
  */
  realdelay = calculatedDelay;
  
  mlog(S_DEBUG, "Work time: " + String(worktime) + "ms Calculated delay: " + calculatedDelay + "ms Real delay: " + String(realdelay) + "ms");
  
  digitalWrite(LED_BUILTIN, HIGH);  // turn led off to show we are sleeping
  
  // Sleep until the next measurement
  
  mlog(S_INFO, "Sleep for " + String(realdelay / 1000) + " sec");

  #ifdef SERIAL
    Serial.flush();
  #endif

  if (DEEP_SLEEP)
  {
    // Clear UART FIFO to enter into deep sleep mode immediatelly without waiting for sending over what is in the FIFO
    //SET_PERI_REG_MASK(UART_CONF0(0), UART_TXFIFO_RST); //RESET FIFO 
    //CLEAR_PERI_REG_MASK(UART_CONF0(0), UART_TXFIFO_RST);
    // https://www.espressif.com/sites/default/files/9b-esp8266-low_power_solutions_en_0.pdf

    ESP.deepSleep(realdelay * 1000, WAKE_NO_RFCAL);
  }
  else
  {
    for (int i = realdelay/100; i > 0; i--)
    {
      //mlog(S_TRACE, String(i) + " tick left");
      delay(100);
      wdt_reset();
    }
  }
  
  // It seems this line never will be called (reboot happens after deep sleep)
  mlog(S_INFO, "Next measurement comes NOW!");
}
