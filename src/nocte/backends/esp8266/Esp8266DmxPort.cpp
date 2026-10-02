/**************************************************************************/
/*!
    @file     Esp8266DmxPort.cpp
    @author   Claude Heintz
    @license  BSD (see LXESP8266UARTDMX.h)
    @copyright 2015-2016 by Claude Heintz

    DMX Driver for ESP8266 using UART0.

    @section  HISTORY

    v1.0 - First release
    v1.1 - Consolidated Output and Input into a single class
    v2.1 - add RDM controller support
    v2.2 - RDM device support
*/
/**************************************************************************/

#if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "Arduino.h"
#include "cbuf.h"
#include <interrupts.h>

extern "C" {
#include "osapi.h"
#include "ets_sys.h"
#include "mem.h"
#include "user_interface.h"
}

#include "Esp8266DmxPort.h"
#include "../../core/DmxFrame.h"
#include "../../core/RdmPacket.h"
#include <rdm/rdm_utility.h>

LX8266DMX ESP8266DMX;
static LX8266DMX* uartOwner = nullptr;

UID LX8266DMX::THIS_DEVICE_ID(0x6C, 0x78, 0x00, 0x00, 0x00, 0x01);

/* ***************** Utility functions derived from ESP HardwareSerial.cpp  ****************
   HardwareSerial.cpp - esp8266 UART support - Copyright (c) 2014 Ivan Grokhotkov. All rights reserved.
   HardwareSerial is licensed under GNU Lesser General Public License
   HardwareSerial is included in the esp8266 by ESP8266 Community board package for Arduino IDE
*/

//these constants are now defined in the Arduino ESP8266 library v2.1.0
//static const int UART0 = 0;
//static const int UART0 = 1;
//static const int UART_NO = -1;


/**
 *  UART GPIOs
 *
 *  UART0 is used for both DMX output and input.  Serial debug output cannot
 *  be used while DMX is active.
 *
 *
 * UART0 TX: 1 or 2				SPECIAL or FUNCTION_4
 * UART0 RX: 3						SPECIAL
 *
 * UART0 SWAP TX: 15
 * UART0 SWAP RX: 13
 *
 *
 * UART1 TX: 2 or 7 (NC)		SPECIAL or FUNCTION_4
 * UART1 RX: 8 (NC)				FUNCTION_4
 *
 * UART1 SWAP TX: 11 (NC)
 * UART1 SWAP RX: 6 (NC)
 *
 * NC = Not Connected to Module Pads --> No Access
 *      Pins 6-11 typically connected to flash on most modules
 *      see http://arduino.esp8266.com/versions/1.6.5-1160-gef26c5f/doc/reference.html
 */
 
void uart_tx_interrupt_handler(void* argument, void* context);
void uart_rx_interrupt_handler(void* argument, void* context);
void uart_rdm_interrupt_handler(void* argument, void* context);
void uart__tx_flush(void);
void uart__rx_flush(void);
void uart_enable_rx_interrupt(LX8266DMX* dmxi);
void uart_disable_rx_interrupt(void);
void uart_enable_tx_interrupt(LX8266DMX* dmxo);
void uart_disable_tx_interrupt(void);
void uart_set_baudrate(int uart_nr, int baud_rate);

void uart_init_tx(int baudrate, byte config, LX8266DMX* dmxo);
void uart_init_rx(int baudrate, byte config, LX8266DMX* dmxi);
void uart_init_rdm(int baudrate, byte config, int txbaudrate, byte txconfig, LX8266DMX* dmxr);

void uart_uninit_tx(void);
void uart_uninit_rx(void);
void uart_uninit_rdm(void);

// ####################################################################################################

// UART register definitions see esp8266_peri.h

IRAM_ATTR void uart_tx_interrupt_handler(void* argument, void*) {
    LX8266DMX* dmxo = static_cast<LX8266DMX*>(argument);

    // -------------- UART 1 --------------
    // check uart status register 
    // if fifo is empty clear interrupt
    // then call _tx_empty_irq
	  if(U0IS & (1 << UIFE)) {
			U0IC = (1 << UIFE);
			dmxo->txEmptyInterruptHandler();
	  }
	 
}

IRAM_ATTR void uart_rx_interrupt_handler(void* argument, void*) {
    LX8266DMX* dmxi = static_cast<LX8266DMX*>(argument);
	  uint32_t status = U0IS;

    // -------------- UART 0 --------------
    // check uart status register 
    // if read buffer is full, call receiveInterruptHandler and then clear interrupt

	  while ((USS(UART0) >> USRXC) & 0xff) {
			dmxi->byteReceived((char) (U0F & 0xff));
	  }
     
     // if break detected, call receiveInterruptHandler and then clear interrupt
     if ( (status & (1 << UIBD)) ) {				//break detected
     		dmxi->breakReceived();
     } else if ( (status & (1 << UITO)) ) {			//rx idle timeout
     		dmxi->receiveTimeout();
     }

     U0IC = status;
}

IRAM_ATTR void uart_rdm_interrupt_handler(void* argument, void*) {
    LX8266DMX* dmxr = static_cast<LX8266DMX*>(argument);
	  uint32_t status = U0IS;

    // -------------- UART 0 --------------
    // check uart status register 
    // if read buffer is full, call receiveInterruptHandler and then clear interrupt

	  while((USS(UART0) >> USRXC) & 0xff) {
			dmxr->byteReceived((char) (U0F & 0xff));
	  }
     
     // if break detected, call receiveInterruptHandler and then clear interrupt
     if ( (status & (1 << UIBD)) ) {				//break detected
     		dmxr->breakReceived();
     } else if ( (status & (1 << UITO)) ) {			//rx idle timeout
     		dmxr->receiveTimeout();
     }
     
     if ( dmxr->rdmTaskMode() ) {
		 if (status & (1 << UIFE)) {
			dmxr->rdmTxEmptyInterruptHandler();
		 }
     }

     U0IC = status;
}


// ####################################################################################################

//LX uses UART0 for tx
void uart_tx_flush(void) {
    uint32_t tmp = 0x00000000;

    tmp |= (1 << UCTXRST);
    
    USC0(UART0) |= (tmp);
    USC0(UART0) &= ~(tmp);
}

//LX uses uart0 for rx
void uart_rx_flush(void) {
    uint32_t tmp = 0x00000000;

    tmp |= (1 << UCRXRST);

    USC0(UART0) |= (tmp);
    USC0(UART0) &= ~(tmp);
}


// ------------- uart_enable/disable RX functions

//LX uses uart0 for rx
void uart_enable_rx_interrupt(LX8266DMX* dmxi) {
	USIC(UART0) = 0x1ff;
	ETS_UART_INTR_ATTACH(&uart_rx_interrupt_handler, dmxi);
    USIE(UART0) |= (1 << UIFF);   //receive full
    USIE(UART0) |= (1 << UITO);   //receive timeout
    //USIE(UART0) |= (1 << UIFR); frame error
    USIE(UART0) |= (1 << UIBD);   //break detected
    ETS_UART_INTR_ENABLE();
}

//LX uses uart0 for rx
void uart_disable_rx_interrupt(void) {
   USIE(UART0) &= ~(1 << UIFF);   //receive full
   USIE(UART0) &= ~(1 << UITO);   //receive timeout
   USIE(UART0) &= ~(1 << UIBD);   //break detected
   //ETS_UART_INTR_DISABLE();		disables all UART interrupts including Hardware serial
}

// ------------- uart_enable/disable TX functions
//LX uses UART0 for tx
void uart_enable_tx_interrupt(LX8266DMX* dmxo) {
	USIC(UART0) = 0x1ff;								//clear interrupts	
	ETS_UART_INTR_ATTACH(&uart_tx_interrupt_handler, dmxo);
   USIE(UART0) |= (1 << UIFE);							//enable fifo empty interrupt
   ETS_UART_INTR_ENABLE();
}

//LX uses UART0 for tx
void uart_disable_tx_interrupt(void) {
   USIE(UART0) &= ~(1 << UIFE);
   //ETS_UART_INTR_DISABLE();		disables all UART interrupts including Hardware serial
}

// ------------- uart_enable/disable RDM functions

void uart_enable_rdm_interrupts(LX8266DMX* dmxr) {
//TX
	USIC(UART0) = 0x1ff;
	if (dmxr->rdmTaskMode()) {		//only enable if in send task mode
		USIE(UART0) |= (1 << UIFE);
	}

//RX
	USIC(UART0) = 0x1ff;
    USIE(UART0) |= (1 << UIFF);   //receive full
    USIE(UART0) |= (1 << UITO);   //receive timeout
    USIE(UART0) |= (1 << UIBD);   //break detected
    
    ETS_UART_INTR_ATTACH(&uart_rdm_interrupt_handler, dmxr);
    ETS_UART_INTR_ENABLE();
}

void uart_disable_rdm_interrupts(void) {
//TX
   USIE(UART0) &= ~(1 << UIFE);
//RX
   USIE(UART0) &= ~(1 << UIFF);
   USIE(UART0) &= ~(1 << UITO);
}

// ------------- uart_set functions

//LX uses UART0 for tx, uart0 for rx
void uart_set_baudrate(int uart_nr, int baud_rate) {
    USD(uart_nr) = (ESP8266_CLOCK / baud_rate);
}

//LX uses UART0 for tx, uart0 for rx
void uart_set_config(int uart_nr, byte config) {
    USC0(uart_nr) = config;
}

// ------------- uart_init functions

void uart_init_tx(int baudrate, byte config, LX8266DMX* dmxo) {
	pinMode(1, SPECIAL);
	uint32_t conf1 = 0x00000000;
	
    uart_set_baudrate(UART0, baudrate);
    USC0(UART0) = config;
    uart_tx_flush();
    uart_enable_tx_interrupt(dmxo);

    //conf1 |= (0x00 << UCFET);// tx empty threshold is zero
    						   // tx fifo empty interrupt triggers continuously unless
    						   // data register contains a byte which has not moved to shift reg yet
    USC1(UART0) = conf1;
}

void uart_init_rx(int baudrate, byte config, LX8266DMX* dmxi) {
    uint32_t conf1 = 0x00000000;
    pinMode(3, SPECIAL);
    uart_set_baudrate(UART0, baudrate);
    USC0(UART0) = config;
    
    conf1 |= (0x01 << UCFFT);
    conf1 |= (DMX_RX_TIMEOUT_THRESHOLD << UCTOT);
    conf1 |= (1 << UCTOE);
    USC1(UART0) = conf1;

    uart_rx_flush();
    uart_enable_rx_interrupt(dmxi);
}

void uart_init_rdm(int baudrate, byte config, int txbaudrate, byte txconfig, LX8266DMX* dmxr) {
//TX
	pinMode(1, SPECIAL);
	uint32_t conf1 = 0x00000000;
	
    uart_set_baudrate(UART0, txbaudrate);
    USC0(UART0) = txconfig;
    uart_tx_flush();

    //conf1 |= (0x00 << UCFET);// tx empty threshold is zero
    						   // tx fifo empty interrupt triggers continuously unless
    						   // data register contains a byte which has not moved to shift reg yet
    USC1(UART0) = conf1;

//RX
    conf1 = 0x00000000;
    pinMode(3, SPECIAL);
    uart_set_baudrate(UART0, baudrate);
    USC0(UART0) = config;
    
    conf1 |= (0x01 << UCFFT);
    conf1 |= (DMX_RX_TIMEOUT_THRESHOLD << UCTOT);
    conf1 |= (1 << UCTOE);
    USC1(UART0) = conf1;

    uart_rx_flush();
    
    uart_enable_rdm_interrupts(dmxr);
}

// ------------- uart_uninit functions

void uart_uninit_tx(void) {
    uart_disable_tx_interrupt();
	 pinMode(1, INPUT);
}

void uart_uninit_rx(void) {
    uart_disable_rx_interrupt();
	 pinMode(3, INPUT);
}

void uart_uninit_rdm(void) {
    uart_uninit_rx();
	uart_uninit_tx();
}

// **************************** global data (can be accessed in ISR)  ***************

// UART register definitions see esp8266_peri.h

#define DMX_DATA_BAUD		250000
#define DMX_BREAK_BAUD 	 	88000
// E1.20 requires controller-generated RDM breaks to be 176-352 us.  A zero
// byte at 50 kbaud with the break framing used below produces about 180 us on
// the ESP8266, while the shorter DMX break remains unchanged.
#define RDM_BREAK_BAUD       50000
#define RDM_CONTROLLER_BREAK_US 200
#define RDM_CONTROLLER_MAB_US 20
#define RDM_CONTROLLER_TX_DRAIN_US 60
#define RDM_UART_FIFO_FULL_LEVEL 0x7f
#define RDM_TRANSMIT_TIMEOUT_US 35000
#define RDM_TRANSMIT_TAIL_TIMEOUT_US 120
#define RDM_DISCOVERY_RESPONSE_WAIT_MS 6
#define RDM_CONTROLLER_RESPONSE_START_WAIT_US 3000
// E1.20 permits up to 2.8 ms at the controller before the response and up to
// 2.1 ms of individual inter-slot delay on received responder packets. A
// conforming transmitter must additionally keep average inter-slot delay at
// or below 76 us, so a maximum-sized response completes in about 31 ms.
#define RDM_CONTROLLER_RESPONSE_FRAME_WAIT_US 32000
#define RDM_CONTROLLER_BROADCAST_SPACING_US 176
// Receiver timestamps are taken at the end of each 44 us slot. E1.20-2025
// Table 3-1 therefore maps the 2.1 ms maximum inter-slot delay to a 2144 us
// maximum interval between successive byte timestamps.
#define RDM_RESPONDER_MAX_SLOT_INTERVAL_US 2144
// At the controller port, the earliest acceptable first response slot ends
// after 176 us packet spacing, an 88 us received BREAK, an 8 us MAB, and one
// 44 us slot (E1.20-2025 Tables 3-1 and 3-2).
#define RDM_RESPONSE_FIRST_SLOT_MIN_US 316
#define RDM_SLOT_WIRE_US 44
/*
#define UART_STOP_BIT_NUM_SHIFT  4
TWO_STOP_BIT             = 0x3
ONE_STOP_BIT             = 0x1,

#define UART_BIT_NUM_SHIFT       2
EIGHT_BITS = 0x3

parity
#define UCPAE   1  //Parity Enable			(possibly set for none??)
#define UCPA    0  //Parity 0:even, 1:odd

111100 = 8n2  = 60 = 0x3C  (or 0x3E if bit1 is set for no parity)
011100 = 8n1  = 28 = 0x1C

*/

#define FORMAT_8N2			0x3C
#define FORMAT_8E1			0x1C


 //***** states indicate current position in DMX stream
    #define DMX_STATE_BREAK 0
    #define DMX_STATE_START 1
    #define DMX_STATE_DATA 2
    #define DMX_STATE_IDLE 3
	#define DMX_STATE_BREAK_SENT 4
	
	//***** interrupts to wait before changing Baud
    #define DATA_END_WAIT 50		//initially was 25 with processor at 80 mHz  set to 50 @ 160mHz
    #define BREAK_SENT_WAIT 80		//initially was 70 with processor at 80 mHz  set to 80 @ 160mHz

	//***** status is if interrupts are enabled and IO is active
    #define ISR_DISABLED 		0
    #define ISR_OUTPUT_ENABLED 	1
    #define ISR_INPUT_ENABLED 	2
    #define ISR_RDM_ENABLED 	3


/*******************************************************************************
 ***********************  LX8266DMX member functions  ********************/

LX8266DMX::LX8266DMX ( void ) {
	_direction_pin = DIRECTION_PIN_NOT_USED;	//optional
	_receiver_enable_not_pin = DIRECTION_PIN_NOT_USED;	//optional split /RE
	_frames.slots = DMX_MAX_SLOTS;
	_interrupt_status = ISR_DISABLED;
	_dmx_send_state = DMX_STATE_IDLE;
	_dmx_read_state = DMX_READ_STATE_IDLE;
	_idle_count = 0;
	_rdm_task_mode = DMX_TASK_RECEIVE;
	_rdm.handled = 0;
	_rdm.breakSeen = 0;
	_rdm.lastSlotUs = 0;
	_rdm.maximumSlotIntervalUs = 0;
	_rdm.requestEndUs = 0;
	_rdm.firstSlotDelayUs = 0;
	_rdm.validationFailures = 0;
	_rdm.responseLength = 0;
	_rdm.transaction = 0;
	_frames.expectedLength = DMX_MAX_FRAME;
	_next_send_slot = 0;
	_frames.receivedLength = 0;
	_rdm.transmitLength = 0;
	_rdm.discoveryLength = 0;
	_receive_callback = NULL;
	_rdm_receive_callback = NULL;
	clearSlots();
	memset(_frames.received, 0, sizeof(_frames.received));
	memset(_rdm.request, 0, sizeof(_rdm.request));
	memset(_rdm.response, 0, sizeof(_rdm.response));
	memset(_rdm.discovery, 0,
		sizeof(_rdm.discovery));
}

LX8266DMX::~LX8266DMX ( void ) {
    stop();
    _receive_callback = NULL;
    _rdm_receive_callback = NULL;
}

void LX8266DMX::startOutput ( void ) {
    if (uartOwner && uartOwner != this) return;
	setTransceiverTransmit();
	if ( _interrupt_status != ISR_OUTPUT_ENABLED ) {
		stop();
	}
	if ( _interrupt_status == ISR_DISABLED ) {	//prevent messing up sequence if already started...
		if (!claimHardware()) return;
		_interrupt_status = ISR_OUTPUT_ENABLED;
		_dmx_send_state = DMX_STATE_BREAK;
		_idle_count = 0;
		uart_init_tx(DMX_BREAK_BAUD, FORMAT_8E1, this);//starts interrupt because fifo is empty								
	}
}

void LX8266DMX::startInput ( void ) {
    if (uartOwner && uartOwner != this) return;
	setTransceiverReceive();
	if ( _interrupt_status != ISR_INPUT_ENABLED ) {
		stop();
	}
	if ( _interrupt_status == ISR_DISABLED ) {	//prevent messing up sequence if already started...
	   if (!claimHardware()) return;
	   _dmx_read_state = DMX_STATE_IDLE;
	   uart_init_rx(DMX_DATA_BAUD, FORMAT_8N2, this);
	   _interrupt_status = ISR_INPUT_ENABLED;
	}
}

void LX8266DMX::startRDM ( uint8_t pin, uint8_t direction ) {
	if (uartOwner && uartOwner != this) return;
	setDirectionPin(pin);
	startRDMConfigured(direction);
}

void LX8266DMX::startRDM (
		uint8_t pin, nocte::dmx::PortMode direction ) {
	startRDM(pin, static_cast<uint8_t>(direction));
}

void LX8266DMX::startRDM ( uint8_t driverEnablePin,
		uint8_t receiverEnableNotPin, uint8_t direction ) {
	if (uartOwner && uartOwner != this) return;
	setDirectionPins(driverEnablePin, receiverEnableNotPin);
	startRDMConfigured(direction);
}

void LX8266DMX::startRDM (
		uint8_t driverEnablePin,
		uint8_t receiverEnableNotPin,
		nocte::dmx::PortMode direction ) {
	startRDM(
		driverEnablePin,
		receiverEnableNotPin,
		static_cast<uint8_t>(direction));
}

void LX8266DMX::startRDMConfigured ( uint8_t direction ) {
    if (uartOwner && uartOwner != this) return;
	_rdm_task_mode = direction;
	
	setTransceiverTransmit();

	if ( _interrupt_status != ISR_RDM_ENABLED ) {
		stop();
	}
	if ( _interrupt_status == ISR_DISABLED ) {
		if (!claimHardware()) return;
		_interrupt_status = ISR_RDM_ENABLED;
		//TX
		_dmx_send_state = DMX_STATE_BREAK;
		_idle_count = 0;
		//RX
		_dmx_read_state = DMX_STATE_IDLE;
	    uart_init_rdm(DMX_DATA_BAUD, FORMAT_8N2, RDM_BREAK_BAUD, FORMAT_8E1, this);
	}
	
	if ( direction == 0 ) {
		setTransceiverReceive();
	}
}

void LX8266DMX::stop ( void ) { 
    if (uartOwner != this) return;
	if ( _interrupt_status == ISR_OUTPUT_ENABLED ) {
		uart_uninit_tx();
	} else if ( _interrupt_status == ISR_INPUT_ENABLED ) {
		uart_uninit_rx();
	} else if ( _interrupt_status == ISR_RDM_ENABLED ) {
		uart_uninit_rdm();
	}
	_interrupt_status = ISR_DISABLED;
    uartOwner = nullptr;
}

bool LX8266DMX::claimHardware() {
    esp8266::InterruptLock lock;
    if (uartOwner && uartOwner != this) return false;
    uartOwner = this;
    return true;
}

bool LX8266DMX::isActive() const { return uartOwner == this; }
void LX8266DMX::setUid(const nocte::dmx::core::Uid& value) {
    _uid = value;
    _customUid = true;
}
const uint8_t* LX8266DMX::sourceUid() const {
    return _customUid ? _uid.data() : THIS_DEVICE_ID.rawbytes();
}
nocte::dmx::core::Uid LX8266DMX::uid() const {
    return nocte::dmx::core::Uid(sourceUid());
}

void LX8266DMX::setDirectionPin( uint8_t pin ) {
	if (uartOwner && uartOwner != this) return;
	_direction_pin = pin;
	_receiver_enable_not_pin = DIRECTION_PIN_NOT_USED;

	if (_direction_pin != DIRECTION_PIN_NOT_USED) {
		pinMode(_direction_pin, OUTPUT);
	}
}

void LX8266DMX::setDirectionPins( uint8_t driverEnablePin,
		uint8_t receiverEnableNotPin ) {
	if (uartOwner && uartOwner != this) return;
	_direction_pin = driverEnablePin;
	_receiver_enable_not_pin = receiverEnableNotPin;

	if (_direction_pin != DIRECTION_PIN_NOT_USED) {
		pinMode(_direction_pin, OUTPUT);
	}

	if (_receiver_enable_not_pin != DIRECTION_PIN_NOT_USED) {
		pinMode(_receiver_enable_not_pin, OUTPUT);
	}
}

IRAM_ATTR void LX8266DMX::setTransceiverTransmit( void ) {
	// Disable the receiver before enabling the line driver.  With DE and
	// active-low /RE tied together only the second write is required.
	if (_receiver_enable_not_pin != DIRECTION_PIN_NOT_USED) {
		digitalWrite(_receiver_enable_not_pin, HIGH);
	}

	if (_direction_pin != DIRECTION_PIN_NOT_USED) {
		digitalWrite(_direction_pin, HIGH);
	}
}

IRAM_ATTR void LX8266DMX::setTransceiverReceive( void ) {
	// Release the bus before enabling the receiver.  With DE and active-low
	// /RE tied together the first write performs both operations.
	if (_direction_pin != DIRECTION_PIN_NOT_USED) {
		digitalWrite(_direction_pin, LOW);
	}

	if (_receiver_enable_not_pin != DIRECTION_PIN_NOT_USED) {
		digitalWrite(_receiver_enable_not_pin, LOW);
	}
}

uint16_t LX8266DMX::numberOfSlots (void) {
	return _frames.slots;
}

void LX8266DMX::setMaxSlots (int slots) {
	_frames.slots = nocte::dmx::core::clampOutputSlotCount(slots);
}

void LX8266DMX::setSlot (int slot, uint8_t value) {
	if (slot >= 0 && slot <= DMX_MAX_SLOTS) {
		_frames.dmx[slot] = value;
	}
}

uint8_t LX8266DMX::getSlot (int slot) {
	if (slot < 0 || slot > DMX_MAX_SLOTS) {
		return 0;
	}

	return _frames.dmx[slot];
}

bool LX8266DMX::setFrame(const uint8_t* data, uint16_t slots) {
	if (!nocte::dmx::core::isValidOutputSlotCount(slots) || data == NULL) {
		return false;
	}

	esp8266::InterruptLock lock;
	const bool replaced = nocte::dmx::core::replaceChannelData(
		_frames.dmx, data, slots);
	_frames.slots = slots;

	return replaced;
}

uint16_t LX8266DMX::copyFrame(uint8_t* destination, uint16_t capacity) {
	if (destination == NULL || capacity == 0) {
		return 0;
	}

	esp8266::InterruptLock lock;
	const uint16_t currentSlots = _frames.slots;
	const uint16_t slots = nocte::dmx::core::copyChannelData(
		_frames.dmx, currentSlots, destination, capacity);

	return slots;
}

void LX8266DMX::clearSlots (void) {
	memset(_frames.dmx, 0, DMX_MAX_SLOTS+1);
}

uint8_t* LX8266DMX::dmxData(void) {
	return &_frames.dmx[0];
}

uint8_t* LX8266DMX::rdmData( void ) {
	return _rdm.request;
}

uint16_t LX8266DMX::rdmPacketLength() {
    return _rdm.transmitLength;
}

uint8_t* LX8266DMX::receivedData( void ) {
	return _frames.received;
}

uint8_t* LX8266DMX::receivedRDMData( void ) {
	return _rdm.response;
}

/*!
 * @discussion TX FIFO EMPTY INTERRUPT
 *
 * this routine is called when UART fifo is empty
 *
 * what this does is to push the next byte into the fifo register
 *
 * when that byte is shifted out and the fifo is empty , the ISR is called again
 *
 * and the cycle repeats...
 *
 * until _frames.slots worth of bytes have been sent on succesive triggers of the ISR
 *
 * and then the fifo empty interrupt is allowed to trigger 25 times to insure the last byte is fully sent
 *
 * then the break/mark after break is sent at a different speed
 *
 * and then the fifo empty interrupt is allowed to trigger 60 times to insure the MAB is fully sent
 *
 * then the baud is restored and the start code is sent
 *
 * and then on the next fifo empty interrupt
 *
 * the next data byte is sent
 *
 * and the cycle repeats...
*/

IRAM_ATTR void LX8266DMX::txEmptyInterruptHandler(void) {

	switch ( _dmx_send_state ) {
		
		case DMX_STATE_BREAK:
			// set the slower baud rate and send the break
			uart_set_baudrate(UART0, DMX_BREAK_BAUD);
			uart_set_config(UART0, FORMAT_8E1);			
			_dmx_send_state = DMX_STATE_BREAK_SENT;
			_idle_count = 0;
			USF(0) = 0x0;
			break;		// <- DMX_STATE_BREAK
			
		case DMX_STATE_START:
			// set the baud to full speed and send the start code
			uart_set_baudrate(UART0, DMX_DATA_BAUD);
			uart_set_config(UART0, FORMAT_8N2);	
			_next_send_slot = 0;
			_dmx_send_state = DMX_STATE_DATA;			
			USF(0) = _frames.dmx[_next_send_slot++];	//send next slot (start code)
			break;		// <- DMX_STATE_START
		
		case DMX_STATE_DATA:
			// send the next data byte until the end is reached
			USF(0) = _frames.dmx[_next_send_slot++];	//send next slot
			if ( _next_send_slot > _frames.slots ) {
				_dmx_send_state = DMX_STATE_IDLE;
				_idle_count = 0;
			}
			break;		// <- DMX_STATE_DATA
			
		case DMX_STATE_IDLE:
			// wait a number of interrupts to be sure last data byte is sent before changing baud
			_idle_count++;
			if ( _idle_count > DATA_END_WAIT ) {
				_dmx_send_state = DMX_STATE_BREAK;
			}
			break;		// <- DMX_STATE_IDLE
			
		case DMX_STATE_BREAK_SENT:
			//wait to insure MAB before changing baud back to data speed (takes longer at slower speed)
			_idle_count++;
			if ( _idle_count > BREAK_SENT_WAIT ) {			
				_dmx_send_state = DMX_STATE_START;
			}
			break;		// <- DMX_STATE_BREAK_SENT
	}
}

IRAM_ATTR void LX8266DMX::rdmTxEmptyInterruptHandler(void) {

	if ( _rdm_task_mode == DMX_TASK_SEND_RDM ) {
		switch ( _dmx_send_state ) {
		
			case DMX_STATE_BREAK:
				// set the slower baud rate and send the break
				uart_set_baudrate(UART0, RDM_BREAK_BAUD);
				uart_set_config(UART0, FORMAT_8E1);			
				_dmx_send_state = DMX_STATE_BREAK_SENT;
				_idle_count = 0;
				USF(0) = 0x0;
				break;		// <- DMX_STATE_BREAK
			
			case DMX_STATE_START:
				// set the baud to full speed and send the start code
				uart_set_baudrate(UART0, DMX_DATA_BAUD);
				uart_set_config(UART0, FORMAT_8N2);	
				_next_send_slot = 0;
				_dmx_send_state = DMX_STATE_DATA;
				USF(0) = _rdm.request[_next_send_slot++];	//send next slot (start code)
				break;		// <- DMX_STATE_START
		
			case DMX_STATE_DATA:
				// send the next data byte until the end is reached
				USF(0) = _rdm.request[_next_send_slot++];	//send next slot
				if ( _next_send_slot >= _rdm.transmitLength ) {
					// TX-empty moves this final slot into the shift register.
					// Its EOP is one complete 8N2 slot later.
					_rdm.requestEndUs =
						micros() + RDM_SLOT_WIRE_US;
					_dmx_send_state = DMX_STATE_IDLE;
					_idle_count = 0;
				}
				break;		// <- DMX_STATE_DATA
			
			case DMX_STATE_IDLE:
				// wait a number of interrupts to be sure last data byte is sent before changing baud
				_idle_count++;
				if ( _idle_count > DATA_END_WAIT ) {
					_dmx_send_state = DMX_STATE_BREAK;
					
					//setTask to receive
					USIE(UART0) &= ~(1 << UIFE); 			// uart_disable_tx_interrupt();
					setTransceiverReceive();				// call from interrupt only because receiving starts
					_frames.receivedLength = 0;						// and these flags need to be set
					_frames.expectedLength = DMX_MAX_FRAME;			// but no bytes read from fifo until next task loop
					if ( _rdm.handled ) {
						_dmx_read_state = DMX_READ_STATE_RECEIVING;
					} else {
						_dmx_read_state = DMX_READ_STATE_IDLE;// if not after controller message, wait for a break
					}										  // signaling start of packet
					_rdm_task_mode = DMX_TASK_RECEIVE;
					
				}
				break;		// <- DMX_STATE_IDLE
			
			case DMX_STATE_BREAK_SENT:
				//wait to insure MAB before changing baud back to data speed (takes longer at slower speed)
				_idle_count++;
				if ( _idle_count > BREAK_SENT_WAIT ) {			
					_dmx_send_state = DMX_STATE_START;
				}
				break;		// <- DMX_STATE_BREAK_SENT
				
			}				// <- switch
		
	} else  {	// Send type state other than DMX_TASK_SEND_RDM
				// (handler not called when DMX_TASK_RECEIVE)
		switch ( _dmx_send_state ) {
		
			case DMX_STATE_BREAK:
				// set the slower baud rate and send the break
				uart_set_baudrate(UART0, DMX_BREAK_BAUD);
				uart_set_config(UART0, FORMAT_8E1);			
				_dmx_send_state = DMX_STATE_BREAK_SENT;
				_idle_count = 0;
				USF(0) = 0x0;
				break;		// <- DMX_STATE_BREAK
			
			case DMX_STATE_START:
				// set the baud to full speed and send the start code
				uart_set_baudrate(UART0, DMX_DATA_BAUD);
				uart_set_config(UART0, FORMAT_8N2);	
				_next_send_slot = 0;
				_dmx_send_state = DMX_STATE_DATA;
				USF(0) = _frames.dmx[_next_send_slot++];	//send next slot (start code)
				break;		// <- DMX_STATE_START
		
			case DMX_STATE_DATA:
				// send the next data byte until the end is reached
				USF(0) = _frames.dmx[_next_send_slot++];	//send next slot
				if ( _next_send_slot > _frames.slots ) {
					_dmx_send_state = DMX_STATE_IDLE;
					_idle_count = 0;
				}
				break;		// <- DMX_STATE_DATA
			
			case DMX_STATE_IDLE:
				// wait a number of interrupts to be sure last data byte is sent before changing baud
				_idle_count++;
				if ( _idle_count > DATA_END_WAIT ) {
					_dmx_send_state = DMX_STATE_BREAK;
					
					if ( _rdm_task_mode == DMX_TASK_SET_SEND ) {
						_rdm_task_mode = DMX_TASK_SEND;
					} else if ( _rdm_task_mode == DMX_TASK_SET_SEND_RDM ) {
						_rdm_task_mode = DMX_TASK_SEND_RDM;
					} else if (_rdm_task_mode == DMX_TASK_SET_RECEIVE) {
						// Finish the current DMX frame before a foreground FIFO
						// transaction takes over. Keep DE asserted until that code
						// has drained the complete RDM request, including stop bits.
						USIE(UART0) &= ~(1 << UIFE);
						_dmx_send_state = DMX_STATE_IDLE;
						_rdm_task_mode = DMX_TASK_RECEIVE;
					}
				}
				break;		// <- DMX_STATE_IDLE
			
			case DMX_STATE_BREAK_SENT:
				//wait to insure MAB before changing baud back to data speed (takes longer at slower speed)
				_idle_count++;
				if ( _idle_count > BREAK_SENT_WAIT ) {			
					_dmx_send_state = DMX_STATE_START;
				}
				break;		// <- DMX_STATE_BREAK_SEN
			
		}				// <- switch
	}					// <- state other than DMX_TASK_SEND_RDM
}

//************************************************************************************

void LX8266DMX::printReceivedData( void ) {
	for(int j=0; j<_frames.receivedLength; j++) {
		Serial.println(_frames.received[j]);
	}
}

IRAM_ATTR void LX8266DMX::packetComplete( void ) {
	if ( _frames.received[0] == 0 ) {				//zero start code is DMX
		if ( _rdm.handled == 0 ) {			// not handled by specific method
			if ( _frames.receivedLength > DMX_MIN_RECEIVE_SLOTS ) {
				_frames.slots = _frames.receivedLength - 1;				//_frames.receivedLength represents next slot so subtract one
				for(int j=0; j<_frames.receivedLength; j++) {	//copy dmx values from read buffer
					_frames.dmx[j] = _frames.received[j];
				}
	
				if ( _receive_callback != NULL ) {
					_receive_callback(_frames.slots);
				}
			}
		}
	} else {
		if ( _frames.received[0] == RDM_START_CODE ) {			//zero start code is RDM
			if ( _rdm.handled == 0 ) {					// not handled by specific method
				if ( validateRDMPacket(_frames.received) ) {	// evaluate checksum
					uint16_t plen = static_cast<uint16_t>(_frames.received[2]) + 2;
					for(int j=0; j<plen; j++) {
						_rdm.response[j] = _frames.received[j];
					}
					if ( _rdm_receive_callback != NULL ) {
						_rdm_receive_callback(plen);
					}
				}
			}
		} else {
#if defined LXESP8266UARTDMX_DEBUG
			Serial.println("________________ unknown data packet ________________");
			printReceivedData();
#endif
		}
	}
	resetFrame();
}

IRAM_ATTR void LX8266DMX::resetFrame( void ) {
	_dmx_read_state = DMX_READ_STATE_IDLE;						// insure wait for next break
	//_dmx_send_state????
}

IRAM_ATTR void LX8266DMX::receiveTimeout( void ) {
	if ( _dmx_read_state == DMX_READ_STATE_RECEIVING ) {
		if ( _frames.receivedLength > DMX_MIN_RECEIVE_SLOTS ) {
			packetComplete();
		}
	}
}

IRAM_ATTR void LX8266DMX::breakReceived( void ) {
	if (_rdm.handled) {
		_rdm.breakSeen = 1;
	}
	if ( _dmx_read_state == DMX_READ_STATE_RECEIVING ) {	// break has already been detected
		if ( _frames.receivedLength > 1 ) {						// break before end of maximum frame
			if ( _frames.received[0] == 0 ) {				// zero start code is DMX
				packetComplete();						// packet terminated with slots<512
			}
		}
	}
	_dmx_read_state = DMX_READ_STATE_RECEIVING;
	_frames.receivedLength = 0;
	_frames.expectedLength = DMX_MAX_FRAME;						// default to receive complete frame
}

IRAM_ATTR void LX8266DMX::byteReceived(uint8_t c) {
	if ( _dmx_read_state == DMX_READ_STATE_RECEIVING ) {
		if ( _frames.receivedLength >= DMX_MAX_FRAME ) {
			packetComplete();
			return;
		}

		if (_rdm.handled && _rdm.breakSeen) {
			const uint32_t receivedAtUs = micros();
			if (_frames.receivedLength == 0
					&& _rdm.requestEndUs != 0) {
				_rdm.firstSlotDelayUs =
					receivedAtUs - _rdm.requestEndUs;
			}
			if (_rdm.lastSlotUs != 0) {
				const uint32_t intervalUs =
					receivedAtUs - _rdm.lastSlotUs;
				if (intervalUs > _rdm.maximumSlotIntervalUs) {
					_rdm.maximumSlotIntervalUs = intervalUs;
				}
			}
			_rdm.lastSlotUs = receivedAtUs;
		}

		_frames.received[_frames.receivedLength] = c;
		// A DISC_UNIQUE_BRANCH response has 0-7 optional 0xFE preamble
		// slots, one 0xAA separator, and exactly 16 encoded payload slots.
		// Once the separator arrives its exact frame length is known. Closing
		// the frame there prevents a later transceiver-release edge from being
		// mistaken for an additional collision byte.
		if (_rdm.handled
				&& _frames.receivedLength < 8
				&& c == RDM_DISC_PREAMBLE_SEPARATOR
				&& (_frames.receivedLength == 0
					|| _frames.received[0] == RDM_DISC_PREAMBLE)) {
			_frames.expectedLength = _frames.receivedLength + 17;
		}
		if ( _frames.receivedLength == 2 ) {						//RDM length slot
			if ( _frames.received[0] == RDM_START_CODE ) {			//RDM start code
				// Message Length is authoritative for both callback-driven receive
				// and synchronous controller transactions. Discovery responses do
				// not carry the 0xCC start code and retain their special handling.
				if (c >= RDM_PKT_BASE_MSG_LEN) {
					_frames.expectedLength = c + 2;				//add two bytes for checksum
				} else {
					_dmx_read_state = DMX_READ_STATE_IDLE;
				}
			} else if ( _frames.received[0] == RDM_DISC_PREAMBLE ) {	//RDM Discovery Response
				_frames.expectedLength = DMX_MAX_FRAME;
			} else if ( _frames.received[0] != 0 ) {		// if Not Null Start Code
				_dmx_read_state = DMX_STATE_IDLE;			//unrecognized, ignore packet
			}
		}
	
		_frames.receivedLength++;
		if ( _frames.receivedLength >= _frames.expectedLength ) {		//reached expected end of packet
			// RDM EOP is the end of the second checksum stop bit. Close at the
			// authoritative Message Length and discard later unframed line
			// activity rather than folding post-EOP bus noise into this packet.
			packetComplete();
		}
	}
}


uint8_t LX8266DMX::isReceiving( void ) {
	return ( _dmx_read_state == DMX_READ_STATE_RECEIVING );
}

void LX8266DMX::setDataReceivedCallback(LXRecvCallback callback) {
	_receive_callback = callback;
}

/************************************ RDM Methods **************************************/

void LX8266DMX::setRDMReceivedCallback(LXRecvCallback callback) {
	_rdm_receive_callback = callback;
}

IRAM_ATTR uint8_t LX8266DMX::rdmTaskMode( void ) {		// applies to bidirectional RDM connection
	return _rdm_task_mode;
}

void LX8266DMX::setTaskSendDMX( void ) {		// only valid if connection started using startRDM()
	if (_interrupt_status != ISR_RDM_ENABLED) return;
	setTransceiverTransmit();
	 _rdm_task_mode = DMX_TASK_SEND;
}


IRAM_ATTR void LX8266DMX::restoreTaskSendDMX( void ) {		// only valid if connection started using startRDM()
	if (_interrupt_status != ISR_RDM_ENABLED) return;
	setTransceiverTransmit();
	_dmx_send_state = DMX_STATE_BREAK;
	 _rdm_task_mode = DMX_TASK_SET_SEND;
	 USIE(UART0) |= (1 << UIFE);					//restore the interrupt
	 do {
	 	delay(1);
	 } while ( _rdm_task_mode != DMX_TASK_SEND );	//set to send on interrupt pass after first DMX frame sent
}

void LX8266DMX::setTaskReceive( void ) {		// only valid if connection started using startRDM()
	if (_interrupt_status != ISR_RDM_ENABLED) return;
	_frames.receivedLength = 0;
	_frames.expectedLength = DMX_MAX_FRAME;
    _dmx_send_state = DMX_STATE_IDLE;
    _rdm_task_mode = DMX_TASK_RECEIVE;
    _rdm.handled = 0;
    USIE(UART0) &= ~(1 << UIFE);				// uart_disable_tx_interrupt();
    setTransceiverReceive();
}

namespace {
// The core's size-optimized InterruptLock destructor may be outlined into
// flash. Force both operations inline in our two IRAM timing windows.
class RdmTimingInterruptLock {
public:
	__attribute__((always_inline)) RdmTimingInterruptLock() : saved(xt_rsil(15)) {}
	__attribute__((always_inline)) ~RdmTimingInterruptLock() { xt_wsr_ps(saved); }
	RdmTimingInterruptLock(const RdmTimingInterruptLock&) = delete;
	RdmTimingInterruptLock& operator=(const RdmTimingInterruptLock&) = delete;
private:
	uint32_t saved;
};
}

void LX8266DMX::beginRdmTransmission(uint8_t firstByte) {
	RdmTimingInterruptLock lock;
	// Mask only the framing window, not the data packet. Queuing the start
	// code before unlocking prevents an IRQ from arbitrarily extending MAB.
	USC0(UART0) |= (1 << UCBRK);
	delayMicroseconds(RDM_CONTROLLER_BREAK_US);
	USC0(UART0) &= ~(1 << UCBRK);
	delayMicroseconds(RDM_CONTROLLER_MAB_US);
	USF(0) = firstByte;
}

bool LX8266DMX::finishRdmTransmission(uint8_t finalByte) {
	RdmTimingInterruptLock lock;
	// Reserve the final byte and observe its FIFO-to-shifter transition
	// without IRQ latency, not a possibly stale FIFO-empty observation.
	const uint32_t started = micros();
	if (((USS(UART0) >> USTXC) & 0xff) > 1) return false;
	USF(0) = finalByte;
	while ((USS(UART0) >> USTXC) & 0xff) {
		if (static_cast<uint32_t>(micros() - started) >= RDM_TRANSMIT_TAIL_TIMEOUT_US)
			return false;
	}
	const uint32_t finalSlotStartedUs = micros();
	delayMicroseconds(RDM_CONTROLLER_TX_DRAIN_US);
	_rdm.requestEndUs = finalSlotStartedUs + RDM_SLOT_WIRE_US;
	// Drop echo/stale data BEFORE arming capture and releasing DE. A later
	// foreground observation must never flush the earliest legal response.
	USC0(UART0) |= (1 << UCRXRST);
	USC0(UART0) &= ~(1 << UCRXRST);
	USIC(UART0) = (1 << UIFF) | (1 << UITO) | (1 << UIBD);
	_frames.receivedLength = 0;
	_frames.expectedLength = DMX_MAX_FRAME;
	_dmx_read_state = _rdm.handled ? DMX_READ_STATE_RECEIVING : DMX_READ_STATE_IDLE;
	_rdm_task_mode = DMX_TASK_RECEIVE;
	USIE(UART0) |= (1 << UIFF) | (1 << UITO) | (1 << UIBD);
	setTransceiverReceive();
	return true;
}

void LX8266DMX::abortRdmTransmission() {
	esp8266::InterruptLock lock;
	_rdmTransmitFailed = true;
	++_rdmTransmitTimeouts;
	USC0(UART0) &= ~(1 << UCBRK);
	uart_tx_flush();
	uart_rx_flush();
	USIC(UART0) = (1 << UIFF) | (1 << UITO) | (1 << UIBD) | (1 << UIFE);
	_frames.receivedLength = 0;
	_dmx_read_state = DMX_READ_STATE_IDLE;
	_rdm_task_mode = DMX_TASK_RECEIVE;
	USIE(UART0) |= (1 << UIFF) | (1 << UITO) | (1 << UIBD);
	setTransceiverReceive();
}

void LX8266DMX::sendRawRDMPacket( uint16_t len ) {		// only valid if connection started using startRDM()
	if (_interrupt_status != ISR_RDM_ENABLED) return;
	if (len < RDM_PKT_BASE_TOTAL_LEN || len > RDM_MAX_FRAME) {
		return;
	}
	_rdm.transmitLength = len;
	_rdmTransmitFailed = false;
	// calculate checksum:  len should include 2 bytes for checksum at the end
	uint16_t checksum = rdmChecksum(
		_rdm.request,
		static_cast<uint8_t>(_rdm.transmitLength - 2));
	_rdm.request[_rdm.transmitLength-2] = checksum >> 8;
	_rdm.request[_rdm.transmitLength-1] = checksum & 0xFF;

	if (_rdm_task_mode) {
		_rdm_task_mode = DMX_TASK_SET_RECEIVE;
		while (_rdm_task_mode != DMX_TASK_RECEIVE) delay(1);
	}
	{
		// A controller transaction starts while the bus is already in receive
		// mode.  Send it synchronously through the hardware FIFO: the historic
		// byte-per-interrupt path can develop large inter-slot gaps under Wi-Fi
		// load and was observed truncating consecutive broadcast 0xFF bytes.
		USIE(UART0) &= ~(1 << UIFE);
		USIE(UART0) &= ~((1 << UIFF) | (1 << UITO) | (1 << UIBD));
		setTransceiverTransmit();
		delayMicroseconds(100);

		// Drop any bytes left by the preceding DMX frame before asserting the
		// controller break.  This mirrors uart_flush() in the ESP8266 core, but
		// only resets TX so an already armed RDM receive path remains intact.
		uart_tx_flush();
		uart_set_baudrate(UART0, DMX_DATA_BAUD);
		uart_set_config(UART0, FORMAT_8N2);
		beginRdmTransmission(_rdm.request[0]);
		const uint32_t started = micros();

		for (uint16_t index = 1; index + 1 < _rdm.transmitLength; index++) {
			while (((USS(UART0) >> USTXC) & 0xff)
					>= RDM_UART_FIFO_FULL_LEVEL) {
				if (static_cast<uint32_t>(micros() - started) >= RDM_TRANSMIT_TIMEOUT_US) {
					abortRdmTransmission();
					return;
				}
			}
			USF(0) = _rdm.request[index];
		}

		// Bulk drain stays interruptible. Only the final <=3 wire slots and
		// direction handoff are protected (normally <=148 us, bounded).
		while (((USS(UART0) >> USTXC) & 0xff) > 1) {
			if (static_cast<uint32_t>(micros() - started) >= RDM_TRANSMIT_TIMEOUT_US) {
				abortRdmTransmission();
				return;
			}
		}
		if (!finishRdmTransmission(_rdm.request[_rdm.transmitLength - 1])) {
			abortRdmTransmission();
			return;
		}
	}
	
	while ( _rdm_task_mode ) {	//wait for packet to be sent and listening to start
		delay(1);				//_rdm_task_mode is set to 0 (receive) after RDM packet is completely sent
	}
}

void  LX8266DMX::setupRDMControllerPacket(uint8_t* pdata, uint8_t msglen, uint8_t port, uint16_t subdevice) {
	nocte::dmx::core::initializeRdmControllerHeader(
		pdata,
		msglen,
		sourceUid(),
		_rdm.transaction++,
		port,
		subdevice);
}

void  LX8266DMX::setupRDMDevicePacket(uint8_t* pdata, uint8_t msglen, uint8_t rtype, uint8_t msgs, uint16_t subdevice) {
	nocte::dmx::core::initializeRdmResponderHeader(
		pdata,
		msglen,
		sourceUid(),
		_rdm.transaction,
		rtype,
		msgs,
		subdevice);
}

void  LX8266DMX::setupRDMMessageDataBlock(uint8_t* pdata, uint8_t cmdclass, uint16_t pid, uint8_t pdl) {
	nocte::dmx::core::setRdmParameterHeader(
		pdata, cmdclass, pid, pdl);
}

uint8_t LX8266DMX::sendRDMDiscoveryPacket(const UID& lower, const UID& upper, UID* single) {
    if (!isActive() || _interrupt_status != ISR_RDM_ENABLED) return RDM_NO_DISCOVERY;
	uint8_t rv = RDM_NO_DISCOVERY;
	
	//Build RDM packet
	setupRDMControllerPacket(_rdm.request, RDM_DISC_UNIQUE_BRANCH_MSGL, RDM_PORT_ONE, RDM_ROOT_DEVICE);
	UID::copyFromUID(BROADCAST_ALL_DEVICES_ID, _rdm.request, 3);
	setupRDMMessageDataBlock(_rdm.request, RDM_DISCOVERY_COMMAND, RDM_DISC_UNIQUE_BRANCH, RDM_DISC_UNIQUE_BRANCH_PDL);
	UID::copyFromUID(lower, _rdm.request, 24);
	UID::copyFromUID(upper, _rdm.request, 30);
	
	_rdm.handled = 1;
	// Derive the wire length from the packet just built.  This keeps discovery
	// on the same proven send path as normal controller requests and prevents
	// stale historical packet-length constants from diverging from byte 2.
	sendRawRDMPacket(_rdm.request[RDM_IDX_PACKET_SIZE] + 2);
	if (_rdmTransmitFailed) {
		_rdm.discoveryLength = 0;
		_rdm.handled = 0;
		resetFrame();
		restoreTaskSendDMX();
		return RDM_PARTIAL_DISCOVERY;
	}
	delay(RDM_DISCOVERY_RESPONSE_WAIT_MS);

	const uint16_t receivedLength = _frames.receivedLength;
	_rdm.discoveryLength = min(
		(uint16_t)LX_RDM_DISCOVERY_DIAGNOSTIC_BYTES,
		receivedLength);
	memcpy(
		_rdm.discovery,
		_frames.received,
		_rdm.discoveryLength);

	// any bytes read indicate response to discovery packet
	// check if a single, complete, uncorrupted packet has been received
	// otherwise, refine discovery search
	
	if ( _frames.receivedLength ) {
		uint8_t uid[6];
		const nocte::dmx::core::RdmDiscoveryResult result =
			nocte::dmx::core::decodeRdmDiscoveryResponse(
				_frames.received, _frames.receivedLength, uid);
		rv = static_cast<uint8_t>(result);
		if (result == nocte::dmx::core::RdmDiscoveryResult::SingleDevice
				&& single != NULL) {
			*single = uid;
		}
		
		_rdm.handled = 0;
		resetFrame();
	} else {
		_rdm.handled = 0;
	}

	restoreTaskSendDMX();
	return rv;
}

uint8_t LX8266DMX::lastRDMDiscoveryResponseLength( void ) const {
	return _rdm.discoveryLength;
}

uint8_t LX8266DMX::copyLastRDMDiscoveryResponse(
		uint8_t* destination, uint8_t capacity) const {
	if (!destination || capacity == 0) {
		return 0;
	}

	const uint8_t copied = min(
		capacity,
		_rdm.discoveryLength);
	memcpy(destination, _rdm.discovery, copied);
	return copied;
}

uint8_t LX8266DMX::sendRDMDiscoveryMute(const UID& target, uint8_t cmd) {
    if (!isActive() || _interrupt_status != ISR_RDM_ENABLED) return 0;
	uint8_t rv = 0;

	//Build RDM packet
	// total packet length 0 parameter is 24 (+cksum =26 for sendRawRDMPacket) 
	setupRDMControllerPacket(_rdm.request, RDM_PKT_BASE_MSG_LEN, RDM_PORT_ONE, RDM_ROOT_DEVICE);
	UID::copyFromUID(target, _rdm.request, 3);
	setupRDMMessageDataBlock(_rdm.request, RDM_DISCOVERY_COMMAND, cmd, 0x00);

	if (target.isBroadcast()) {
		sendRDMControllerPacketNoResponse();
		return 0; // Broadcasts deliberately have no ACK.
	}
	if ( sendRDMControllerPacket() ) {
		if ( _rdm.response[RDM_IDX_PACKET_SIZE] + 2 >= (RDM_PKT_BASE_TOTAL_LEN+2) ) {
			if ( _rdm.response[RDM_IDX_RESPONSE_TYPE] == RDM_RESPONSE_TYPE_ACK ) {
				if ( _rdm.response[RDM_IDX_CMD_CLASS] == RDM_DISC_COMMAND_RESPONSE ) {
					if ( uid() == nocte::dmx::core::Uid(&_rdm.response[RDM_IDX_DESTINATION_UID]) ) {
						rv = 1;
					}
				}
			} else {
#if defined LXESP8266UARTDMX_DEBUG
				Serial.println("fail ACK");
#endif
			}
		}
	}
	return rv;
}

uint8_t LX8266DMX::sendRDMControllerPacket( void ) {
	if (!isActive() || _interrupt_status != ISR_RDM_ENABLED) return 0;
	uint8_t rv = 0;
	_rdm.handled = 1;
	_rdm.breakSeen = 0;
	_rdm.lastSlotUs = 0;
	_rdm.maximumSlotIntervalUs = 0;
	_rdm.firstSlotDelayUs = 0;
	_rdm.validationFailures = 0;
	_rdm.responseLength = 0;
	sendRawRDMPacket(_rdm.request[2]+2);
	if (_rdmTransmitFailed) {
		_rdm.validationFailures = nocte::dmx::core::kRdmReceiveError;
		_rdm.handled = 0;
		resetFrame();
		restoreTaskSendDMX();
		return 0;
	}

	// Use microsecond deadlines so the 3 ms E1.20 response-start window cannot
	// lose almost a millisecond at a millis() tick boundary.
	uint32_t deadline =
		micros() + RDM_CONTROLLER_RESPONSE_START_WAIT_US;
	while (_frames.receivedLength == 0
			&& !_rdm.breakSeen
			&& static_cast<int32_t>(micros() - deadline) < 0) {
		delay(0);
	}

	if (_frames.receivedLength > 0 || _rdm.breakSeen) {
		deadline = micros() + RDM_CONTROLLER_RESPONSE_FRAME_WAIT_US;
		while (_dmx_read_state == DMX_READ_STATE_RECEIVING
				&& static_cast<int32_t>(micros() - deadline) < 0) {
			delay(0);
		}
	}
	
	// Accept only a frame closed at its declared Message Length. A partial
	// timeout must never checksum stale bytes from a previous transaction.
	_rdm.responseLength = _frames.receivedLength;
	const nocte::dmx::core::RdmResponseObservation observation = {
		_rdm.breakSeen != 0,
		_dmx_read_state == DMX_READ_STATE_IDLE,
		_rdm.firstSlotDelayUs,
		_rdm.maximumSlotIntervalUs,
		RDM_RESPONSE_FIRST_SLOT_MIN_US,
		RDM_RESPONDER_MAX_SLOT_INTERVAL_US,
	};
	uint16_t failures = nocte::dmx::core::validateRdmResponse(
		_frames.received, _frames.receivedLength, observation);
	if (failures == 0 && !nocte::dmx::core::matchesRdmResponse(
			_rdm.request, _rdm.transmitLength,
			_frames.received, _frames.receivedLength)) {
		failures |= nocte::dmx::core::kRdmUnexpectedResponse;
	}
	_rdm.validationFailures = failures;

	if (failures == 0) {
		uint16_t plen = _frames.received[RDM_IDX_PACKET_SIZE] + 2;
		for(uint16_t index=0; index<plen; index++) {
			_rdm.response[index] = _frames.received[index];
		}
		rv = 1;
	}
	_rdm.handled = 0;
	resetFrame();
	
	restoreTaskSendDMX();
	return rv;
}

uint32_t LX8266DMX::lastRDMResponseFirstSlotDelayUs( void ) const {
	return _rdm.firstSlotDelayUs;
}

uint32_t LX8266DMX::lastRDMResponseMaxSlotIntervalUs( void ) const {
	return _rdm.maximumSlotIntervalUs;
}

bool LX8266DMX::lastRDMResponseBreakSeen( void ) const {
	return _rdm.breakSeen != 0;
}

uint16_t LX8266DMX::lastRDMResponseValidationFailures( void ) const {
	return _rdm.validationFailures;
}

uint16_t LX8266DMX::lastRDMResponseLength( void ) const {
	return _rdm.responseLength;
}

uint8_t LX8266DMX::lastRDMResponseDeclaredLength( void ) const {
	return _frames.received[RDM_IDX_PACKET_SIZE];
}

uint8_t LX8266DMX::lastRDMResponseTrailingByte( void ) const {
	return _rdm.responseLength > 0
		? _frames.received[_rdm.responseLength - 1]
		: 0;
}

void LX8266DMX::sendRDMControllerPacketNoResponse( void ) {
    if (!isActive() || _interrupt_status != ISR_RDM_ENABLED) return;
	_rdm.handled = 1;
	_rdm.breakSeen = 0;
	_rdm.lastSlotUs = 0;
	_rdm.maximumSlotIntervalUs = 0;
	_rdm.firstSlotDelayUs = 0;
	sendRawRDMPacket(_rdm.request[RDM_IDX_PACKET_SIZE] + 2);

	// Broadcast requests do not receive a response. Preserve the controller's
	// minimum spacing before allowing the regular DMX sender back onto the bus.
	delayMicroseconds(RDM_CONTROLLER_BROADCAST_SPACING_US);
	_rdm.handled = 0;
	resetFrame();
	restoreTaskSendDMX();
}

uint8_t LX8266DMX::sendRDMControllerPacket( uint8_t* bytes, uint16_t len ) {
	if (!bytes
			|| len < RDM_PKT_BASE_TOTAL_LEN
			|| len > RDM_MAX_FRAME
			|| len != static_cast<uint16_t>(bytes[RDM_IDX_PACKET_SIZE]) + 2) {
		return 0;
	}
	for (uint16_t j=0; j<len; j++) {
		_rdm.request[j] = bytes[j];
	}
	return sendRDMControllerPacket();
}

nocte::dmx::core::RdmCommandResult LX8266DMX::commandResult(bool received) {
    using namespace nocte::dmx::core;
    if (!received) {
        return {_rdm.responseLength == 0 ? RdmCommandStatus::Timeout
                                        : RdmCommandStatus::InvalidResponse,
                0, 0, _rdm.validationFailures};
    }
    return classifyRdmResponse(_rdm.response, _rdm.responseLength);
}

nocte::dmx::core::RdmCommandResult LX8266DMX::getRdmParameter(
        const nocte::dmx::core::Uid& target, uint16_t pid,
        uint8_t* destination, uint16_t capacity) {
    using namespace nocte::dmx::core;
    if ((capacity && !destination) || target.isBroadcast() || !isActive()
            || _interrupt_status != ISR_RDM_ENABLED) {
        return {RdmCommandStatus::InvalidArgument, 0, 0, 0};
    }
    buildRdmRequest(_rdm.request, sourceUid(), target.data(), _rdm.transaction++,
                    RDM_GET_COMMAND, pid, nullptr, 0);
    RdmCommandResult result = commandResult(sendRDMControllerPacket() != 0);
    if (result.status == RdmCommandStatus::Ack || result.status == RdmCommandStatus::Overflow) {
        result.copiedLength = copyRdmParameterData(
            _rdm.response, _rdm.responseLength, destination, capacity);
    }
    return result;
}

nocte::dmx::core::RdmCommandResult LX8266DMX::setRdmParameter(
        const nocte::dmx::core::Uid& target, uint16_t pid,
        const uint8_t* data, uint16_t length) {
    using namespace nocte::dmx::core;
    if (length > nocte::dmx::rdm::kMaximumParameterDataLength
            || (length && !data) || target.isBroadcast() || !isActive()
            || _interrupt_status != ISR_RDM_ENABLED) {
        return {RdmCommandStatus::InvalidArgument, 0, 0, 0};
    }
    buildRdmRequest(_rdm.request, sourceUid(), target.data(), _rdm.transaction++,
                    RDM_SET_COMMAND, pid, data, length);
    return commandResult(sendRDMControllerPacket() != 0);
}

uint8_t LX8266DMX::sendRDMGetCommand(
        const UID& target, uint16_t pid, uint8_t* info, uint8_t len) {
    return getRdmParameter(target, pid, info, len).ok();
}

uint8_t LX8266DMX::sendRDMSetCommand(
        const UID& target, uint16_t pid, uint8_t* info, uint8_t len) {
    return setRdmParameter(target, pid, info, len).ok();
}

void LX8266DMX::sendRDMGetResponse(UID target, uint16_t pid, uint8_t* info, uint8_t len) {
	uint8_t plen = RDM_PKT_BASE_MSG_LEN+len;
	
	//Build RDM packet
	setupRDMDevicePacket(_rdm.request, plen, RDM_RESPONSE_TYPE_ACK, 0, RDM_ROOT_DEVICE);
	UID::copyFromUID(target, _rdm.request, 3);
	setupRDMMessageDataBlock(_rdm.request, RDM_GET_COMMAND_RESPONSE, pid, len);
	for(int j=0; j<len; j++) {
		_rdm.request[24+j] = info[j];
	}
	
	sendRawRDMPacket(plen+2);	//add 2 bytes for checksum
}

void LX8266DMX::sendAckRDMResponse(uint8_t cmdclass, UID target, uint16_t pid) {
	uint8_t plen = RDM_PKT_BASE_MSG_LEN;
	
	//Build RDM packet
	setupRDMDevicePacket(_rdm.request, plen, RDM_RESPONSE_TYPE_ACK, 0, RDM_ROOT_DEVICE);
	UID::copyFromUID(target, _rdm.request, 3);
	setupRDMMessageDataBlock(_rdm.request, cmdclass, pid, 0x00);
	
	sendRawRDMPacket(plen+2);	//add 2 bytes for checksum
}

void LX8266DMX::sendMuteAckRDMResponse(uint8_t cmdclass, UID target, uint16_t pid) {
	uint8_t plen = RDM_PKT_BASE_MSG_LEN + 2;
	
	//Build RDM packet
	setupRDMDevicePacket(_rdm.request, plen, RDM_RESPONSE_TYPE_ACK, 0, RDM_ROOT_DEVICE);
	UID::copyFromUID(target, _rdm.request, 3);
	setupRDMMessageDataBlock(_rdm.request, cmdclass, pid, 0x02);
	
	sendRawRDMPacket(plen+2);	//add 2 bytes for checksum
}

void LX8266DMX::sendRDMDiscoverBranchResponse( void ) {
	if (_interrupt_status != ISR_RDM_ENABLED) return;
	// should be listening when this is called
	
	_rdm.request[0] = 0;
	_rdm.request[1] = 0xFE;
	_rdm.request[2] = 0xFE;
	_rdm.request[3] = 0xFE;
	_rdm.request[4] = 0xFE;
	_rdm.request[5] = 0xFE;
	_rdm.request[6] = 0xFE;
	_rdm.request[7] = 0xFE;
	_rdm.request[8] = 0xAA;
	
	_rdm.request[9] = sourceUid()[0] | 0xAA;
	_rdm.request[10] = sourceUid()[0] | 0x55;
	_rdm.request[11] = sourceUid()[1] | 0xAA;
	_rdm.request[12] = sourceUid()[1] | 0x55;
	
	_rdm.request[13] = sourceUid()[2] | 0xAA;
	_rdm.request[14] = sourceUid()[2] | 0x55;
	_rdm.request[15] = sourceUid()[3] | 0xAA;
	_rdm.request[16] = sourceUid()[3] | 0x55;
	_rdm.request[17] = sourceUid()[4] | 0xAA;
	_rdm.request[18] = sourceUid()[4] | 0x55;
	_rdm.request[19] = sourceUid()[5] | 0xAA;
	_rdm.request[20] = sourceUid()[5] | 0x55;
	
	uint16_t checksum = rdmChecksum(&_rdm.request[9], 12);
	uint8_t bite = checksum >> 8;
	_rdm.request[21] = bite | 0xAA;
	_rdm.request[22] = bite | 0x55;
	bite = checksum & 0xFF;
	_rdm.request[23] = bite | 0xAA;
	_rdm.request[24] = bite | 0x55;
	
	// send (no break)
	_rdm.transmitLength = 25;
	setTransceiverTransmit(); 			// could cut off receiving (?)
	delayMicroseconds(100);
	_next_send_slot = 1;//SKIP start code
	uart_set_baudrate(UART0, DMX_DATA_BAUD);
	uart_set_config(UART0, FORMAT_8N2);	
	_dmx_send_state = DMX_STATE_DATA;
	
	_rdm_task_mode = DMX_TASK_SEND_RDM;
	 //set the interrupt
	USIE(UART0) |= (1 << UIFE);

	
	while ( _rdm_task_mode ) {	//wait for packet to be sent and listening to start again
		delay(1);				//_rdm_task_mode is set to 0 (receive) after RDM packet is completely sent
	}
}

#endif // ESP8266 backend only
