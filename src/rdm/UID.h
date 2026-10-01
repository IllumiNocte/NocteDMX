/**************************************************************************/
/*!
    @file     UID.cpp
    @author   Claude Heintz
    @license  BSD (see LXESP8266DMX.h)
    @copyright 2017 by Claude Heintz

    RDM support for DMX Driver for ESP32
    
    Implements class encapsulating 48bit RDM unique device identifier
    MM:DDDD
    Where MM is ESTA manufacturer code.

    @section  HISTORY

    v1.0 - First release
*/
/**************************************************************************/

#ifndef RDMUID_h
#define RDMUID_h

#include <stdint.h>
#include <WString.h>
#include <Printable.h>
#include <nocte/core/Uid.h>


// utility functions
void print64Bit(uint64_t n);
uint64_t uid_bytes2long(uint8_t* b);
uint64_t uid_bytes2long(const uint8_t* b);
void uid_long2Bytes(uint64_t u, uint8_t* bytes);


/*!   
@class UID
@abstract
   A class to make it easier to handle RDM UIDs.
   (Based on Arduino IPAddress class using 6 bytes instead of 4)
*/
 
class UID: public Printable, public nocte::dmx::core::Uid
{
public:
	/*!
	 * @brief default constructor
	 * @discussion
	 */
    UID( void );
    
	/*!
	 * @brief construct UID from 64bit integer
	 * @discussion
	 */
    UID( uint64_t u );
    /*!
	 * @brief construct UID from 6 individual bytes
	 */
    UID(uint8_t m1, uint8_t m2, uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4);
    /*!
	 * @brief construct UID from pointer to 6 byte array
	 */
    UID(const uint8_t *address);
    UID(const UID&) = default;

	// Overloaded equality operator
    bool operator==(const UID& addr) const;
    bool operator==(const uint8_t* addr) const;

    // Overloaded index operator to allow getting and setting individual bytes
    uint8_t operator[](int index) const {
        return data()[index];
    }
    uint8_t& operator[](int index) {
        return data()[index];
    }

    // Overloaded copy operators to allow initialisation of UID objects
    UID& operator=(const uint8_t *address);
    UID& operator=(const UID& address);
    
    /*!
	 * @brief copy bytes of a UID into an array starting at index
	 */
    static void copyFromUID(const UID& id, uint8_t *address, uint16_t index=0) {
        memcpy(&address[index], id.data(), nocte::dmx::rdm::kUidSize);
    }
    
    /*!
	 * @brief copy bytes into a UID from an array starting at index
	 */
    static void copyToUID(UID& id, const uint8_t *address, uint16_t index=0) {
        memcpy(id.data(), &address[index], nocte::dmx::rdm::kUidSize);
    }
    
    /*!
	 * @brief set the bytes of this UID to the midpoint of 2 UIDs
	 */
    uint8_t becomeMidpoint(const UID& a, const UID& b);
    
    /*!
	 * @brief pointer to the 6 byte array
	 */
    uint8_t* rawbytes( void );
    const uint8_t* rawbytes( void ) const;
    
    /*!
	 * @brief set the bytes of this UID with a 64bit integer
	 */
    void setBytes(uint64_t u);
    
    /*!
	 * @brief set the bytes of this UID with another UID (copy)
	 */
    void setBytes(const UID& u);
    
    /*!
	 * @brief set the bytes of this UID with individual mfg code and device ID bytes
	 */
    void setBytes(uint8_t m1, uint8_t m2, uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4);
    
    //void setBytes(uint8_t* u);
    
    /*!
	 * @brief UID as 64 bit integer
	 */
    uint64_t getValue ( void ) const;

	/*!
	 * @brief print with formatting
	 */
    virtual size_t printTo(Print& p) const;
    
    /*!
	 * @brief convert to formatted string
	 */
    String toString() const;
};

	/*!
	 * @brief ALL_DEVICES wildcard UID
	 */
const UID BROADCAST_ALL_DEVICES_ID(0xFFFFFFFFFFFF);

#endif	//RDMUID
