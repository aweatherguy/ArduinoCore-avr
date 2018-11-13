/*
  HardwareSerial.cpp - Hardware serial library for Wiring
  Copyright (c) 2006 Nicholas Zambetti.  All right reserved.
  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.
  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.
  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
  
  Modified 23 November 2006 by David A. Mellis
  Modified 28 September 2010 by Mark Sproul
  Modified 14 August 2012 by Alarus
  Modified 3 December 2013 by Matthijs Kooijman
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <util/atomic.h>
#include "Arduino.h"

#include "HardwareSerial.h"
#include "HardwareSerial_private.h"

// this next line disables the entire HardwareSerial.cpp, 
// this is so I can support Attiny series and any other chip without a uart
#if defined(HAVE_HWSERIAL0) || defined(HAVE_HWSERIAL1) || defined(HAVE_HWSERIAL2) || defined(HAVE_HWSERIAL3)

// SerialEvent functions are weak, so when the user doesn't define them,
// the linker just sets their address to 0 (which is checked below).
// The Serialx_available is just a wrapper around Serialx.available(),
// but we can refer to it weakly so we don't pull in the entire
// HardwareSerial instance if the user doesn't also refer to it.
#if defined(HAVE_HWSERIAL0)
  void serialEvent() __attribute__((weak));
  bool Serial0_available() __attribute__((weak));
#endif

#if defined(HAVE_HWSERIAL1)
  void serialEvent1() __attribute__((weak));
  bool Serial1_available() __attribute__((weak));
#endif

#if defined(HAVE_HWSERIAL2)
  void serialEvent2() __attribute__((weak));
  bool Serial2_available() __attribute__((weak));
#endif

#if defined(HAVE_HWSERIAL3)
  void serialEvent3() __attribute__((weak));
  bool Serial3_available() __attribute__((weak));
#endif

void serialEventRun(void)
{
#if defined(HAVE_HWSERIAL0)
  if (Serial0_available && serialEvent && Serial0_available()) serialEvent();
#endif
#if defined(HAVE_HWSERIAL1)
  if (Serial1_available && serialEvent1 && Serial1_available()) serialEvent1();
#endif
#if defined(HAVE_HWSERIAL2)
  if (Serial2_available && serialEvent2 && Serial2_available()) serialEvent2();
#endif
#if defined(HAVE_HWSERIAL3)
  if (Serial3_available && serialEvent3 && Serial3_available()) serialEvent3();
#endif
}

// Macro to guard critical sections for large transmit buffer sizes
// Reads from the transmit buffer tail pointer and writes to either head or tail pointer
// should be atomic in this case
#if (SERIAL_TX_BUFFER_SIZE>256)
#define TX_BUFFER_ATOMIC ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
#else
#define TX_BUFFER_ATOMIC
#endif

// Macro to guard critical sections for large receive buffer sizes
// Reads from the receive buffer head pointer and writes to either head or tail pointer
// should be atomic in this case
#if (SERIAL_RX_BUFFER_SIZE>256)
#define RX_BUFFER_ATOMIC ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
#else
#define RX_BUFFER_ATOMIC
#endif

#define IS_PWR_TWO(n) ( ( ( (n) - 1 ) & (n) ) == 0 )

// Actual interrupt handlers //////////////////////////////////////////////////////////////

void HardwareSerial::_tx_udr_empty_irq(void)
{
  tx_buffer_index_t head = _tx_buffer_head;
  tx_buffer_index_t tail = _tx_buffer_tail;

  // If interrupts are enabled, there must be more data in the output
  // buffer. Send the next byte
  unsigned char c = _tx_buffer[tail];
#if IS_PWR_TWO( SERIAL_TX_BUFFER_SIZE )
  tail = (tail + 1) & (SERIAL_TX_BUFFER_SIZE - 1);
#else
  tail += 1;
  if (tail >= SERIAL_TX_BUFFER_SIZE) tail = 0;
#endif

  *_udr = c;

  // clear the TXC bit -- "can be cleared by writing a one to its bit
  // location". This makes sure flush() won't return until the bytes
  // actually got written. Other r/w bits are preserved, and zeroes
  // written to the rest.

  *_ucsra = ((*_ucsra) & ((1 << U2X0) | (1 << MPCM0))) | (1 << TXC0);

  if (head == tail) {
    // Buffer empty, so disable interrupts
    cbi(*_ucsrb, UDRIE0);
  }
  _tx_buffer_tail = tail;
}

// Public Methods //////////////////////////////////////////////////////////////

void HardwareSerial::begin(unsigned long baud, byte config)
{
  // Try u2x mode first
  uint16_t baud_setting = (F_CPU / 4 / baud - 1) / 2;
  *_ucsra = 1 << U2X0;

  // hardcoded exception for 57600 for compatibility with the bootloader
  // shipped with the Duemilanove and previous boards and the firmware
  // on the 8U2 on the Uno and Mega 2560. Also, The baud_setting cannot
  // be > 4095, so switch back to non-u2x mode if the baud rate is too
  // low.
  if (((F_CPU == 16000000UL) && (baud == 57600)) || (baud_setting >4095))
  {
    *_ucsra = 0;
    baud_setting = (F_CPU / 8 / baud - 1) / 2;
  }

  // assign the baud_setting, a.k.a. ubrr (USART Baud Rate Register)
  *_ubrrh = baud_setting >> 8;
  *_ubrrl = baud_setting;

  _written = false;

  //set the data bits, parity, and stop bits
#if defined(__AVR_ATmega8__)
  config |= 0x80; // select UCSRC register (shared with UBRRH)
#endif
  *_ucsrc = config;
  
  sbi(*_ucsrb, RXEN0);
  sbi(*_ucsrb, TXEN0);
  sbi(*_ucsrb, RXCIE0);
  cbi(*_ucsrb, UDRIE0);
}

void HardwareSerial::end()
{
  // wait for transmission of outgoing data
  flush();

  cbi(*_ucsrb, RXEN0);
  cbi(*_ucsrb, TXEN0);
  cbi(*_ucsrb, RXCIE0);
  cbi(*_ucsrb, UDRIE0);
  
  // clear any received data
  RX_BUFFER_ATOMIC {
  _rx_buffer_head = _rx_buffer_tail;
  }
}

int HardwareSerial::available(void)
{
  rx_buffer_index_t head;
  rx_buffer_index_t tail;
  RX_BUFFER_ATOMIC {
    head = _rx_buffer_head;
  }
  tail = _rx_buffer_tail;
#if IS_PWR_TWO( SERIAL_RX_BUFFER_SIZE )
  return (head - tail) & (SERIAL_RX_BUFFER_SIZE - 1);
#else
  if (tail > head)
    return (SERIAL_RX_BUFFER_SIZE + head - tail);
  else
    return head - tail;
#endif
}

int HardwareSerial::peek(void)
{
  rx_buffer_index_t head;
  rx_buffer_index_t tail;
  RX_BUFFER_ATOMIC {
    head = _rx_buffer_head;
  }
  tail = _rx_buffer_tail;
  if (head == tail) {
    return -1;
  } else {
    return _rx_buffer[tail];
  }
}

int HardwareSerial::read(void)
{
  rx_buffer_index_t head;
  rx_buffer_index_t tail;
  RX_BUFFER_ATOMIC {
    head = _rx_buffer_head;
  }
  tail = _rx_buffer_tail;
  // if the head isn't ahead of the tail, we don't have any characters
  if (head == tail) {
    return -1;
  } else {
    rx_buffer_index_t newTail;
#if IS_PWR_TWO( SERIAL_RX_BUFFER_SIZE )
    newTail = (tail + 1) & (SERIAL_RX_BUFFER_SIZE - 1);
#else
    newTail = tail + 1;
    if (newTail >= SERIAL_RX_BUFFER_SIZE) newTail = 0;
#endif
	RX_BUFFER_ATOMIC {
      _rx_buffer_tail = newTail;
    }
    return _rx_buffer[tail];
  }
}

int HardwareSerial::availableForWrite(void)
{
  tx_buffer_index_t head;
  tx_buffer_index_t tail;

  TX_BUFFER_ATOMIC {
    head = _tx_buffer_head;
    tail = _tx_buffer_tail;
  }
#if IS_PWR_TWO( SERIAL_TX_BUFFER_SIZE )
  return (tx_buffer_index_t)((tail - head - 1) & (SERIAL_TX_BUFFER_SIZE - 1));
#else
  if (head >= tail) return (SERIAL_TX_BUFFER_SIZE - 1) - head + tail;
  return (tx_buffer_index_t)(tail - head - 1);
#endif
}

void HardwareSerial::flush()
{
  // If we have never written a byte, no need to flush. This special
  // case is needed since there is no way to force the TXC (transmit
  // complete) bit to 1 during initialization
  if (!_written)
    return;

  while (bit_is_set(*_ucsrb, UDRIE0) || bit_is_clear(*_ucsra, TXC0)) {
    if (bit_is_clear(SREG, SREG_I) && bit_is_set(*_ucsrb, UDRIE0))
	// Interrupts are globally disabled, but the DR empty
	// interrupt should be enabled, so poll the DR empty flag to
	// prevent deadlock
	if (bit_is_set(*_ucsra, UDRE0))
	  _tx_udr_empty_irq();
  }
  // If we get here, nothing is queued anymore (DRIE is disabled) and
  // the hardware finished tranmission (TXC is set).
}

size_t HardwareSerial::write(uint8_t c)
{
  _written = true;
  tx_buffer_index_t head = _tx_buffer_head;
  tx_buffer_index_t tail;
  TX_BUFFER_ATOMIC {
    tail = _tx_buffer_tail;
  }
  // If the buffer and the data register is empty, just write the byte
  // to the data register and be done. This shortcut helps
  // significantly improve the effective datarate at high (>
  // 500kbit/s) bitrates, where interrupt overhead becomes a slowdown.
  if (head == tail && bit_is_set(*_ucsra, UDRE0)) {
    // If TXC is cleared before writing UDR and the previous byte
    // completes before writing to UDR, TXC will be set but a byte
    // is still being transmitted causing flush() to return too soon.
    // So writing UDR must happen first.
    // Writing UDR and clearing TC must be done atomically, otherwise
    // interrupts might delay the TXC clear so the byte written to UDR
    // is transmitted (setting TXC) before clearing TXC. Then TXC will
    // be cleared when no bytes are left, causing flush() to hang
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      *_udr = c;
      *_ucsra = ((*_ucsra) & ((1 << U2X0) | (1 << MPCM0))) | (1 << TXC0);
    }
    return 1;
  }
  _tx_buffer[head] = c;
  tx_buffer_index_t newHead;
#if IS_PWR_TWO
  newHead = (head + 1) & (SERIAL_TX_BUFFER_SIZE - 1);
#else
  newHead = head + 1;
  if (newHead >= SERIAL_TX_BUFFER_SIZE) newHead = 0;
#endif
  // If the output buffer is full, there's nothing for it other than to 
  // wait for the interrupt handler to empty it a bit
  // OTOH, if interrupts are enabled, the local copy of tail must be refreshed each pass through the
  // loop because it is updated by the UDRE ISR.
  if (newHead == tail)
  {
      if (bit_is_clear(SREG, SREG_I)) {
          // Interrupts are disabled, so we'll have to poll the data
          // register empty flag ourselves. If it is set, pretend an
          // interrupt has happened and call the handler to free up
          // space for us. On the plus side, we don't need to worry 
          // about making atomic accesses to the tail pointer.
          while (newHead == _rx_buffer_tail) {
              if(bit_is_set(*_ucsra, UDRE0)) _tx_udr_empty_irq();
          }
      }
      else
      {
          while (newHead == tail)
          {
              TX_BUFFER_ATOMIC {
                  tail = _rx_buffer_tail;
              }
          }
      }
  }
  // make atomic to prevent execution of ISR between setting the
  // head pointer and setting the interrupt flag resulting in buffer
  // retransmission
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    _tx_buffer_head = newHead;
    sbi(*_ucsrb, UDRIE0);
  }
  
  return 1;
}

#endif // whole file
