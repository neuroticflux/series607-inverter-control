# series607-inverter-control

### What is this?
The Series607 Inverter Control is an Arduino-based device providing data logging and remote control of a Series 607 inverter (Found in Sweden at Kjell &amp; Co under the brand LuxorParts).
The Series 607 comes with a wired control panel showing power draw on the AC side and voltage on the DC side. The device described in this repository hijacks the communication bus going to the control panel and decodes the data, then reports it via Modbus to a master device, such as a Victron Cerbo GX, or a Raspberry Pi running VenusOS. It even lets you to remotely turn the inverter on or off from the touch menu. :)

<img src="https://github.com/neuroticflux/series607-inverter-control/assets/871649/73e0d629-a947-4566-ada9-d2cb691dabe4" width=400 align="right">

If you want to try this out with your own Series 607 inverter, feel free to try! I take no responsibility for what happens to your hardware if you use any material or information provided in this repository.

### RJ12 cable pinout (color codes may be different):

<img src="https://github.com/neuroticflux/series607-inverter-control/assets/871649/72bbce81-5711-48db-a272-294f5fbefaaf" width=400>

- PIN1 (blue): power switch
- PIN2 (black): +12V
- PIN3 (red): GND
- PIN4 (green): DATA
- PIN5 (yellow): CS
- PIN6 (brown): CLK

### Instructions

#### Hardware
You need an Arduino (I've tested with Uno, but should work with Micro or any Atmega328-based board as well), and some RS232-to-USB bridge, breakout board or dongle. You also need one PNP and one NPN transisto - 2N3904 and 2N3906 work fine - and two 10kohm resistors. Connect them according to the schematic (look in pcb/) and -hopefully - Bob's your uncle. The circuit is simple enough to deadbug, or stick it on a protoboard with fancy pin headers if you want. :)
Upload the sketch to the Arduino and connect it to the inverter according to the pinout above.

#### VenusOS
Make sure you have superuser access to your Victron device, and a way to get files onto it (wget works fine I guess).
Put the series607.py script in /opt/victronenergy/dbus-modbus-client/, then edit the dbus-modbus-client.py script in that same folder. Add this line to the list of imports:
```Python
import series607
```
When you connect the interface using the RS232 dongle, a new device called 'Series607 FSW Inverter' should show up in your VenusOS menu. It may take a minute for the TTY poller to run, so be patient. ;)

If you've gotten the pinout and high-side switch right, you should now be able to control your inverter and get DC-side voltage and AC-side power showing up.

### Technical

Simulating the high side BJT switch circuit in Falstad:
https://tinyurl.com/24upg6vq

TODO: Schematics/images
