/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

/**
 * MarlinSerial.cpp - Hardware serial library for Wiring
 * Copyright (c) 2006 Nicholas Zambetti.  All right reserved.
 *
 * Modified 23 November 2006 by David A. Mellis
 * Modified 28 September 2010 by Mark Sproul
 * Modified 14 February 2016 by Andreas Hardtung (added tx buffer)
 * Modified 01 October 2017 by Eduardo José Tagle (added XON/XOFF)
 * Modified 10 June 2018 by Eduardo José Tagle (See #10991)
 * Templatized 01 October 2018 by Eduardo José Tagle to allow multiple instances
 */

#if  defined PANDAPI

// Disable HardwareSerial.cpp to support chips without a UART (Attiny, etc.)

#include "../../inc/MarlinConfig.h"

#if 1// !defined(USBCON) && (defined(UBRRH) || defined(UBRR0H) || defined(UBRR1H) || defined(UBRR2H) || defined(UBRR3H))

  #include "MarlinSerial.h"
  #include "../../MarlinCore.h"
  #include "wiringSerial.h"

  #if ENABLED(DIRECT_STEPPING)
    #include "../../feature/direct_stepping.h"
  #endif


  template<typename Cfg> typename MarlinSerial<Cfg>::ring_buffer_r MarlinSerial<Cfg>::rx_buffer = { 0, 0, { 0 } };
  template<typename Cfg> typename MarlinSerial<Cfg>::ring_buffer_t MarlinSerial<Cfg>::tx_buffer = { 0 };
  template<typename Cfg> bool     MarlinSerial<Cfg>::_written = false;
  template<typename Cfg> uint8_t  MarlinSerial<Cfg>::xon_xoff_state = MarlinSerial<Cfg>::XON_XOFF_CHAR_SENT | MarlinSerial<Cfg>::XON_CHAR;
  template<typename Cfg> uint8_t  MarlinSerial<Cfg>::rx_dropped_bytes = 0;
  template<typename Cfg> uint8_t  MarlinSerial<Cfg>::rx_buffer_overruns = 0;
  template<typename Cfg> uint8_t  MarlinSerial<Cfg>::rx_framing_errors = 0;
  template<typename Cfg> typename MarlinSerial<Cfg>::ring_buffer_pos_t MarlinSerial<Cfg>::rx_max_enqueued = 0;

  // A SW memory barrier, to ensure GCC does not overoptimize loops
  #define sw_barrier() asm volatile("": : :"memory");

  #include "../../feature/e_parser.h"

  // "Atomically" read the RX head index value without disabling interrupts:
  // This MUST be called with RX interrupts enabled, and CAN'T be called
  // from the RX ISR itself!
  template<typename Cfg>
  FORCE_INLINE typename MarlinSerial<Cfg>::ring_buffer_pos_t MarlinSerial<Cfg>::atomic_read_rx_head() {
    if (Cfg::RX_SIZE > 256) {
      // Keep reading until 2 consecutive reads return the same value,
      // meaning there was no update in-between caused by an interrupt.
      // This works because serial RX interrupts happen at a slower rate
      // than successive reads of a variable, so 2 consecutive reads with
      // the same value means no interrupt updated it.
      ring_buffer_pos_t vold, vnew = rx_buffer.head;
      sw_barrier();
      do {
        vold = vnew;
        vnew = rx_buffer.head;
        sw_barrier();
      } while (vold != vnew);
      return vnew;
    }
    else {
      // With an 8bit index, reads are always atomic. No need for special handling
      return rx_buffer.head;
    }
  }

  template<typename Cfg>
  volatile bool MarlinSerial<Cfg>::rx_tail_value_not_stable = false;
  template<typename Cfg>
  volatile uint16_t MarlinSerial<Cfg>::rx_tail_value_backup = 0;
  template<typename Cfg>
  volatile int MarlinSerial<Cfg>::port_fd = 0;

  // Set RX tail index, taking into account the RX ISR could interrupt
  //  the write to this variable in the middle - So a backup strategy
  //  is used to ensure reads of the correct values.
  //    -Must NOT be called from the RX ISR -
  template<typename Cfg>
  FORCE_INLINE void MarlinSerial<Cfg>::atomic_set_rx_tail(typename MarlinSerial<Cfg>::ring_buffer_pos_t value) {
    if (Cfg::RX_SIZE > 256) {
      // Store the new value in the backup
      rx_tail_value_backup = value;
      sw_barrier();
      // Flag we are about to change the true value
      rx_tail_value_not_stable = true;
      sw_barrier();
      // Store the new value
      rx_buffer.tail = value;
      sw_barrier();
      // Signal the new value is completely stored into the value
      rx_tail_value_not_stable = false;
      sw_barrier();
    }
    else
      rx_buffer.tail = value;
  }

  // Get the RX tail index, taking into account the read could be
  //  interrupting in the middle of the update of that index value
  //    -Called from the RX ISR -
  template<typename Cfg>
  FORCE_INLINE typename MarlinSerial<Cfg>::ring_buffer_pos_t MarlinSerial<Cfg>::atomic_read_rx_tail() {
    if (Cfg::RX_SIZE > 256) {
      // If the true index is being modified, return the backup value
      if (rx_tail_value_not_stable) return rx_tail_value_backup;
    }
    // The true index is stable, return it
    return rx_buffer.tail;
  }

  // (called with RX interrupts disabled)
  template<typename Cfg>
  FORCE_INLINE void MarlinSerial<Cfg>::store_rxd_char(char rec_c) {

    static EmergencyParser::State emergency_state; // = EP_RESET

    // This must read the R_UCSRA register before reading the received byte to detect error causes
    if (Cfg::DROPPED_RX  && !++rx_dropped_bytes) --rx_dropped_bytes;
    if (Cfg::RX_OVERRUNS && !++rx_buffer_overruns) --rx_buffer_overruns;
    if (Cfg::RX_FRAMING_ERRORS && !++rx_framing_errors) --rx_framing_errors;

    // Read the character from the USART
    // uint8_t c = R_UDR;
    char c;

    #if ENABLED(DIRECT_STEPPING)
      if (page_manager.maybe_store_rxd_char(c)) return;
    #endif

    // Get the tail - Nothing can alter its value while this ISR is executing, but there's
    // a chance that this ISR interrupted the main process while it was updating the index.
    // The backup mechanism ensures the correct value is always returned.
    const ring_buffer_pos_t t = atomic_read_rx_tail();

    // Get the head pointer - This ISR is the only one that modifies its value, so it's safe to read here
    ring_buffer_pos_t h = rx_buffer.head;

    // Get the next element
    ring_buffer_pos_t i = (ring_buffer_pos_t)(h + 1) & (ring_buffer_pos_t)(Cfg::RX_SIZE - 1);

    if (Cfg::EMERGENCYPARSER) emergency_parser.update(emergency_state, c);

    // If the character is to be stored at the index just before the tail
    // (such that the head would advance to the current tail), the RX FIFO is
    // full, so don't write the character or advance the head.
    c = rec_c;//USART_ReceiveData(USART1);//M_UDRx;
    if (i != t) {
      rx_buffer.buffer[h] = c;
      h = i;
    }
    else if (Cfg::DROPPED_RX && !++rx_dropped_bytes)
      --rx_dropped_bytes;

    if (Cfg::MAX_RX_QUEUED) {
      // Calculate count of bytes stored into the RX buffer
      const ring_buffer_pos_t rx_count = (ring_buffer_pos_t)(h - t) & (ring_buffer_pos_t)(Cfg::RX_SIZE - 1);

      // Keep track of the maximum count of enqueued bytes
      NOLESS(rx_max_enqueued, rx_count);
    }

    if (Cfg::XONOFF) {
      // If the last char that was sent was an XON
      if ((xon_xoff_state & XON_XOFF_CHAR_MASK) == XON_CHAR) {

        // Bytes stored into the RX buffer
        const ring_buffer_pos_t rx_count = (ring_buffer_pos_t)(h - t) & (ring_buffer_pos_t)(Cfg::RX_SIZE - 1);

        // If over 12.5% of RX buffer capacity, send XOFF before running out of
        // RX buffer space .. 325 bytes @ 250kbits/s needed to let the host react
        // and stop sending bytes. This translates to 13mS propagation time.
        if (rx_count >= (Cfg::RX_SIZE) / 8) {

          // At this point, definitely no TX interrupt was executing, since the TX ISR can't be preempted.
          // Don't enable the TX interrupt here as a means to trigger the XOFF char, because if it happens
          // to be in the middle of trying to disable the RX interrupt in the main program, eventually the
          // enabling of the TX interrupt could be undone. The ONLY reliable thing this can do to ensure
          // the sending of the XOFF char is to send it HERE AND NOW.

          // About to send the XOFF char
          xon_xoff_state = XOFF_CHAR | XON_XOFF_CHAR_SENT;

          // Wait until the TX register becomes empty and send it - Here there could be a problem
          // - While waiting for the TX register to empty, the RX register could receive a new
          //   character. This must also handle that situation!
          while (1)
		   {

            //if (B_RXC)
			 {
              // A char arrived while waiting for the TX buffer to be empty - Receive and process it!

              i = (ring_buffer_pos_t)(h + 1) & (ring_buffer_pos_t)(Cfg::RX_SIZE - 1);

              // Read the character from the USART
           //   c = R_UDR;

              if (Cfg::EMERGENCYPARSER) emergency_parser.update(emergency_state, c);

              // If the character is to be stored at the index just before the tail
              // (such that the head would advance to the current tail), the FIFO is
              // full, so don't write the character or advance the head.
              if (i != t) {
                rx_buffer.buffer[h] = c;
                h = i;
              }
              else if (Cfg::DROPPED_RX && !++rx_dropped_bytes)
                --rx_dropped_bytes;
            }
            sw_barrier();
          }

        //  R_UDR = XOFF_CHAR;

          // Clear the TXC bit -- "can be cleared by writing a one to its bit
          // location". This makes sure flush() won't return until the bytes
          // actually got written
       //   B_TXC = 1;

          // At this point there could be a race condition between the write() function
          // and this sending of the XOFF char. This interrupt could happen between the
          // wait to be empty TX buffer loop and the actual write of the character. Since
          // the TX buffer is full because it's sending the XOFF char, the only way to be
          // sure the write() function will succeed is to wait for the XOFF char to be
          // completely sent. Since an extra character could be received during the wait
          // it must also be handled!
        /*  while (!B_UDRE)
		   {

            if (B_RXC) {
              // A char arrived while waiting for the TX buffer to be empty - Receive and process it!

              i = (ring_buffer_pos_t)(h + 1) & (ring_buffer_pos_t)(Cfg::RX_SIZE - 1);

              // Read the character from the USART
              c = R_UDR;

              if (Cfg::EMERGENCYPARSER)
                emergency_parser.update(emergency_state, c);

              // If the character is to be stored at the index just before the tail
              // (such that the head would advance to the current tail), the FIFO is
              // full, so don't write the character or advance the head.
              if (i != t) {
                rx_buffer.buffer[h] = c;
                h = i;
              }
              else if (Cfg::DROPPED_RX && !++rx_dropped_bytes)
                --rx_dropped_bytes;
            }
            sw_barrier();
          }
*/
          // At this point everything is ready. The write() function won't
          // have any issues writing to the UART TX register if it needs to!
        }
      }
    }
	
    // Store the new head value - The main loop will retry until the value is stable
    rx_buffer.head = h;
  }

  // (called with TX irqs disabled)
  template<typename Cfg>
  FORCE_INLINE void MarlinSerial<Cfg>::_tx_udr_empty_irq() {

  }

  // Public Methods
  template<typename Cfg>
  void MarlinSerial<Cfg>::begin(const long baud) {
     if(Cfg::PORT==0)
	  	port_fd=serialOpen("/dev/ttyAMA0",baud);
	 else if(Cfg::PORT==1)
	  	port_fd=serialOpen("/dev/tnt1",baud);	
	 // Serial_PC(baud,Cfg::PORT,&store_rxd_char);


  }
 
  template<typename Cfg>
  void MarlinSerial<Cfg>::end() {

  }

  template<typename Cfg>
  int MarlinSerial<Cfg>::peek() {
    const ring_buffer_pos_t h = atomic_read_rx_head(), t = rx_buffer.tail;
    return h == t ? -1 : rx_buffer.buffer[t];
  }

  template<typename Cfg>
  int MarlinSerial<Cfg>::read() {
    unsigned char rc[32];
  //  if(Cfg::PORT==0)
    {
		rc[0]=serialGetchar(port_fd);
		// printf("0Y%x ",rc[0]);
		return rc[0];
    }
    const ring_buffer_pos_t h = atomic_read_rx_head();

    // Read the tail. Main thread owns it, so it is safe to directly read it
    ring_buffer_pos_t t = rx_buffer.tail;

    // If nothing to read, return now
    if (h == t) return -1;

    // Get the next char
    const int v = rx_buffer.buffer[t];
    t = (ring_buffer_pos_t)(t + 1) & (Cfg::RX_SIZE - 1);
	 printf("0Y%x ",v);

    // Advance tail - Making sure the RX ISR will always get an stable value, even
    // if it interrupts the writing of the value of that variable in the middle.
    atomic_set_rx_tail(t);

    if (Cfg::XONOFF) {
      // If the XOFF char was sent, or about to be sent...
      if ((xon_xoff_state & XON_XOFF_CHAR_MASK) == XOFF_CHAR) {
        // Get count of bytes in the RX buffer
        const ring_buffer_pos_t rx_count = (ring_buffer_pos_t)(h - t) & (ring_buffer_pos_t)(Cfg::RX_SIZE - 1);
        if (rx_count < (Cfg::RX_SIZE) / 128) {
          if (Cfg::TX_SIZE > 0) {
            // Signal we want an XON character to be sent.
            xon_xoff_state = XON_CHAR;
            // Enable TX ISR. Non atomic, but it will eventually enable them
        //    B_UDRIE = 1;
          }
          else {
            // If not using TX interrupts, we must send the XON char now
            xon_xoff_state = XON_CHAR | XON_XOFF_CHAR_SENT;
          //  while (!B_UDRE) sw_barrier();
          //  R_UDR = XON_CHAR;
          }
        }
      }
    }

    return v;
  }

  template<typename Cfg>
  typename MarlinSerial<Cfg>::ring_buffer_pos_t MarlinSerial<Cfg>::available() {
    ring_buffer_pos_t n=serialDataAvail(port_fd);
   // printf("serialDataAvail==%d ",n);
	return n;
	const ring_buffer_pos_t h = atomic_read_rx_head(), t = rx_buffer.tail;
    return (ring_buffer_pos_t)(Cfg::RX_SIZE + h - t) & (Cfg::RX_SIZE - 1);
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::flush() {

    // Set the tail to the head:
    //  - Read the RX head index in a safe way. (See atomic_read_rx_head.)
    //  - Set the tail, making sure the RX ISR will always get a stable value, even
    //    if it interrupts the writing of the value of that variable in the middle.
    atomic_set_rx_tail(atomic_read_rx_head());
/*
    if (Cfg::XONOFF) {
      // If the XOFF char was sent, or about to be sent...
      if ((xon_xoff_state & XON_XOFF_CHAR_MASK) == XOFF_CHAR) {
        if (Cfg::TX_SIZE > 0) {
          // Signal we want an XON character to be sent.
          xon_xoff_state = XON_CHAR;
          // Enable TX ISR. Non atomic, but it will eventually enable it.
          B_UDRIE = 1;
        }
        else {
          // If not using TX interrupts, we must send the XON char now
          xon_xoff_state = XON_CHAR | XON_XOFF_CHAR_SENT;
          while (!B_UDRE) sw_barrier();
          R_UDR = XON_CHAR;
        }
      }
    }*/
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::write(const uint8_t c) {
//  printf("port:%d,c:0x%x ",Cfg::PORT,c);
 //  if(Cfg::PORT==0)
	 serialPutchar(port_fd,c);
 //  else
  // 	Serial_send_char(Cfg::PORT,c);
  }
#if HAS_DGUS_LCD  
  template<typename Cfg>
  int MarlinSerial<Cfg>:: get_tx_buffer_free() {
	  const int t = tx_buffer.tail,  // next byte to send.
							  h = tx_buffer.head;  // next pos for queue.
	  int ret = t - h - 1;
	  if (ret < 0) ret += Cfg::TX_SIZE + 1;
	  return ret;
	}
#endif
  template<typename Cfg>
  void MarlinSerial<Cfg>::flushTX() {
/*
    if (Cfg::TX_SIZE == 0) {
      // No bytes written, no need to flush. This special case is needed since there's
      // no way to force the TXC (transmit complete) bit to 1 during initialization.
      if (!_written) return;

      // Wait until everything was transmitted
      while (!B_TXC) sw_barrier();

      // At this point nothing is queued anymore (DRIE is disabled) and
      // the hardware finished transmission (TXC is set).

    }
    else {

      // No bytes written, no need to flush. This special case is needed since there's
      // no way to force the TXC (transmit complete) bit to 1 during initialization.
      if (!_written) return;

      // If global interrupts are disabled (as the result of being called from an ISR)...
      if (!ISRS_ENABLED()) {

        // Wait until everything was transmitted - We must do polling, as interrupts are disabled
        while (tx_buffer.head != tx_buffer.tail || !B_TXC) {

          // If there is more space, send an extra character
          if (B_UDRE) _tx_udr_empty_irq();

          sw_barrier();
        }

      }
      else {
        // Wait until everything was transmitted
        while (tx_buffer.head != tx_buffer.tail || !B_TXC) sw_barrier();
      }

      // At this point nothing is queued anymore (DRIE is disabled) and
      // the hardware finished transmission (TXC is set).
    }*/
  }

  /**
   * Imports from print.h
   */

  template<typename Cfg>
  void MarlinSerial<Cfg>::print(char c, int base) {
    print((long)c, base);
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::print(unsigned char b, int base) {
    print((unsigned long)b, base);
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::print(int n, int base) {
    print((long)n, base);
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::print(unsigned int n, int base) {
    print((unsigned long)n, base);
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::print(long n, int base) {
    if (base == 0) write(n);
    else if (base == 10) {
      if (n < 0) { print('-'); n = -n; }
      printNumber(n, 10);
    }
    else
      printNumber(n, base);
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::print(unsigned long n, int base) {
    if (base == 0) write(n);
    else printNumber(n, base);
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::print(double n, int digits) {
    printFloat(n, digits);
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::println() {
    print('\r');
    print('\n');
  }
  /*

  template<typename Cfg>
 void MarlinSerial<Cfg>::println(const String& s) {
    print(s);
    println();
  }*/

  template<typename Cfg>
  void MarlinSerial<Cfg>::println(const char c[]) {
    print(c);
    println();
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::println(char c, int base) {
    print(c, base);
    println();
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::println(unsigned char b, int base) {
    print(b, base);
    println();
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::println(int n, int base) {
    print(n, base);
    println();
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::println(unsigned int n, int base) {
    print(n, base);
    println();
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::println(long n, int base) {
    print(n, base);
    println();
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::println(unsigned long n, int base) {
    print(n, base);
    println();
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::println(double n, int digits) {
    print(n, digits);
    println();
  }

  // Private Methods

  template<typename Cfg>
  void MarlinSerial<Cfg>::printNumber(unsigned long n, uint8_t base) {
    if (n) {
      unsigned char buf[8 * sizeof(long)]; // Enough space for base 2
      int8_t i = 0;
      while (n) {
        buf[i++] = n % base;
        n /= base;
      }
      while (i--)
        print((char)(buf[i] + (buf[i] < 10 ? '0' : 'A' - 10)));
    }
    else
      print('0');
  }

  template<typename Cfg>
  void MarlinSerial<Cfg>::printFloat(double number, uint8_t digits) {
    // Handle negative numbers
    if (number < 0.0) {
      print('-');
      number = -number;
    }

    // Round correctly so that print(1.999, 2) prints as "2.00"
    double rounding = 0.5;
    LOOP_L_N(i, digits) rounding *= 0.1;
    number += rounding;

    // Extract the integer part of the number and print it
    unsigned long int_part = (unsigned long)number;
    double remainder = number - (double)int_part;
    print(int_part);

    // Print the decimal point, but only if there are digits beyond
    if (digits) {
      print('.');
      // Extract digits from the remainder one at a time
      while (digits--) {
        remainder *= 10.0;
        int toPrint = int(remainder);
        print(toPrint);
        remainder -= toPrint;
      }
    }
  }
/*
  // Hookup ISR handlers
  ISR(SERIAL_REGNAME(USART,SERIAL_PORT,_RX_vect)) {
    MarlinSerial<MarlinSerialCfg<SERIAL_PORT>>::store_rxd_char();
  }

  ISR(SERIAL_REGNAME(USART,SERIAL_PORT,_UDRE_vect)) {
    MarlinSerial<MarlinSerialCfg<SERIAL_PORT>>::_tx_udr_empty_irq();
  }
*/
  // Preinstantiate
  template class MarlinSerial<MarlinSerialCfg<SERIAL_PORT>>;

  // Instantiate
  MarlinSerial<MarlinSerialCfg<SERIAL_PORT>> customizedSerial1;

  #ifdef SERIAL_PORT_2

    // Hookup ISR handlers
 /*   ISR(SERIAL_REGNAME(USART,SERIAL_PORT_2,_RX_vect)) {
      MarlinSerial<MarlinSerialCfg<SERIAL_PORT_2>>::store_rxd_char();
    }

    ISR(SERIAL_REGNAME(USART,SERIAL_PORT_2,_UDRE_vect)) {
      MarlinSerial<MarlinSerialCfg<SERIAL_PORT_2>>::_tx_udr_empty_irq();
    }
*/
    // Preinstantiate
    template class MarlinSerial<MarlinSerialCfg<SERIAL_PORT_2>>;

    // Instantiate
    MarlinSerial<MarlinSerialCfg<SERIAL_PORT_2>> customizedSerial2;

  #endif

#endif // !USBCON && (UBRRH || UBRR0H || UBRR1H || UBRR2H || UBRR3H)

#ifdef INTERNAL_SERIAL_PORT

  ISR(SERIAL_REGNAME(USART,INTERNAL_SERIAL_PORT,_RX_vect)) {
    MarlinSerial<MarlinInternalSerialCfg<INTERNAL_SERIAL_PORT>>::store_rxd_char();
  }

  ISR(SERIAL_REGNAME(USART,INTERNAL_SERIAL_PORT,_UDRE_vect)) {
    MarlinSerial<MarlinInternalSerialCfg<INTERNAL_SERIAL_PORT>>::_tx_udr_empty_irq();
  }

  // Preinstantiate
  template class MarlinSerial<MarlinInternalSerialCfg<INTERNAL_SERIAL_PORT>>;

  // Instantiate
  MarlinSerial<MarlinInternalSerialCfg<INTERNAL_SERIAL_PORT>> internalSerial;

#endif

#ifdef DGUS_SERIAL_PORT

 // template<typename Cfg>
  /*int MarlinSerial<Cfg>::  get_tx_buffer_free() {
    const int t = tx_buffer.tail,  // next byte to send.
                            h = tx_buffer.head;  // next pos for queue.
    int ret = t - h - 1;
    if (ret < 0) ret += Cfg::TX_SIZE + 1;
    return ret;
  }*/

//void MarlinSerial<Cfg>::begin(const long baud)

/* PANDA
  ISR(SERIAL_REGNAME(USART,DGUS_SERIAL_PORT,_RX_vect)) {
    MarlinSerial<MarlinInternalSerialCfg<DGUS_SERIAL_PORT>>::store_rxd_char();
  }

  ISR(SERIAL_REGNAME(USART,DGUS_SERIAL_PORT,_UDRE_vect)) {
    MarlinSerial<MarlinInternalSerialCfg<DGUS_SERIAL_PORT>>::_tx_udr_empty_irq();
  }

  // Preinstantiate
  template class MarlinSerial<MarlinInternalSerialCfg<DGUS_SERIAL_PORT>>;

  // Instantiate
  MarlinSerial<MarlinInternalSerialCfg<DGUS_SERIAL_PORT>> internalDgusSerial;*/

#endif

// For AT90USB targets use the UART for BT interfacing
#if defined(USBCON) && ENABLED(BLUETOOTH)
  HardwareSerial bluetoothSerial;
#endif

#endif // __AVR__
