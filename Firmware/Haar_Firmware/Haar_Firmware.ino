//Haar_Firmware_Test.ino

#include "SlowSoftI2CMaster.h"
#include <avr/sleep.h>
#include <avr/power.h>
#include "WireS.h"
#include <EEPROM.h>


#define SHT31_ADDR    0x45
#define LPS35HW_ADDR 	0x5D ///< LPS35HW default i2c address

#define SHT31_MEAS_HIGHREP_STRETCH 0x2C06
#define SHT31_MEAS_MEDREP_STRETCH  0x2C0D
#define SHT31_MEAS_LOWREP_STRETCH  0x2C10
#define SHT31_MEAS_HIGHREP         0x2400
#define SHT31_MEAS_MEDREP          0x240B
#define SHT31_MEAS_LOWREP          0x2416
#define SHT31_READSTATUS           0xF32D
#define SHT31_CLEARSTATUS          0x3041
#define SHT31_SOFTRESET            0x30A2
#define SHT31_HEATEREN             0x306D
#define SHT31_HEATERDIS            0x3066

#define LPS35HW_CTRL_REG1     0x10 ///< Control register 1
#define LPS35HW_CTRL_REG2     0x11 ///< Control register 2
#define LPS35HW_CTRL_REG3     0x12 ///< Control register 3
#define LPS35HW_PRESS_OUT_XL  0x28 ///< Pressure low byte
#define LPS35HW_PRESS_OUT_L   0x29 ///< Pressure mid byte
#define LPS35HW_PRESS_OUT_H   0x2A ///< Pressure high byte
#define LPS35HW_TEMP_OUT_L    0x2B ///< Temperature low byte
#define LPS35HW_TEMP_OUT_H    0x2C ///< Temperature high byte

#define LPS35HW_CTRL_REG2_DEFAULT 0x10

#define READ 0x01

//Firmware patch version: bump on any behavioural change visible to the
//library. The hardware version lives in Page 0 (EEPROM), written at
//provisioning; the firmware writes this constant into the served copy of
//Page 0 at 0x0A and recomputes the CRC there (NW-Device-Specification).
#define FW_FW_PATCH 1

//Page 0 (identity, 32 bytes) is the top of EEPROM: 0xE0-0xFF on the
//ATtiny1634's 256-byte EEPROM. Written once by NW-Provision; read at boot.
#define PAGE0_BASE   (E2END + 1 - 32)
#define REG_I2C_ADDR 0x1F
#define ADR_DEFAULT  0x48  //Schema 1 'H'; used when Page 0 byte 0x1F is 0xFF (was 0x42)

//Page 1 Block 0 (NW-Device-Specification): universal status and control.
#define REG_STATUS   0x20
#define REG_CTRL     0x21
#define REG_COUNTER  0x22
#define REG_REQUEST  0x24  //Readings requested, uint16 LE, writable; Haar has no chip power to hold, so it only accepts the write
#define REG_CONFIG   0x26  //No bits defined for Haar
#define REG_FAULT    0x27
#define BIT_READY    0x01
#define BIT_PANFAULT 0x80
#define BIT_TRIGGER  0x01
#define CHIP_SHT31   0x02  //Control chip-select bit and status fault bit: chip 0
#define CHIP_LPS35HW 0x04  //chip 1
#define BIT_SLEEP    0x80
#define FAULT_SHT31_NOACK     0x01  //chip 0, kind 1
#define FAULT_SHT31_CHECKSUM  0x03  //chip 0, kind 3: the SHT31's own CRC failed
#define FAULT_LPS35HW_NOACK   0x21  //chip 1, kind 1
#define FAULT_LPS35HW_TIMEOUT 0x22  //chip 1, kind 2: ONE_SHOT never cleared
#define FAULT_UNIT_RESET      0xE6  //unit (7), kind 6: reset since the controller last wrote Control
#define FAULT_UNIT_PAGE0      0xE3  //unit (7), kind 3: Page 0 CRC did not match (unprovisioned or corrupt)
#define WRITE 0x00

unsigned long ReadTimeout = 100; //Wait at most 100ms for new read

uint8_t Config = 0; //Global config value

uint8_t Reg[64] = {0}; //Initialize registers; 0x00-0x1F = Page 0 (identity), 0x20-0x27 = Page 1 Block 0 (status/control), 0x28-0x3F = Page 1 sensor data
bool page0Valid = false; //Page 0 CRC matched what NW-Provision wrote
bool Sample = true; //Flag used to start a new converstion
bool Sleep = false; //Used to put the device into deep sleep //ADD
bool Startup = false;
bool shtNoAck = false; //SHT31 did not acknowledge during the last reading
bool shtCrcFail = false; //SHT31 data failed its own CRC during the last reading
bool lpsNoAck = false; //LPS35HW did not acknowledge during the last reading

uint16_t ST, SRH; //Global values for RH sensor (FIX!!!!)

volatile uint8_t ADR = ADR_DEFAULT; //I2C address: Page 0 byte 0x1F (EEPROM), or ADR_DEFAULT if unprogrammed

SlowSoftI2CMaster si = SlowSoftI2CMaster(PIN_C4, PIN_C5, true);  //Initialize software I2C

volatile bool StopFlag = false; //Used to indicate a stop condition
volatile uint8_t RegID = 0; //Used to denote which register will be read from
volatile bool RepeatedStart = false; //Used to show if the start was repeated or not

void setup() {
	pinMode(15, OUTPUT); //DEBUG!
	digitalWrite(15, HIGH); //DEBUG!
	Reg[REG_CONFIG] = 0x00; //Set Config to POR value
	loadPage0();
	if(Reg[REG_I2C_ADDR] != 0xFF) ADR = Reg[REG_I2C_ADDR]; //Provisioned address; 0xFF = use default
	Reg[REG_STATUS] = 0; //Not ready: no reading yet
	Reg[REG_CTRL] = CHIP_SHT31 | CHIP_LPS35HW; //Power-up: every chip selected
	Reg[REG_FAULT] = page0Valid ? FAULT_UNIT_RESET : FAULT_UNIT_PAGE0; //Latched until the controller writes Control
	Wire.begin(ADR);  //Begin slave I2C

	Wire.onAddrReceive(addressEvent); // register event
	Wire.onRequest(requestEvent);     // register event
	Wire.onReceive(receiveEvent);
	Wire.onStop(stopEvent);

  	si.i2c_init(); //Begin I2C master
  	// Serial.begin(4800); //DEBUG!
  	RHreset(); //Reset RH sensor on startup
  	PresReset(); //Reset pressure sensor on startup

  	WriteByte(LPS35HW_ADDR, LPS35HW_CTRL_REG3, 0x40);; //Setup 50Hz data rate //DEBUG!

}

void loop() {
	Sample = BitRead(Reg[REG_CTRL], 0); //Trigger: Control bit 0 (on-demand only; no free-running cycle)

	if(Sample == true) {
		//A reading begins: clear ready, take the chip selection, consume the trigger.
		Reg[REG_STATUS] &= ~BIT_READY;
		bool doSHT = Reg[REG_CTRL] & CHIP_SHT31;
		bool doLPS = Reg[REG_CTRL] & CHIP_LPS35HW;
		Reg[REG_CTRL] &= ~(BIT_TRIGGER | BIT_SLEEP); //trigger consumed; sleep not implemented
		shtNoAck = shtCrcFail = lpsNoAck = false;
		bool presDone = true;
		if(doLPS) WriteByte(LPS35HW_ADDR, LPS35HW_CTRL_REG2, LPS35HW_CTRL_REG2_DEFAULT | 0x01); //Set ONE_SHOT bit in order to trigger new conversion for pressure
		if(doSHT) {
		if(!readRH()) shtCrcFail = true; //Get new temp/RH values
		SplitAndLoad(0x28, (unsigned int)(int16_t)((ST * 17500UL) / 65535UL - 4500)); //Schema 1: temp SHT31, int16, 0.01 C (Block 1); -45 + 175*ST/65535
		SplitAndLoad(0x2A, (unsigned int)((SRH * 10000UL) / 65535UL)); //Schema 1: humidity, uint16, 0.01 %RH (Block 1); 100*SRH/65535
		}
		if(doLPS) presDone = ReadPres(); //FIX!!! Make non-blocking/parellel conversion

		//Reading complete: load status and fault, bump the counter, set ready.
		//Atomic so a controller's page read never straddles the update.
		uint8_t status = BIT_READY;
		if(doSHT && shtNoAck) { status |= CHIP_SHT31; Reg[REG_FAULT] = FAULT_SHT31_NOACK; }
		else if(doSHT && shtCrcFail) { status |= CHIP_SHT31; Reg[REG_FAULT] = FAULT_SHT31_CHECKSUM; }
		if(doLPS && lpsNoAck) { status |= CHIP_LPS35HW; Reg[REG_FAULT] = FAULT_LPS35HW_NOACK; }
		else if(doLPS && !presDone) { status |= CHIP_LPS35HW; Reg[REG_FAULT] = FAULT_LPS35HW_TIMEOUT; }
		if(status & 0x7E) status |= BIT_PANFAULT;
		uint16_t count = Reg[REG_COUNTER] | (Reg[REG_COUNTER + 1] << 8);
		count++;
		cli();
		Reg[REG_COUNTER] = count & 0xFF; Reg[REG_COUNTER + 1] = count >> 8;
		Reg[REG_STATUS] = status;
		sei();
		Sample = false; //Clear sample bit
		Startup = true; //Set after first conversion
	}

	//Sleep after loading registers
	// ADCSRA &= ~(1<<ADEN); //Disable ADC
	// SPCR   &= ~_BV(SPE); //Disable SPI
	//    PRR = 0xFF;
  	digitalWrite(15, LOW); //DEBUG!

	if(Sleep) set_sleep_mode(SLEEP_MODE_PWR_DOWN);
	else set_sleep_mode(SLEEP_MODE_STANDBY);
	sleep_enable();
	sleep_mode(); //Waits here while in sleep mode

	sleep_disable(); //Wake up
	TWSCRA = bit(TWEN);  //Re-enable I2C
	Wire.begin(ADR);
	si.i2c_init(); //Begin I2C master


	delay(10); //DEBUG!
}

uint16_t readStatusRH(void) {
  writeCommand(SHT31_READSTATUS);
  si.i2c_start((SHT31_ADDR << 1) | READ);
  // Wire.requestFrom(SHT31_ADDR, (uint8_t)3);
  uint16_t stat = si.i2c_read(false);;
  stat <<= 8;
  stat |= si.i2c_read(false);;
  //Serial.println(stat, HEX);
  return stat;
}

void RHreset(void) {
  writeCommand(SHT31_SOFTRESET);
  delay(10);
}

void PresReset(void) {
	Serial.println(WriteByte(LPS35HW_ADDR, LPS35HW_CTRL_REG2, 0x04)); //Set bit 2 in CtrlReg2 to force a software reset //DEBUG!
}

void heater(boolean h) {
  if (h)
    writeCommand(SHT31_HEATEREN);
  else
    writeCommand(SHT31_HEATERDIS);
}



// int16_t readTemperatureRH(void) {
//   if (! readTempHum()) return NAN;

//   return ST;
// }


// uint16_t readHumidity(void) {
//   if (! readTempHum()) return NAN;

//   return SRH;
// }
bool ReadPres(void) {
	// WriteByte(LPS35HW_ADDR, LPS35HW_CTRL_REG2, LPS35HW_CTRL_REG2_DEFAULT | 0x01); //Set ONE_SHOT bit in order to trigger new conversion
	unsigned long Timeout = millis();
	bool Done = false; //Flag to wait for new data
	while((millis() - Timeout) < ReadTimeout && !Done) {
		if(BitRead(ReadByte(LPS35HW_ADDR, LPS35HW_CTRL_REG2), 0) == 0) { //Wait for ONE_SHOT to be cleared
			Done = true; //Set flag
		}
	}
	if(Done) {  //If read succesfully
		uint32_t PresRaw = ReadByte(LPS35HW_ADDR, LPS35HW_PRESS_OUT_XL); //Read out LSB
		PresRaw |= (uint32_t)ReadByte(LPS35HW_ADDR, LPS35HW_PRESS_OUT_L) << 8; //Read out Mid byte
		PresRaw |= (uint32_t)ReadByte(LPS35HW_ADDR, LPS35HW_PRESS_OUT_H) << 16; //Read out MSB
		SplitAndLoad(0x30, long((PresRaw * 25UL) / 1024UL)); //Schema 1: pressure, uint32, 0.01 hPa (Block 2); raw/4096 hPa

		unsigned int TempRaw = ReadByte(LPS35HW_ADDR, LPS35HW_TEMP_OUT_L); //Read out LSB
		// Reg[0x02] = 10;
		TempRaw |= ReadByte(LPS35HW_ADDR, LPS35HW_TEMP_OUT_H) << 8; //Read out MSB
		SplitAndLoad(0x34, TempRaw); //Schema 1: temp LPS35HW, int16, 0.01 C (Block 2); the chip's own unit
	}
	return Done; //Return valid status
}

boolean readRH(void) {
  uint8_t readbuffer[6];

  writeCommand(SHT31_MEAS_HIGHREP);

  delay(30);  //FIX! Read byte and test for NACK value
  // Wire.requestFrom(SHT31_ADDR, (uint8_t)6);
  si.i2c_start((SHT31_ADDR << 1) | READ);
  // if (Wire.available() != 6) //FIX??
    // return false;
  for (uint8_t i=0; i<6; i++) {
    readbuffer[i] = si.i2c_read(false);
  //  Serial.print("0x"); Serial.println(readbuffer[i], HEX);
  }
  si.i2c_stop();
  // uint16_t ST, SRH;
  ST = readbuffer[0];
  ST <<= 8;
  ST |= readbuffer[1];

  if (readbuffer[2] != crc8(readbuffer, 2)) return false;

  SRH = readbuffer[3];
  SRH <<= 8;
  SRH |= readbuffer[4];

  if (readbuffer[5] != crc8(readbuffer+3, 2)) return false;

//  // Serial.print("ST = "); Serial.println(ST);
//   double stemp = ST;
//   stemp *= 175;
//   stemp /= 0xffff;
//   stemp = -45 + stemp;
//   temp = stemp;

// //  Serial.print("SRH = "); Serial.println(SRH);
//   double shum = SRH;
//   shum *= 100;
//   shum /= 0xFFFF;

//   humidity = shum;

  return true;
}

void writeCommand(uint16_t cmd) {
  // Wire.beginTransmission(SHT31_ADDR);
  if(!si.i2c_start((SHT31_ADDR << 1) | WRITE)) shtNoAck = true; //No acknowledge: chip 0 fault on this reading
  si.i2c_write(cmd >> 8);
  si.i2c_write(cmd & 0xFF);
  // Wire.endTransmission();
  si.i2c_stop();
}

uint8_t crc8(const uint8_t *data, int len)
{
/*
*
 * CRC-8 formula from page 14 of SHT spec pdf
 *
 * Test data 0xBE, 0xEF should yield 0x92
 *
 * Initialization data 0xFF
 * Polynomial 0x31 (x8 + x5 +x4 +1)
 * Final XOR 0x00
 */

  const uint8_t POLYNOMIAL(0x31);
  uint8_t crc(0xFF);

  for ( int j = len; j; --j ) {
      crc ^= *data++;

      for ( int i = 8; i; --i ) {
	crc = ( crc & 0x80 )
	  ? (crc << 1) ^ POLYNOMIAL
	  : (crc << 1);
      }
  }
  return crc;
}


/////////// Utility Functions///////////////
//CRC-8/SMBUS (poly 0x07, init 0x00), the NW-Device-Specification reference
//for Page 0. (crc8() above is the SHT31's own CRC: poly 0x31, init 0xFF.)
uint8_t crc8smbus(const uint8_t* data, uint8_t len) {
	uint8_t crc = 0x00;
	for(uint8_t i = 0; i < len; i++) {
		crc ^= data[i];
		for(uint8_t b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
	}
	return crc;
}

//Copy Page 0 from EEPROM into the served register array, check its CRC,
//then substitute this firmware's patch version at 0x0A and recompute the
//CRC of the served copy (EEPROM is left as provisioned).
void loadPage0() {
	for(uint8_t i = 0; i < 32; i++) Reg[i] = EEPROM.read(PAGE0_BASE + i);
	page0Valid = (crc8smbus(Reg, 0x1E) == Reg[0x1E]) && Reg[0x00] == 0x01;
	Reg[0x0A] = FW_FW_PATCH;
	Reg[0x1E] = crc8smbus(Reg, 0x1E);
}

//Registers a controller may write. Everything else is read-only and writes
//to it are ignored (NW-Device-Specification, Page 1 rules).
bool isWritable(uint8_t pos) {
	return pos == REG_CTRL || pos == REG_CONFIG || pos == REG_I2C_ADDR
	    || pos == REG_REQUEST || pos == REG_REQUEST + 1;
}

bool BitRead(uint8_t Val, uint8_t Pos) //Read the bit value at the specified position
{
	return (Val >> Pos) & 0x01;
}

uint8_t SendCommand(uint8_t Adr, uint8_t Command)
{
    if(!si.i2c_start((Adr << 1) | WRITE) && Adr == LPS35HW_ADDR) lpsNoAck = true; //No acknowledge: chip 1 fault on this reading
    bool Error = si.i2c_write(Command);
    // si.i2c_stop(); //DEBUG!
    return 1; //DEBUG!
}

uint8_t WriteWord(uint8_t Adr, uint8_t Command, unsigned int Data)  //Writes value to 16 bit register
{
	si.i2c_start((Adr << 1) | WRITE);
	si.i2c_write(Command); //Write Command value
	si.i2c_write(Data & 0xFF); //Write LSB
	uint8_t Error = si.i2c_write((Data >> 8) & 0xFF); //Write MSB
	si.i2c_stop();
	return Error;  //Invert error so that it will return 0 is works
}

uint8_t WriteByte(uint8_t Adr, uint8_t Command, uint8_t Data)  //Writes value to 16 bit register
{
	if(!si.i2c_start((Adr << 1) | WRITE) && Adr == LPS35HW_ADDR) lpsNoAck = true; //No acknowledge: chip 1 fault on this reading
	si.i2c_write(Command); //Write Command value
	uint8_t Error = si.i2c_write((Data) & 0xFF); //Write MSB
	si.i2c_stop();
	return Error;  //Invert error so that it will return 0 is works
}

uint8_t WriteWord_LE(uint8_t Adr, uint8_t Command, unsigned int Data)  //Writes value to 16 bit register
{
	si.i2c_start((Adr << 1) | WRITE);
	si.i2c_write(Command); //Write Command value
	si.i2c_write((Data >> 8) & 0xFF); //Write MSB
	si.i2c_write(Data & 0xFF); //Write LSB
	si.i2c_stop();
	// return Error;  //Invert error so that it will return 0 is works
}

// uint8_t WriteConfig(uint8_t Adr, uint8_t NewConfig)
// {
// 	si.i2c_start((Adr << 1) | WRITE);
// 	si.i2c_write(CONF_CMD);  //Write command code to Config register
// 	uint8_t Error = si.i2c_write(NewConfig);
// 	si.i2c_stop();
// 	if(Error == true) {
// 		Config = NewConfig; //Set global config if write was sucessful
// 		return 0;
// 	}
// 	else return -1; //If write failed, return failure condition
// }

int ReadByte(uint8_t Adr, uint8_t Command, uint8_t Pos) //Send command value, and high/low byte to read, returns desired byte
{
	bool Error = SendCommand(Adr, Command);
	si.i2c_rep_start((Adr << 1) | READ);
	uint8_t ValLow = si.i2c_read(false);
	uint8_t ValHigh = si.i2c_read(false);
	si.i2c_stop();
	Error = true; //DEBUG!
	if(Error == true) {
		if(Pos == 0) return ValLow;
		if(Pos == 1) return ValHigh;
	}
	else return -1; //Return error if read failed

}

int ReadByte(uint8_t Adr, uint8_t Command) //Send command value, and high/low byte to read, returns desired byte
{
	bool Error = SendCommand(Adr, Command);
	si.i2c_rep_start((Adr << 1) | READ);
	// si.i2c_start((Adr << 1) | READ);
	uint8_t Val = si.i2c_read(true);
	si.i2c_stop();
	Error = true; //DEBUG!
	if(Error == true) {
		return Val;
	}
	else return -1; //Return error if read failed

}

int ReadWord(uint8_t Adr, uint8_t Command)  //Send command value, returns entire 16 bit word
{
	bool Error = SendCommand(Adr, Command);
	Serial.print("Error = "); Serial.println(Error); //DEBUG!
	si.i2c_rep_start((Adr << 1) | READ);
	uint8_t ByteLow = si.i2c_read(false);  //Read in high and low bytes (big endian)
	uint8_t ByteHigh = si.i2c_read(false);
	si.i2c_stop();
	// if(Error == true) return ((ByteHigh << 8) | ByteLow); //If read succeeded, return concatonated value
	// else return -1; //Return error if read failed
	return ((ByteHigh << 8) | ByteLow); //DEBUG!
}

int ReadWord_LE(uint8_t Adr, uint8_t Command)  //Send command value, returns entire 16 bit word
{
	bool Error = SendCommand(Adr, Command);
	si.i2c_stop();
	si.i2c_start((Adr << 1) | READ);
	uint8_t ByteHigh = si.i2c_read(false);  //Read in high and low bytes (big endian)
	uint8_t ByteLow = si.i2c_read(false);
	si.i2c_stop();
	// if(Error == true) return ((ByteHigh << 8) | ByteLow); //If read succeeded, return concatonated value
	// else return -1; //Return error if read failed
	return ((ByteHigh << 8) | ByteLow); //DEBUG!
}

void SplitAndLoad(uint8_t Pos, unsigned int Val) //Write 16 bits
{
	uint8_t Len = sizeof(Val);
	for(int i = Pos; i < Pos + Len; i++) {
		Reg[i] = (Val >> (i - Pos)*8) & 0xFF; //Pullout the next byte
	}
}

void SplitAndLoad(uint8_t Pos, long Val)  //Write 32 bits
{
	uint8_t Len = sizeof(Val);
	for(int i = Pos; i < Pos + Len; i++) {
		Reg[i] = (Val >> (i - Pos)*8) & 0xFF; //Pullout the next byte
	}
}

boolean addressEvent(uint16_t address, uint8_t count)
{
	RepeatedStart = (count > 0 ? true : false);
	return true; // send ACK to master
}

void requestEvent()
{
	//Serve up to one full page from the requested register with auto-increment.
	//WireS clocks out only as many bytes as the controller asks for; the rest
	//of the buffer is discarded at the stop condition. Reads past the end of
	//the array wrap, so a controller never receives bytes from outside it.
	for(uint8_t i = 0; i < 32; i++) {
		Wire.write(Reg[(RegID + i) % sizeof(Reg)]);
	}
}

void receiveEvent(int DataLen)
{
    //Write data to appropriate location
    if(DataLen == 2){
	    //Remove while loop??
	    while(Wire.available() < 2); //Only option for writing would be register address, and single 8 bit value
	    uint8_t Pos = Wire.read();
	    uint8_t Val = Wire.read();
	    //Check for validity of write??
	    if(!isWritable(Pos)) return; //Read-only register: ignore the write
	    Reg[Pos] = Val; //Set register value
	    if(Pos == REG_CTRL) Reg[REG_FAULT] = 0; //A control write acknowledges the latched fault
	    if(Pos == REG_I2C_ADDR) EEPROM.update(PAGE0_BASE + REG_I2C_ADDR, Val); //Persist I2C address (compare-before-write); takes effect on next boot
	}

	if(DataLen == 1){
		RegID = Wire.read(); //Read in the register ID to be used for subsequent read
	}
}

void stopEvent()
{
	StopFlag = true;
	//End comunication
}
