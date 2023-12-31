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

#define POWER_PIN 4

#define BUFFER_SIZE 0x100
#define BUFFER_SIZE_MASK 0xff

#define FIRMWARE_VER 0x0001
#define HARDWARE_VER 0x0001

#define SERIAL_NO 1234

#define MODEL_NO 6072

// TODO: Can we get AC voltage from Venus in case it's available and override this? Since this isn't actually
// reading a sensor it feels like a bit of a hack.
#define AC_VOLTAGE 230

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

  REG_DCVoltage = 0x3001,
  REG_ACVoltage, // The inverter isn't measuring AC volts, but we might want to add functionality for it eventually. For now we just fake it by sending AC_VOLTAGE whenever the inverter is on.
  REG_Current,
  REG_Power,
  REG_Mode,
  REG_State,
  REG_AlarmLowVoltage = 0x3010,
  REG_AlarmHighVoltage,
  REG_AlarmOverload,
  REG_AlarmHighTemperature,
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

enum FaultCode {
  Fault_NoFault = 0,
  Fault_UnderVoltage,
  Fault_OverVoltage,
  Fault_Overload,
  Fault_Overheat,
  Fault_ShortCircuit,
};

enum FaultLevel {
  FaultLevel_OK = 0,
  FaultLevel_Warning,
  FaultLevel_Alarm
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
uint32_t keepAlivePeriod = 700000; //us

uint32_t displaySwitchTimer = 0;
uint32_t displaySwitchPeriod = 1000000; //us

// TODO: Alarms (follow Victron specs)
FaultCode faultCode = Fault_NoFault;

// Buffer for SPI messages
uint8_t message[8];

// Arduino Micro has hardware serial on Serial1, the built-in USB serial is software
// and doesn't work with VenusOS (it shows up as ttyACM* instead of ttyUSB*).
// So we can't use the USB port on the Micro for Modbus, but for now
// we'll use an FTDI breakout board on Serial1 and eventually maybe move to a custom PCB.
ModbusSerial modbus(Serial1, 42);

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

// TODO: Maybe try doing just a warning on overload and alarm on short circuit using the Overload register, since there's no short circuit alarm on VenusOS
void UpdateAlarms()
{
  // This is quite ugly but for now it works. The more I work with this modbus lib the less I like it...
  // TODO: Should really look into rolling our own or replace it.
  // Iterate all the alarms and reset every code except the one that was triggered.
  modbus.setHreg(REG_AlarmLowVoltage, faultCode == Fault_UnderVoltage ? FaultLevel_Alarm : FaultLevel_OK);
  modbus.setHreg(REG_AlarmHighVoltage, faultCode == Fault_OverVoltage ? FaultLevel_Alarm : FaultLevel_OK);
  modbus.setHreg(REG_AlarmOverload, faultCode == Fault_Overload ? FaultLevel_Alarm : FaultLevel_OK);
  modbus.setHreg(REG_AlarmHighTemperature, faultCode == Fault_Overheat ? FaultLevel_Alarm : FaultLevel_OK);
  modbus.setHreg(REG_AlarmOverload, faultCode == Fault_ShortCircuit ? FaultLevel_Alarm : FaultLevel_OK); // VenusOS doesn't support the short circuit fault code, so we'll set overload instead
}

void setup() {

  // Enable internal pullup resistor on SS line, in case the device is disconnected from the inverter we don't want garbage data in the SPI buffer
  pinMode(SS, INPUT_PULLUP);

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

  // TODO: Proper debug #define and dummy functions for debug/release builds
  Serial.begin(115200);

  // TODO: Dip switches to set baud rate?
  Serial1.begin(38400);

  delay(500);

  Serial.print("Series607 Interface starting up... ");
  modbus.config(38400);
  
  // TODO: Figure out if this is useful for anything
  modbus.setAdditionalServerData("TEST");

  // Start in off state
  powerState = State_Off;
  powerMode = Mode_Off;

  // Set up Modbus registers
  // NOTE: For now everything is holding registers, it looks like Venus defaults to sending the Modbus 0x3 command (Read Holding Register) no matter what,
  // and uses a flag in the driver to signify read-only vs read/write registers. See the Python script.
  modbus.addHreg(REG_ModelNo, MODEL_NO);
  
  // TODO: Fix serial number registers
  modbus.addHreg(REG_SerialNo + 0, 'A' >> 8 & 'B');
  modbus.addHreg(REG_SerialNo + 1, 'C' >> 8 & 'D');
  modbus.addHreg(REG_SerialNo + 2, 'E' >> 8 & 'F');

  // TODO: Proper version numbers and decoding on Venus
  modbus.addHreg(REG_HardwareVer, HARDWARE_VER);
  modbus.addHreg(REG_FirmwareVer, FIRMWARE_VER);

  // Actual device stats and alarms/warnings
  modbus.addHreg(REG_DCVoltage);
  modbus.addHreg(REG_ACVoltage);
  modbus.addHreg(REG_Current);
  modbus.addHreg(REG_Power);

  modbus.addHreg(REG_State, powerState);
  modbus.addHreg(REG_Mode, powerMode);

  modbus.addHreg(REG_AlarmLowVoltage, 0);
  modbus.addHreg(REG_AlarmHighVoltage, 0);
  modbus.addHreg(REG_AlarmOverload, 0);
  modbus.addHreg(REG_AlarmHighTemperature, 0);

  // TODO: Fault alarm registers

  // Short delay to allow SPI to arrive, in case the inverter is already on
  delay(500);
  
  Serial.println("Ready.");
}

int32_t CalculateCurrent(int32_t power) {

  // Some fixed-point math to avoid floats
  int32_t voltage = AC_VOLTAGE * 10;
  power *= 1000;

  return (power / voltage); // One division is fine, just don't get comfortable using them on the 328... :p
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

    // TODO: Clean this up, functionize etc
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
      faultCode = Fault_NoFault;

      //...then falsify
      if(message[1] >> 1 == SEG_F) {
        faultCode = (FaultCode)SegmentDataToDigit(message[3] >> 1);
        UpdateAlarms();
      }

      else if (displayMode == DM_Voltage) {
        // Compose DC voltage result (in mV)
        voltage = SegmentDataToDigit(message[3] >> 1) * 100;
        voltage += SegmentDataToDigit(message[2] >> 1) * 1000;
        voltage += SegmentDataToDigit(message[1] >> 1) * 10000;

      } else {
        // TODO: Look into 32-bit register by using two regs for one value, but might be easier with own/other modbus lib
        power = SegmentDataToDigit(message[3] >> 1) * 1;
        power += SegmentDataToDigit(message[2] >> 1) * 10;
        power += SegmentDataToDigit(message[1] >> 1) * 100;
      }

      // Message handled, reset state
      receivingData = 0;
      address = 0;
    }
  }
  // When we're done processing messages, do higher level stuff

  // Set fault state if we detected a fault
  if(faultCode != Fault_NoFault) {
    powerState = State_Fault;
  }

  uint32_t t = micros();

  // Check the mode register, should we power up/down?
  switch((PowerMode)modbus.hreg(REG_Mode)) {
    case Mode_On: {
      // If the mode is On and the inverter is currently off, and not in fault mode, press the button
      // TODO: Timeout, don't want to indefinitely press the button if something's wrong
      if(powerState == State_Off) {
        DoPressDown();
      } else {
        DoPressUp();
      }
    } break;

    case Mode_Off: {
      // If the mode is Off and the inverter isn't off, press the button
      if(powerState != State_Off) {
        DoPressDown();
      } else {
        DoPressUp();
      }
    } break;
  }

  // FSM
  switch(powerState) {

    case State_Off: {
      voltage = 0;
      power = 0;
      current = 0;

      // TODO: Turning off is slightly unreliable because setting the mode switch line high activates SPI,
      // and because we're still using the power pin to switch display mode, the device thinks that the inverter turned back on,
      // then off again when keepAlive times out.
      // Easiest solution is probably to have some grace period after changing power state before we allow switching the display mode.
      // Seems we're also receiving some garbage data packets when this happens, sometimes the voltage will report as several hundred volts for example,
      // so it's probably a good idea for several reasons.
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
      if(t - displaySwitchTimer > displaySwitchPeriod && powerMode != Mode_Off) {
        SwitchDisplay();
        displaySwitchTimer = t;

        // TODO: Proper debug mode
//        Serial.print("Voltage: ");
//        Serial.println(voltage);
//        Serial.print("Current: ");
//        Serial.println(current);
//        Serial.print("Power: ");
//        Serial.println(power);
      }

    // On-states fall through to State_Fault so we can skip switching displays in fault mode (button presses have no effect) but still do keepalive in one place
    case State_Fault:
      // Clear fault state and set correct state according to power usage
      if (faultCode == Fault_NoFault) {
        UpdateAlarms();
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
  current = CalculateCurrent(power);

  modbus.setHreg(REG_DCVoltage, voltage);
  modbus.setHreg(REG_ACVoltage, powerState != State_Off ? AC_VOLTAGE : 0);
  modbus.setHreg(REG_Power, power);
  modbus.setHreg(REG_Current, current);
  modbus.setHreg(REG_State, powerState);
}
