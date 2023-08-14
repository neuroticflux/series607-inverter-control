import struct
import device
import probe
import logging

from register import Reg, Reg_s32b, Reg_u16, Reg_u32b, Reg_u64b, Reg_text

log = logging.getLogger()

class Reg_ver(Reg, int):
	def __init__(self, base, name):
		Reg.__init__(self, base, 1, name)
	
	def __int__(self):
		v = self.value
		return v[0] << 16 | v[1] << 8 | v[2]
	
	def __str__(self):
		return '%d.%d.%d' % self.value

	def decode(self, values):
		v = values[0]
		return self.update((v >> 12, v >> 8 & 0xf, v & 0xff))


class Series607(device.ModbusDevice):
	productid = 607
	productname = 'Series607 FSW Inverter'
	min_timeout = 0.5
	allowed_roles = None
	default_role = 'inverter'
	default_instance = 40
	phases = 1

	def __init__(self, *args):
		super(Series607, self).__init__(*args)

		self.info_regs = [
			Reg_ver(0x1005, '/HardwareVersion'),
			Reg_ver(0x1006, '/FirmwareVersion'),
			Reg_text(0x1000, 3, '/Serial'),
		]

	def device_init(self):
		self.data_regs = [
			Reg_u16(0x3001,  '/Dc/0/Voltage',	100, '%.1f V'),
			Reg_u16(0x3002,  '/Ac/Out/L1/I',	100, '%.1f A'),
			Reg_u16(0x3003,  '/Ac/Out/L1/P',	100, '%.1f W'),
			Reg_u16(0x3004,  '/Mode', write=True),
			Reg_u16(0x3005,  '/State'),
		]

	def get_ident(self):
#		return 'series607_1kw'
#		return 'series607_%s' % self.name
		return 'series607_%s' % self.info['/Serial']




models = {
	6072: {
		'model': 'Series607 1000W',
		'handler': Series607,
		},
	}

probe.add_handler(probe.ModelRegister(Reg_u16(0x05a0), models,
					methods=['rtu'],
					units=[42]))
