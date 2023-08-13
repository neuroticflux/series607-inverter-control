// Series 607 Inverter Arduino Remote Control
// Andy Christiansson (@neuroticflux) 2023-08-13

// This sketch lets an Arduino microcontroller interface with a Series 607 FSW inverter (like those sold by Kjell & Co in Sweden),
// effectively hijacking the LCD remote protocol to read power and voltage values, and, with a simple external circuit,
// allows the Arduino to remotely turn the inverter on and off.

// Check out the repo at github.com/neuroticflux/series607-inverter-control for more info.

// THIS IS A HACK! USE AT YOUR OWN RISK! I have NOT tested this for safety, fire hazards etc. IT COULD BURN DOWN YOUR HOUSE/BOAT/MOTORHOME!
// I take no responsibility for what you decide to do with this code.

// GLHF! :)

#include "Arduino.h"
#include "SPI.h"
//#include "ModbusSerial.h"
#include "SoftwareSerial.h"

#define SET_ADDRESS_ZERO_CMD 3

#define POWER_PIN 7

#define BUFFER_SIZE 0x100
#define BUFFER_SIZE_MASK 0xff

// SEGMENT DISPLAY DIGIT LOOKUP TABLE
// In binary, each bit represents a segment on the 7-segment display
// The TM1721 receives an 8-bit value where the low bit is the decimal point,
// so in order to match a received 8-bit value with these segments, shift the value down 1 bit.
enum SegmentDigit {
  SEG_ZERO = 0x7d,
  SEG_ONE = 0x5,
  SEG_TWO = 0x5e,
  SEG_THREE = 0x4f,
  SEG_FOUR = 0x27,
  SEG_FIVE = 0x6b,
  SEG_SIX = 0x7b,
  SEG_SEVEN = 0x45,
  SEG_EIGHT = 0x7f,
  SEG_NINE = 0x6f,
  SEG_F = 0x72,
  SEG_OFF = 0x0,
};

// Translate TM1721 LCD segment data to digits
static inline int8_t SegmentDataToDigit(uint8_t data)
{
  switch(data) {
    case SEG_OFF: return 0;
    case SEG_ZERO: return 0;
    case SEG_ONE: return 1;
    case SEG_TWO: return 2;
    case SEG_THREE: return 3;
    case SEG_FOUR: return 4;
    case SEG_FIVE: return 5;
    case SEG_SIX: return 6;
    case SEG_SEVEN: return 7;
    case SEG_EIGHT: return 8;
    case SEG_NINE: return 9;
    default: return -1;
  }
}

enum DisplayMode
{
  DM_Power = 0,
  DM_Voltage,
};

enum PowerState
{
  P_Off = 0,
  P_On,
  P_Fault,
};

PowerState powerState;
DisplayMode displayMode;

enum DCommand {
  DC_Mode = 0,
  DC_Control = 1,
  DC_Data = 2,
  DC_Address = 3,
};

enum DFaultCode {
  F_NoFault = 0,
  F_UnderVoltage,
  F_OverVoltage,
  F_Overload,
  F_Overheat,
  F_ShortCircuit
};

struct TimeStruct {
  uint32_t period;
  uint32_t t;
};

volatile uint8_t buffer[BUFFER_SIZE];

volatile uint8_t head = 0;
volatile uint8_t tail = 0;

int32_t voltage = 0; // Millivolts
int32_t  current = 0; // Milliamps
int32_t power = 0; // Milliwatts

uint32_t reportTimer = 0;
uint32_t reportPeriod = 1000000; //us

volatile uint8_t didReceiveSPI = 0;

uint32_t keepAliveTimer = 0;
uint32_t keepAlivePeriod = 1000000; //us

uint32_t displaySwitchTimer = 0;
uint32_t displaySwitchPeriod = 800000; //us

DFaultCode faultCode = F_NoFault;

uint8_t message[8];

// Modbus testing
//ModbusSerial modbus(Serial, 42);

SoftwareSerial Serial2(4, 5);

// Receive SPI data
ISR(SPI_STC_vect)
{
  buffer[++head & BUFFER_SIZE_MASK] = SPDR;
  didReceiveSPI = 1;
}

void TurnOff()
{
  Serial.println("Turning off inverter...");
  LongPress();
}

void TurnOn()
{
  Serial.println("Turning on inverter...");
  LongPress();
}

void SetOn()
{
  head = 0;
  tail = 0;
  powerState = P_On;
}

void SetOff()
{
  powerState = P_Off;
}

void SwitchDisplay()
{
  SPI.detachInterrupt();
  head = 0;
  tail = 0;
  ShortPress();
  SPI.attachInterrupt();
}

void LongPress()
{
  digitalWrite(POWER_PIN, HIGH);
  delay(3200);
  digitalWrite(POWER_PIN, LOW);
}

void ShortPress()
{
  digitalWrite(POWER_PIN, HIGH);
  delay(200);
  digitalWrite(POWER_PIN, LOW);
}

void setup() {

  // Set up power on/mode switch pin
  pinMode(POWER_PIN, OUTPUT);
  digitalWrite(POWER_PIN, LOW);

  // Zero data buffer
  head = 0;
  tail = 0;

  // SPI slave mode on
  SPCR |= _BV(SPE);
  SPDR = 0; // Zero the SPI data register so we can check if data arrived

  SPI.setClockDivider(SPI_CLOCK_DIV2);
  SPI.attachInterrupt();

  Serial.begin(115200);
  Serial2.begin(38400);
  delay(500);

  Serial.println("Starting up...");
  Serial2.println("Starting Soft Serial...");

  // Start in off mode
  powerState = P_Off;
  
  delay(500);
  
  Serial.println("Starting reporting.");
}

// Flag when we get an actual data packet, not commands
uint8_t receivingData = 0;

// Track current address of data being written
uint8_t address = 0;

void loop() {

  uint8_t data = 0;
  uint8_t _head = head; // Cache the current buffer head locally (so the ISR can safely cut in here and write new SPI data)

  // NOTE/TODO ON DATA TRANSFERS
  // For unknown reasons, not all data seems to be received. Looking at the logic analyzer, there seems to be a few bytes after the main data packet which the Arduino doesn't pick up.
  // Need to research this further, but as long as we can get out relevant data, it doesn't matter for now.

  // MESSAGE BUFFER ADDRESSES:
  // 00h: State of charge, battery indicator
  // 01h: First digit (high byte is dot)
  // 02h: Second digit
  // 03h: Third digit (high byte is <probably> AC/DC indicator?)
  // 04h: Battery percentages
  // 05h: Battery percentages

  // TODO: Nail down which bit is the AC/DC indicator and battery indicator frame - the latter is blinking at startup, should be possible to spot

  // While there's data in the buffer, get the next byte
  while (_head != tail) {

    data = buffer[++tail & BUFFER_SIZE_MASK];

    switch(data) {

    // Got set address 0 command, so let's start receiving
    case SET_ADDRESS_ZERO_CMD:
      receivingData = 1;
      address = 0;
      continue; // Skip the current command byte

    default: break;
  }

  // We're currently receiving data, so compose the message one byte at a time
  if (receivingData == 1) {
    message[address++] = data;
  }

    // Received complete message (6 bytes), let's process
    if (address > 5) {
      // Is the mode Watts or Voltage? Check the dot bit on 03h to find out

      displayMode = (DisplayMode)(message[3] & 0x1);

      // TODO: Detect unknown segment data (SegmentDataToDigit() returns -1) and handle
      
      // FAULT CODES:
      // F01: Low input voltage
      // F02: High input voltage
      // F03: AC-side current overload
      // F04: High temperature
      // F05: AC-side short circui

      // State should be on since we're receiving messages, so set it here to be able to leave fault mode
      powerState = P_On;

      if(message[1] >> 1 == SEG_F) {
        faultCode = (DFaultCode)SegmentDataToDigit(message[3] >> 1);
        powerState = P_Fault;
      }

      else if (displayMode == DM_Voltage) {
        // Compose DC voltage result (in mV)
        voltage = SegmentDataToDigit(message[3] >> 1) * 10;
        voltage += SegmentDataToDigit(message[2] >> 1) * 100;
        voltage += SegmentDataToDigit(message[1] >> 1) * 1000;

      } else {
        // Compose power result (in mW)
        power = SegmentDataToDigit(message[3] >> 1) * 10;
        power += SegmentDataToDigit(message[2] >> 1) * 100;
        power += SegmentDataToDigit(message[1] >> 1) * 1000;

      }
        // Message handled, reset state
        receivingData = 0;
        address = 0;
    }
  }

  // When we're done processing messages, do higher level stuff

  // Expect commands
  // TODO: Proper modbus interface
  uint8_t cmd = Serial.read();
  switch(cmd) {
    // Power cycle (just receive ASCII "1" for now)
    case 49:
      // Switch power state
      if (powerState == P_On) {
        TurnOff();
      } else {
        TurnOn();
      }
    break;

    default:
    break;

  }

  // Do regular timed updates
  uint32_t t = micros();

  switch(powerState) {

    // Inverter off, set state but don't mess with the unit
    case P_Off: {
      voltage = 0;
      power = 0;
      current = 0;
      if (didReceiveSPI == 1) {
        Serial.println("Inverter turned on.");
          SetOn();
      }
    } break;

    // Inverter on, set state, switch display etc
    case P_On:
      // Ask inverter to switch display mode on the LCD (to sniff both DC voltage and AC power values)
      if(t - displaySwitchTimer > displaySwitchPeriod) {
        SwitchDisplay();
        displaySwitchTimer = t;
      }

    // P_On falls through to P_Fault so we can skip switching displays in fault mode (it has no effect) but still do keepalive
    case P_Fault:
      // If we've received at least one SPI message, reset keepalive, otherwise count down
      if (didReceiveSPI == 1) {
        keepAliveTimer = t;
        didReceiveSPI = 0;

      } else if (t - keepAliveTimer > keepAlivePeriod) {
        keepAliveTimer = 0;
        SetOff();
      }
    break;
  }

  // Calculate data
  uint32_t _v = voltage * 4;
  uint32_t _p = power * 4;

  // Avoid division by zero
  if (voltage > 0) {
    current = (_p / _v) >> 4; // One division is fine, just don't get comfortable using them on the 328... :p
  } else {
    current = 0;
  }

  // TODO: Send data over modbus
  if (t - reportTimer > reportPeriod) {
    Serial.print("Inverter is ");
    switch(powerState) {
      case P_On:
        Serial.println("on.");
      break;
      case P_Fault:
        Serial.println("in fault mode.");
      break;
      case P_Off:
        Serial.println("off.");
      break;
    }

    Serial.print("Voltage: ");
    Serial.print(voltage);
    Serial.println(" mV");
    
    Serial.print("Current: ");
    Serial.print(current);
    Serial.println(" mA");
    
    reportTimer = t;
  }
}
