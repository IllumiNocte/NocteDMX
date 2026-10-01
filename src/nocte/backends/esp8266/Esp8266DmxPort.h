/* LXESP8266UARTDMX.h
   Copyright 2015 by Claude Heintz Design

Copyright (c) 2015, Claude Heintz
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of LXESP8266DMX nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
-----------------------------------------------------------------------------------

   The LXESP8266UARTDMX library supports output and input of DMX using UART0
   of an ESP8266 microcontroller.  DMX output uses UART0 TX on GPIO1 and DMX
   input uses UART0 RX on GPIO3.  Hardware Serial cannot be used at the same
   time as DMX.
   
   This is the circuit for a simple unisolated DMX Shield
   that could be used with LXESP8266UARTDMX.  It uses a line driver IC
   to convert the output from the ESP8266 to DMX:

 ESP8266 Pin
 |                         SN 75176 A or MAX 481CPA
 V                            _______________
RX (3) |----------------------| 1      Vcc 8 |------(+5v)
       |                      |              |              DMX Output
       |                 +----| 2 !RE    B 7 |---------------- Pin 2
       |                 |    |              |
 (4/5) |----{+5v/GND}----+----| 3 DE     A 6 |---------------- Pin 3
       |                      |              |
TX (1) |----------------------| 4 DI   Gnd 5 |---+------------ Pin 1
       |                                         |
       |                                       (GND)

       Data Enable (DE) and Inverted Read Enable (!RE) can be wired to +5v for output or Gnd for input
       if direction switching is not needed.

--------------------------------------------------------------------------
------------------------  About UART GPIOs  ------------------------------

  UART0 is used for both DMX output and input.  Serial debug output must be
  disabled while DMX is active.


 UART0 TX: 1 or 2				SPECIAL or FUNCTION_4
 UART0 RX: 3						SPECIAL

 UART0 SWAP TX: 15
 UART0 SWAP RX: 13

 UART1 TX: 2 or 7 (NC)		SPECIAL or FUNCTION_4
 UART1 RX: 8 (NC)				FUNCTION_4

 UART1 SWAP TX: 11 (NC)
 UART1 SWAP RX: 6 (NC)

 NC = Not Connected to Module Pads --> No Access
      Pins 6-11 typically connected to flash on most modules
      see http://arduino.esp8266.com/versions/1.6.5-1160-gef26c5f/doc/reference.html
 */


#ifndef LX8266DMX_H
#define LX8266DMX_H


#include <Arduino.h>
#include <inttypes.h>
#include <rdm/UID.h>
#include "../../core/Constants.h"
#include "../../core/PortState.h"
#include "../../core/Uid.h"
#include "../../core/RdmPacket.h"

#define DMX_MIN_SLOTS nocte::dmx::kMinimumOutputSlots
#define DMX_MIN_RECEIVE_SLOTS nocte::dmx::kMinimumReceiveSlots
#define DMX_RX_TIMEOUT_THRESHOLD 127
#define RDM_MAX_FRAME nocte::dmx::rdm::kMaximumFrameSize
#define LX_RDM_DISCOVERY_DIAGNOSTIC_BYTES nocte::dmx::rdm::kDiscoveryDiagnosticBytes
#define DMX_MAX_SLOTS nocte::dmx::kMaximumSlots
#define DMX_MAX_FRAME nocte::dmx::kMaximumFrameSize

 //***** states indicate current position in DMX stream
    #define DMX_READ_STATE_IDLE 0
    #define DMX_READ_STATE_RECEIVING 1
    
#define DMX_TASK_RECEIVE		0   
#define DMX_TASK_SEND			1
#define DMX_TASK_SEND_RDM		2
#define DMX_TASK_SET_SEND		3
#define DMX_TASK_SET_SEND_RDM	4
#define DMX_TASK_SET_RECEIVE 5

#define RDM_NO_DISCOVERY		0
#define RDM_PARTIAL_DISCOVERY	1
#define RDM_DID_DISCOVER		2

#define DIRECTION_PIN_NOT_USED 255

typedef void (*LXRecvCallback)(int);

/*!   
@class LX8266DMX
@abstract
   LX8266DMX is a driver for sending or receiving DMX using an ESP8266
   UART0 TX on GPIO1 is used for output and UART0 RX on GPIO3 for input.
   
   LX8266DMX output mode continuously sends DMX once its interrupts have been enabled using startOutput().
   Use setSlot() to set the level value for a particular DMX dimmer/address/channel.
   
   LX8266DMX input mode receives DMX using the ESP8266's UART0 RX pin
   LX8266DMX continuously updates its DMX buffer once its interrupts have been enabled using startInput()
   and DMX data is received by UART0.
   Use getSlot() to read the level value for a particular DMX dimmer/address/channel.
   
   Constructed instances own independent state, but UART0 has one owner.
   ESP8266DMX remains the compatibility/default instance.
   
   LX8266DMX is NOT compatible with Serial.begin when DMX will be read.
*/

class LX8266DMX {

  public:
  
	LX8266DMX ( void );
   ~LX8266DMX( void );
   LX8266DMX(const LX8266DMX&) = delete;
   LX8266DMX& operator=(const LX8266DMX&) = delete;
  static constexpr bool supportsRdm = true;
  static constexpr bool supportsRdmController = true;
  static constexpr bool supportsRdmDiscovery = true;
  static constexpr bool supportsRdmResponder = true;

   // UART0 is exclusive. A second instance cannot stop or replace its owner.
   bool isActive() const;
   void setUid(const nocte::dmx::core::Uid& uid);
   nocte::dmx::core::Uid uid() const;
    
   /*!
    * @brief starts interrupt that continuously sends DMX output
    * @discussion Sets up baud rate, bits and parity, 
    *             sets globals accessed in ISR, 
    *             enables transmission and tx interrupt.
   */
   void startOutput( void );
   
   /*!
    * @brief starts interrupt that continuously reads DMX data
    * @discussion sets up baud rate, bits and parity, 
    *             sets globals accessed in ISR, 
    *             enables transmission and tx interrupt
   */
   void startInput( void );
   
   
   /*!
    * @brief starts interrupt that handles send/receive of RDM & DMX
    * @discussion Use this overload when DE and inverted /RE are tied to one
    *             direction pin.  HIGH transmits and LOW receives.
   */
   void startRDM ( uint8_t pin, uint8_t direction=DMX_TASK_SEND );

   void startRDM ( uint8_t pin, nocte::dmx::PortMode direction );

   /*!
    * @brief starts interrupt that handles send/receive of RDM & DMX
    * @discussion Use this overload when DE and active-low /RE are controlled
    *             by separate pins.  Direction is intentionally required so
    *             two-argument calls remain compatible with the original API.
    * @param driverEnablePin active-high DE pin
    * @param receiverEnableNotPin active-low /RE pin
    * @param direction initial DMX_TASK_SEND or DMX_TASK_RECEIVE mode
   */
   void startRDM ( uint8_t driverEnablePin,
                   uint8_t receiverEnableNotPin,
                   uint8_t direction );

   void startRDM ( uint8_t driverEnablePin,
                   uint8_t receiverEnableNotPin,
                   nocte::dmx::PortMode direction );
   
   /*!
    * @brief disables transmission and tx interrupt
   */
	void stop( void );
	
	/*!
	 * @brief optional utility sets the pin used to control driver chip's
	 *        DE (data enable) line, HIGH for output, LOW for input.     
    * @param pin to be automatically set for input/output direction
   */
   void setDirectionPin( uint8_t pin );

   /*!
    * @brief configures separate active-high DE and active-low /RE pins
    * @param driverEnablePin active-high DE pin
    * @param receiverEnableNotPin active-low /RE pin
    */
   void setDirectionPins( uint8_t driverEnablePin,
                          uint8_t receiverEnableNotPin );
   
   /*!
    * @brief the current number of slots in the latest output or received frame
   */
   uint16_t numberOfSlots (void);
	
	/*!
	 * @brief Sets the number of slots (aka addresses or channels) sent per DMX frame.
	 * @discussion defaults to 512 or DMX_MAX_SLOTS and should be no less DMX_MIN_SLOTS slots.  
	 *             The DMX standard specifies min break to break time no less than 1024 usecs.  
	 *             At 44 usecs per slot ~= 24
	 * @param slot the highest slot number (~24 to 512)
	*/
	void setMaxSlots (int slot);
	
	/*!
    * @brief reads the value of a slot/address/channel
    * @discussion NOTE: Data is not double buffered.  
    *                   So a complete single frame is not guaranteed.  
    *                   The ISR continuously reads the next frame into the buffer
    * @return level (0-255)
   */
   uint8_t getSlot (int slot);
   
   /*!
	 * @brief Sets the output value of a slot  Note:slot[0] is DMX start code!
	 * @param slot number of the slot aka address or channel (1-512)
	 * @param value level (0-255)
	*/
   void setSlot (int slot, uint8_t value);

   /*!
    * @brief Atomically replaces the output frame using channel data without
    *        a start code.  The start code is set to zero.
    * @param data pointer to channel values
    * @param slots number of channels, constrained to 24-512
    * @return true when the frame was accepted
    */
   bool setFrame(const uint8_t* data, uint16_t slots = DMX_MAX_SLOTS);

   /*!
    * @brief Atomically copies the latest received channel data without the
    *        start code.
    * @param destination output buffer
    * @param capacity maximum number of channel bytes to copy
    * @return number of copied channels
    */
   uint16_t copyFrame(
      uint8_t* destination,
      uint16_t capacity = DMX_MAX_SLOTS);
   
   /*!
    * @brief zero buffer including slot[0] which is start code
   */
   void clearSlots (void);
   
   /*!
    * @brief provides direct access to data array
    * @return pointer to dmx array
   */
	uint8_t* dmxData(void);
   
	uint8_t* rdmData( void );

	uint8_t* receivedData( void );

	uint8_t* receivedRDMData( void );
   
   /*!
    * @brief UART tx empty interrupt handler
   */
  	void		txEmptyInterruptHandler(void);
  	
  	/*!
    * @brief UART RDM tx empty interrupt handler
   */
  	void		rdmTxEmptyInterruptHandler(void);
  	
  	/*!
    * @brief Function called when DMX frame has been read
    * @discussion Sets a pointer to a function that is called
    *             on the break after a DMX frame has been received.  
    *             Whatever happens in this function should be quick!  
    *             Best used to set a flag that is polled outside of ISR for available data.
   */
   void setDataReceivedCallback(LXRecvCallback callback);
   
   
   /*!
    * @brief utility for debugging prints received data
   */
   void printReceivedData( void );
   
   /*!
    * @brief called when a packet is finished being received either through start of next packet or size reached
   */
   void packetComplete( void );

   /*!
    * @brief called when the UART receiver is idle long enough to mark end of a short DMX packet
   */
   void receiveTimeout( void );
   
   /*!
    * @brief sets read state to wait for break
   */
   void resetFrame( void );
   
   /*!
    * @brief called when a break is detected
   */
  	void breakReceived( void );
  	
  	/*!
    * @brief called from isr when a byte is read from register
   */
  	void byteReceived(uint8_t c);
  	
  	/*!
    * @brief returns true if _dmx_read_state == DMX_READ_STATE_RECEIVING
   */
  	uint8_t isReceiving( void );
  	
  	/************************************ RDM Methods ***********************************/
  	 
   /*!
    * @brief Function called when RDM frame has been read
    * @discussion Sets a pointer to a function that is called
    *             after an RDM frame has been received.  
    *             Whatever happens in this function should be quick!  
    *             Best used to set a flag that is polled outside of ISR for available data.
    */
   void setRDMReceivedCallback(LXRecvCallback callback);
   
   /*!
    * @brief indicate if dmx frame should be sent by bi-directional task loop
    * @discussion should only be called by task loop
    * @return 1 if dmx frame should be sent
    *  return 2 if RDM should be sent
    *  return 3 if RDM should be sent and set mode to 1 after first frame finished
    */
	uint8_t rdmTaskMode( void );
	
	
   /*!
    * @brief sets rdm task to send mode and the direction pin to HIGH
	*/
	void setTaskSendDMX( void );
	
   /*!
    * @brief sets rdm task to send mode after task mode loops.
    *        Sent after sending RDM message so DMX is resumed.
    *        Blocks until task loop sets mode to send.
	*/
	void restoreTaskSendDMX( void );
	
   /*!
    * @brief sets rdm task to receive mode
    *        Prepares variables to receive starting with next break.
    *        Sets the direction pin to LOW.
	*/
	void setTaskReceive( void );
	
   /*!
    * @brief length of the rdm packet awaiting being sent
	*/
	uint16_t rdmPacketLength( void );
	
   /*!
    * @brief sends packet using bytes from _rdmPacket ( rdmData() )
    * @discussion sets rdm task mode to DMX_TASK_SEND_RDM which causes
    *             _rdmPacket to be sent on next opportunity from task loop.
    *             after _rdmPacket is sent, task mode switches to listen for response.
    *
    *             set _rdm_read_handled flag prior to calling sendRawRDMPacket
    *             _rdm_read_handled = 1 if reading is handled by calling function
    *             _rdm_read_handled = 0 if desired to resume passive listening for next break
    */
	void sendRawRDMPacket( uint16_t len );
	
   /*!
    * @brief convenience method for setting fields in the top 20 bytes of an RDM message
    *        that will be sent.
    *        Destination UID needs to be set outside this method.
    *        Source UID is the per-instance UID or legacy THIS_DEVICE_ID fallback
	*/
	void setupRDMControllerPacket(uint8_t* pdata, uint8_t msglen, uint8_t port, uint16_t subdevice);
	
   /*!
    * @brief convenience method for setting fields in the top 20 bytes of an RDM message
    *        that will be sent.
    *        Destination UID needs to be set outside this method.
    *        Source UID is the per-instance UID or legacy THIS_DEVICE_ID fallback
	*/
	void  setupRDMDevicePacket(uint8_t* pdata, uint8_t msglen, uint8_t rtype, uint8_t msgs, uint16_t subdevice);
	
   /*!
    * @brief convenience method for setting fields in the top bytes 20-23 of an RDM message
    *        that will be sent.
	*/
	void setupRDMMessageDataBlock(uint8_t* pdata, uint8_t cmdclass, uint16_t pid, uint8_t pdl);
	
   /*!
    * @brief send discovery packet using upper and lower bounds
	* @discussion Assumes that regular DMX was sending when method is called and 
	*             so restores sending, waiting for a frame to be sent before returning.
    * @return 1 if discovered, 2 if valid packet (UID stored in uldata[12-17])
    */
    uint8_t sendRDMDiscoveryPacket(const UID& lower, const UID& upper, UID* single);

   /** @return Number of bytes captured from the most recent discovery reply. */
   uint8_t lastRDMDiscoveryResponseLength( void ) const;

   /**
    * @brief Copies the most recent discovery reply for diagnostics.
    * @return Number of bytes copied.
    */
   uint8_t copyLastRDMDiscoveryResponse(
      uint8_t* destination,
      uint8_t capacity) const;
    
   /*!
    * @brief send discovery mute/un-mute packet to target UID
	* @discussion Assumes that regular DMX was sending when method is called and 
	*             so restores sending, waiting for a frame to be sent before returning.
    * @return 1 if ack response is received.
    */
    uint8_t sendRDMDiscoveryMute(const UID& target, uint8_t cmd);
    
   /*!
    * @brief send previously built packet in _rdmPacket and validate response
    * @discussion Response to packet, if valid, is copied into _rdmData and 1 is returned
    *             Otherwise, 0 is returned.
    */
    uint8_t sendRDMControllerPacket( void );

   /** @return Timing of the first response slot relative to request EOP. */
   uint32_t lastRDMResponseFirstSlotDelayUs( void ) const;

   /** @return Largest interval observed between response slot completions. */
   uint32_t lastRDMResponseMaxSlotIntervalUs( void ) const;

   /** @return True when the most recent normal response began with BREAK. */
   bool lastRDMResponseBreakSeen( void ) const;

   /** @return Bit mask describing why the most recent response was rejected. */
   uint16_t lastRDMResponseValidationFailures( void ) const;

   /** @return Number of response slots captured on the most recent transaction. */
   uint16_t lastRDMResponseLength( void ) const;

   /** @return Message Length field captured from the most recent response. */
   uint8_t lastRDMResponseDeclaredLength( void ) const;

   /** @return Final byte captured from the most recent response. */
   uint8_t lastRDMResponseTrailingByte( void ) const;

   /**
    * @brief sends a previously built broadcast request without waiting for a response
    * @discussion Enforces the E1.20 controller spacing before restoring DMX output.
    */
    void sendRDMControllerPacketNoResponse( void );
    
   /*!
    * @brief copies len of bytes into _rdmPacket and sends it
    * @discussion Response to packet, if valid, is copied into _rdmData and 1 is returned
    *             Otherwise, 0 is returned.
    */
    uint8_t sendRDMControllerPacket( uint8_t* bytes, uint16_t len );
    
   /*!
    * @brief send RDM_GET_COMMAND packet
	* @discussion Assumes that regular DMX was sending when method is called and 
	*             so restores sending, waiting for a frame to be sent before returning.
    * @return 1 if ack is received.
    */
    uint8_t sendRDMGetCommand(const UID& target, uint16_t pid, uint8_t* info, uint8_t len);

    // Capacity is independent of actual PDL; the result reports both lengths.
    nocte::dmx::core::RdmCommandResult getRdmParameter(
        const nocte::dmx::core::Uid& target, uint16_t pid,
        uint8_t* destination, uint16_t capacity);
    nocte::dmx::core::RdmCommandResult setRdmParameter(
        const nocte::dmx::core::Uid& target, uint16_t pid,
        const uint8_t* data, uint16_t length);
    
   /*!
    * @brief send RDM_SET_COMMAND packet
	* @discussion Assumes that regular DMX was sending when method is called and 
	*             so restores sending, waiting for a frame to be sent before returning.
    * @return 1 if ack is received.
    */
    uint8_t sendRDMSetCommand(const UID& target, uint16_t pid, uint8_t* info, uint8_t len);
    
     
    /*!
    * @brief send RDM_GET_COMMAND_RESPONSE with RDM_RESPONSE_TYPE_ACK
	* @discussion sends data (info) of length (len)
    */
    void sendRDMGetResponse(UID target, uint16_t pid, uint8_t* info, uint8_t len);
    
    /*!
    * @brief send RDM_SET_COMMAND_RESPONSE/RDM_DISC_COMMAND_RESPONSE with RDM_RESPONSE_TYPE_ACK
	* @discussion PDL is zero
    */
    void sendAckRDMResponse(uint8_t cmdclass, UID target, uint16_t pid);
    
    
    
    
    void sendMuteAckRDMResponse(uint8_t cmdclass, UID target, uint16_t pid);
    
    /*!
    * @brief send response to RDM_DISC_UNIQUE_BRANCH packet
    */
    void sendRDMDiscoverBranchResponse( void );
    
    /*!
    * @brief static member containing UID of this device
    * @discussion call LX8266DMX::THIS_DEVICE_ID.setBytes() or
    *                  ESP8266DMX.THIS_DEVICE_ID.setBytes() to change default
    */
    static UID THIS_DEVICE_ID;
    
  private:
    nocte::dmx::core::FrameStorage _frames;
    nocte::dmx::core::RdmTransactionState _rdm;
    nocte::dmx::core::Uid _uid;
    bool _customUid = false;

    // Only these fields and functions belong to the ESP8266 PHY scheduler.
    IRAM_ATTR void setTransceiverTransmit();
    IRAM_ATTR void setTransceiverReceive();
    void startRDMConfigured(uint8_t direction);
    bool claimHardware();
    nocte::dmx::core::RdmCommandResult commandResult(bool received);
    const uint8_t* sourceUid() const;

    uint8_t _dmx_send_state;
    volatile uint8_t _dmx_read_state;
    uint8_t _interrupt_status;
    uint8_t _idle_count;
    volatile uint8_t _rdm_task_mode;
    uint8_t _direction_pin;
    uint8_t _receiver_enable_not_pin;
    uint16_t _next_send_slot;
    LXRecvCallback _receive_callback;
    LXRecvCallback _rdm_receive_callback;

};

extern LX8266DMX ESP8266DMX;

#endif // ifndef LX8266DMX_H
