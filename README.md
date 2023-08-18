# series607-inverter-control

### Overview
#### What is this?
The Series607 Inverter Control is an Arduino-based device providing data logging and remote control of a Series 607 inverter (Found in Sweden at Kjell &amp; Co under the brand LuxorParts).
The Series 607 comes with a wired control panel showing power draw on the AC side and voltage on the DC side. The device described in this repository hijacks the communication bus going to the control panel and decodes the data, then reports it via Modbus to a master device, such as a Victron Cerbo GX, or a Raspberry Pi running VenusOS. It even lets you remotely turn the inverter on or off from the touch menu, and reports alarms and fault codes like undervoltage, overvoltage, overheating etc. :)

<img src="https://github.com/neuroticflux/series607-inverter-control/assets/871649/73e0d629-a947-4566-ada9-d2cb691dabe4" width=400 align="right">

#### Disclaimer
If you want to try this out with your own Series 607 inverter, feel free! Of course I take no responsibility for what happens to you or your hardware if you use any material or information provided in this repository. You should always be careful around high voltages and high currents. There is NO NEED to get anywhere near the AC side outlets while building this project, and there is ABSOLUTELY NO NEED to open up the inverter itself.

#### RJ12 cable pinout:
Both the pinout and color codes may very well be different on different units, so always double-check before you plug in your expensive equipment that you have your wires hooked up right!

- PIN1 (blue): power switch
- PIN2 (black): +12V
- PIN3 (red): GND
- PIN4 (green): DATA
- PIN5 (yellow): CS
- PIN6 (brown): CLK

### Instructions

#### Hardware
You need an Arduino (I've tested on Uno and the prototype runs on a Micro, likely any ATmega328-based board will work) and some RS232-to-USB bridge like an FTDI breakout board or dongle. It needs to show up in VenusOS as /dev/ttyUSB*, so using the built-in USB port on the Arduinos doesn't work. If anyone can figure out how to set VenusOS up so it scans /dev/ttyACM* as well, please let me know. :)

You also need one PNP and one NPN transistor - 2N3904 and 2N3906 work fine - and two 10kohm resistors. There's a schematic under /pcb but it's not up to date. Check the Falstad link below for a schematic of the high side switch circuit you need for remote on/off switching in VenusOS to work, the rest should be fairly self-explanatory. Pin 1 (power switch) goes to the collector of the PNP, Pin 2 (+12V) goes to the emitter.
Pins 4, 5, and 6 go to the SPI MOSI, CS and CLK pins on the Arduino, and Arduino Pin 4 goes to the base of the NPN.
If you have an Arduino Micro, you can use the hardware serial port to interface with the FTDI board. The sketch in the repo is already set up for that. If you have a Uno or some other Arduino-style board, you might need to alter the code to set up a SoftwareSerial port on two of the digital pins and connect those to the FTDI.

The circuit is simple enough to deadbug, or stick it on a protoboard with fancy pin headers if you want. :)
Burn the sketch to the Arduino and connect it to the inverter according to the pinout.

DON'T GET +12V NEAR THE GPIO LINES! It will fry your microcontroller. Yes, it's happened to me, more than once. :P

#### VenusOS
Make sure you have superuser access to your Victron device, and a way to get files onto it (wget works fine I guess).
Put the series607.py script in /opt/victronenergy/dbus-modbus-client/, then edit the dbus-modbus-client.py script in that same folder. Add this line to the list of imports:
```Python
import series607
```
Now, when you connect the interface using the RS232 dongle, a new device called 'Series607 FSW Inverter' should show up in your VenusOS menu. It may take a minute for the TTY poller to run, so be patient. ;)

If you've gotten the pinout and high-side switch right, you should now be able to control your inverter and get DC-side voltage and AC-side power showing up.

### Technical

Simulating the high side BJT switch circuit in Falstad:
https://tinyurl.com/24upg6vq

TODO: Schematics/images

### Compatibility
I've only been able to find these inverters here in Sweden, but I'd love to know if there are similar products out there with the same or similar connectivity that could be hacked in this way. Feel free to post an issue if you have a question or can provide information, or a pull request if you have something to add like code or revised/improved hardware.
