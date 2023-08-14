// Series 607 Inverter Arduino Interface
// Andy Christiansson (@neuroticflux) 2023-08-15

// Check out the repo at https://github.com/neuroticflux/series607-inverter-control for details and the latest version.

// This sketch lets an Arduino microcontroller interface with a Series 607 PSW inverter (like those sold by Kjell & Co in Sweden),
// effectively hijacking the LCD remote protocol to read power and voltage values, and, with a simple external circuit,
// allows the Arduino to remotely turn the inverter on and off.

// The interface speaks Modbus (currently over software serial port) and,
// with the accompanying Python script, can be used from a Victron Cerbo GX,
// a Raspberry Pi or other hardware running Venus OS.

// Depends on ModbusSerial by @epsilonrt, found in the Arduino IDE Library Manager, or here:
// https://github.com/epsilonrt/modbus-serial

// DISCLAIMER:
// THIS IS A HACK! USE AT YOUR OWN RISK! I have NOT tested this for safety, fire hazards etc. IT COULD BURN DOWN YOUR HOUSE/BOAT/MOTORHOME!
// I take no responsibility for what you decide to do with this code.

// GLHF! :)

#include "Arduino.h"
#include "SPI.h"
#include "ModbusSerial.h"
#include "SoftwareSerial.h"

#define SET_ADDRESS_ZERO_CMD 3

#define POWER_PIN 7

#define BUFFER_SIZE 0x100
#define BUFFER_SIZE_MASK 0xff

#define FIRMWARE_VER 73
#define HARDWARE_VER 124

#define SERIAL_NO 1234

#define MODEL_NO 6072

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

// These registers are somewhat randomly picked,
// I haven't followed any conventions etc.
// Hopefully it won't cause any headaches but for now they are easily changed.
enum ModbusRegisters {
  REG_ModelNo = 0x05a0,
  REG_SerialNo = 0x1000,
  REG_HardwareVer = 0x1005,
  REG_FirmwareVer,

  REG_Voltage = 0x3001,
  REG_Current,
  REG_Power,
  REG_Mode,
  REG_State,
  REG_Fault,
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

// These match the Victron inverter state values according to (https://github.com/victronenergy/venus/wiki/dbus)
enum PowerState
{
  State_Off = 0,
  State_LowPower = 1,
  State_Fault = 2,
  State_Inverting = 9,
};

// Again, these match Victron's values, for simplicity
enum PowerMode
{
  Mode_On = 2,
  Mode_Off = 4,
//  Mode_Eco = 5, // Victron are using this but obviously we don't support it
};

enum DFaultCode {
  F_NoFault = 0,
  F_UnderVoltage,
  F_OverVoltage,
  F_Overload,
  F_Overheat,
  F_ShortCircuit
};

PowerState powerState; // Purely reactive state, never set this explicitly from client/UI
PowerMode powerMode; // Controlling state, this is either set explicitly via Modbus or reactively when the inverter is turned on externally

DisplayMode displayMode; // Keep track of the LCD "page" - a fast button press switches between the DC (voltage) and AC (power) display

volatile uint8_t buffer[BUFFER_SIZE];

volatile uint8_t head = 0;
volatile uint8_t tail = 0;

// TODO: These are actually not in millis, decide what to do here.
//       With 16-bit registers using millis is a bit tight, so this
//       is probably fine, but they will have to be scaled by client

int32_t voltage = 0; // Millivolts
int32_t  current = 0; // Milliamps
int32_t power = 0; // Milliwatts

// Flag to keep an eye on SPI activity
volatile uint8_t didReceiveSPI = 0;

// Timing
uint32_t keepAliveTimer = 0;
uint32_t keepAlivePeriod = 1000000; //us

uint32_t displaySwitchTimer = 0;
uint32_t displaySwitchPeriod = 800000; //us

// TODO: Alarms (follow Victron specs)
DFaultCode faultCode = F_NoFault;

// Buffer for SPI messages
uint8_t message[8];

// TODO: Modbus through the hardware UART (once firmware is more stable)
SoftwareSerial Serial2(4, 5);
ModbusSerial modbus(Serial2, 42);

// SPI interrupt
ISR(SPI_STC_vect)
{
  buffer[++head & BUFFER_SIZE_MASK] = SPDR;
  didReceiveSPI = 1;
}

// Some helper functions to control state
void ModeOn()
{
  head = 0;
  tail = 0;
  powerMode = Mode_On;
  modbus.setHreg(REG_Mode, Mode_On);
}

void ModeOff()
{
  powerMode = Mode_Off;
  modbus.setHreg(REG_Mode, Mode_Off);
}

void SwitchDisplay()
{
  SPI.detachInterrupt();
  head = 0;
  tail = 0;
  ShortPress();
  SPI.attachInterrupt();
}

void DoPressDown()
{
  digitalWrite(POWER_PIN, HIGH);
}

void DoPressUp()
{
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
  modbus.config(38400);
  modbus.setAdditionalServerData("TEST");

  // Start in off state
  powerState = State_Off;
  powerMode = Mode_Off;

  // Set up Modbus registers
  modbus.addHreg(REG_ModelNo, MODEL_NO);
  
  // TODO: Fix serial number registers
  modbus.addHreg(REG_SerialNo + 0, 'A' >> 8 & 'B');
  modbus.addHreg(REG_SerialNo + 1, 'C' >> 8 & 'D');
  modbus.addHreg(REG_SerialNo + 2, 'E' >> 8 & 'F');

  // TODO: Proper version numbers and decoding on Venus
  modbus.addHreg(REG_HardwareVer, HARDWARE_VER);
  modbus.addHreg(REG_FirmwareVer, FIRMWARE_VER);

  modbus.addHreg(REG_Voltage);
  modbus.addHreg(REG_Current);
  modbus.addHreg(REG_Power);

  modbus.addHreg(REG_State, powerState);
  modbus.addHreg(REG_Mode, powerMode);
  
  // Short delay to allow SPI to arrive, in case the inverter is already on
  delay(500);
  
  Serial.println("Series607 Interface ready.");
}

int32_t CalculateCurrent(int32_t voltage, int32_t power) {

  // Early out in case of zero denominator
  if (voltage == 0) {
    return 0;
  }

  //Increase resolution for calculation
  voltage *= 4;
  power *= 4;

  return (power / voltage) >> 4; // One division is fine, just don't get comfortable using them on the 328... :p
}

// Flag when we get an actual data packet, not commands
uint8_t receivingData = 0;

// Track current address of data being written
uint8_t address = 0;

void loop() {
  
  modbus.task();

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

      // Assume no fault...
      faultCode = F_NoFault;

      //...then falsify
      if(message[1] >> 1 == SEG_F) {
        faultCode = (DFaultCode)SegmentDataToDigit(message[3] >> 1);
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

  // Set fault state if we detected a fault
  if(faultCode != F_NoFault) {
    powerState = State_Fault;
  }

  uint32_t t = micros();

  // Check the mode register, should we power up/down?
  switch((PowerMode)modbus.hreg(REG_Mode)) {
    case Mode_On: {
      // If the mode is On and the inverter is currently off, and not in fault mode, press the button
      // TODO: Timeout, don't want to indefinitely press the button if something's wrong
      if(powerState == State_Off)
        DoPressDown();
      else
        DoPressUp();
    } break;

    case Mode_Off: {
      // If the mode is Off and the inverter isn't off, press the button
      if(powerState != State_Off)
        DoPressDown();
      else
        DoPressUp();
    } break;
  }

  // FSM
  switch(powerState) {

    case State_Off: {
      voltage = 0;
      power = 0;
      current = 0;
      if (didReceiveSPI == 1) {
        // The inverter turned on, so we should change state. Are we generating power?
        if(power > 0) {
          powerState = State_Inverting;
        } else {
          powerState = State_LowPower;
        }
        Serial.println("Inverter turned on.");
        ModeOn();
      }
    } break;

    // Inverter on, set state, switch display etc
    case State_LowPower:
    case State_Inverting:
    
      // Ask inverter to switch display mode on the LCD (to sniff both DC voltage and AC power values)
      if(t - displaySwitchTimer > displaySwitchPeriod) {
        SwitchDisplay();
        displaySwitchTimer = t;
      }

    // On-states fall through to State_Fault so we can skip switching displays in fault mode (button presses have no effect) but still do keepalive in one place
    case State_Fault:

      // Clear fault state and set correct state according to power usage
      if (faultCode == F_NoFault) {
        powerState = power > 0 ? State_Inverting : State_LowPower;
      }
      // If we've received at least one SPI message, reset keepalive, otherwise count down
      if (didReceiveSPI == 1) {
        keepAliveTimer = t;
        didReceiveSPI = 0;

      } else if (t - keepAliveTimer > keepAlivePeriod) {
        keepAliveTimer = 0;
        Serial.println("Inverter turned off.");
        powerState = State_Off;
        ModeOff();
      }
    break;
  }

  // Update data registers
  
  // Recalculate current in case power or voltage changed
  // TODO: Recalculate only when value changes
  current = CalculateCurrent(voltage, power);

  modbus.setHreg(REG_Voltage, voltage);
  modbus.setHreg(REG_Power, power);
  modbus.setHreg(REG_Current, current);
  modbus.setHreg(REG_State, powerState);
}
