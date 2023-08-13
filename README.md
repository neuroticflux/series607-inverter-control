# series607-inverter-control

### What is this?
The Series607 Inverter Control is an Arduino-based device providing data logging and remote control of a Series 607 inverter (Found in Sweden at Kjell &amp; Co under the brand LuxorParts).
The Series 607 comes with a wired control panel showing power draw on the AC side and voltage on the DC side. The device described in this repository hijacks the communication bus going to the control panel and decodes the data, then reports it over serial communication to a PC or master device. It also enables remotely turning the inverter on or off by mimicking the on/off switch on the control panel.

<img src="https://github.com/neuroticflux/series607-inverter-control/assets/871649/73e0d629-a947-4566-ada9-d2cb691dabe4" width=400 align="right">


### Overview
For now there's not much here except a simple Arduino sketch. To build this you also need a simple circuit consisting of two BJTs and two resistors and a cable with an RJ12 connector at one end. The BJTs and resistors form a high side switch to let the Arduino's 5V GPIO pins drive the 12V power switch on the inverter.

The goal is to eventually enable integration into Victron's VenusOS and/or SignalK, using ModbusRTU and/or other protocols. The situation as of 2023-08-13 is that there seems to be support for ModbusRTU over TTY in VenusOS, so it may be possible to use the Arduino's own USB port as a USB-to-serial converter, but there's still some research to do. Modbus drivers in VenusOS can be written in Python and seem to be reasonably simple to implement. On the Arduino side, there are several Modbus libraries but the most complete (official) one has a dependency on the RS485 library, making it less useful (I want to avoid RS485 if I can).

Anyway, if you want to try this out with your own Series 607 inverter, feel free to try! I take no responsibility for what happens to your hardware if you use any material or information provided in this repository.

### RJ12 cable pinout (color codes may be different):

<img src="https://github.com/neuroticflux/series607-inverter-control/assets/871649/72bbce81-5711-48db-a272-294f5fbefaaf" width=400>

- PIN1 (blue): power switch
- PIN2 (black): +12V
- PIN3 (red): GND
- PIN4 (brown): CLK
- PIN5 (green): CS
- PIN6 (yellow): DATA

### Technical

Simulating the high side BJT switch circuit in Falstad:
https://tinyurl.com/24upg6vq

TODO: Schematics/images

TODO: Modbus communication

TODO: Victron VenusOS integration using ModbusRTU (getting somewhat conflicting results from Victron's forums re whether it's even possible, but there's some good examples on the VenusOS drive in /opt/victronenergy/dbus-modbus-client/ which indicate it should be possible.

