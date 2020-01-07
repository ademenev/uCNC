/*
	Name: mcu_avr.c
	Description: Implements mcu interface on AVR.
		Besides all the functions declared in the mcu.h it also implements the code responsible
		for handling:
			interpolator.h
				void interpolator_step_isr();
				void interpolator_step_reset_isr();
			serial.h
				void serial_rx_isr(char c);
				char serial_tx_isr();
			trigger_control.h
				void dio_limits_isr(uint8_t limits);
				void dio_controls_isr(uint8_t controls);
				
	Copyright: Copyright (c) João Martins 
	Author: João Martins
	Date: 01/11/2019

	uCNC is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version. Please see <http://www.gnu.org/licenses/>

	uCNC is distributed WITHOUT ANY WARRANTY;
	Also without the implied warranty of	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
	See the	GNU General Public License for more details.
*/

#ifdef __MCU_AVR__

#include "config.h"
#include "mcudefs.h"
#include "mcumap.h"
#include "mcu.h"
#include "utils.h"
#include "serial.h"
#include "interpolator.h"
#include "dio_control.h"

#include <math.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <avr/delay.h>
#include <avr/eeprom.h>


#define PORTMASK (OUTPUT_INVERT_MASK|INPUT_PULLUP_MASK)
#ifndef F_CPU
#define F_CPU 16000000UL
#endif

#ifndef BAUD
#define BAUD 115200
#endif

#ifndef COM_BUFFER_SIZE
#define COM_BUFFER_SIZE 50
#endif

#define PULSE_RESET_DELAY MIN_PULSE_WIDTH_US * F_CPU / 1000000

typedef union{
    uint32_t r; // occupies 4 bytes
#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    struct
    {
        uint16_t rl;
        uint16_t rh;
    };
    struct
    {
        uint8_t r0;
        uint8_t r1;
        uint8_t r2;
        uint8_t r3;
    };
#else
    struct
    {
        uint16_t rh;
        uint16_t rl;
    };
    struct
    {
        uint8_t r3;
        uint8_t r2;
        uint8_t r1;
        uint8_t r0;
    }
    ;
#endif
} IO_REGISTER;

//USART communication
int mcu_putchar(char c, FILE* stream);
FILE g_mcu_streamout = FDEV_SETUP_STREAM(mcu_putchar, NULL, _FDEV_SETUP_WRITE);

#ifdef __PERFSTATS__
volatile uint16_t mcu_perf_step;
volatile uint16_t mcu_perf_step_reset;

uint16_t mcu_get_step_clocks()
{
	uint16_t res = mcu_perf_step;
	return res;
}
uint16_t mcu_get_step_reset_clocks()
{
	uint16_t res = mcu_perf_step_reset;
	return res;
}
#endif

ISR(TIMER1_COMPA_vect, ISR_BLOCK)
{
	static bool busy = false;
	#ifdef __PERFSTATS__
	uint16_t clocks = TCNT1;
	#endif
	if(busy)
	{
		return;
	}
	busy = true;
	interpolator_step_reset_isr();
	
	#ifdef __PERFSTATS__
    uint16_t clocks2 = TCNT1;
    clocks2 -= clocks;
	mcu_perf_step_reset = MAX(mcu_perf_step_reset, clocks2);
	#endif
	busy = false;
}

ISR(TIMER1_COMPB_vect, ISR_BLOCK)
{
	static bool busy = false;
	#ifdef __PERFSTATS__
	uint16_t clocks = TCNT1;
	#endif
	if(busy)
	{
		return;
	}
	busy = true;
    interpolator_step_isr();
    #ifdef __PERFSTATS__
    uint16_t clocks2 = TCNT1;
    clocks2 -= clocks;
	mcu_perf_step = MAX(mcu_perf_step, clocks2);
	#endif
	busy = false;
}

ISR(PCINT0_vect, ISR_NOBLOCK) // input pin on change service routine
{
	#if(LIMITS_ISR_ID==0)
	static uint8_t prev_limits = 0;
	uint8_t limits = mcu_get_limits();
	if(prev_limits != limits)
	{
		dio_limits_isr(limits);
		prev_limits = limits;
	}
	#endif
	
	#if(CONTROLS_ISR_ID==0)
	static uint8_t prev_controls = 0;
	uint8_t controls = mcu_get_controls();
	if(prev_controls != controls)
	{
		dio_controls_isr(controls);
		prev_controls = controls;
	}
	#endif		
}

ISR(PCINT1_vect, ISR_NOBLOCK) // input pin on change service routine
{
    #if(LIMITS_ISR_ID==1)
	static uint8_t prev_limits = 0;
	uint8_t limits = mcu_get_limits();
	if(prev_limits != limits)
	{
		dio_limits_isr(limits);
		prev_limits = limits;
	}
	#endif

	#if(CONTROLS_ISR_ID==1)
	static uint8_t prev_controls = 0;
	uint8_t controls = mcu_get_controls();
	if(prev_controls != controls)
	{
		dio_controls_isr(controls);
		prev_controls = controls;
	}
	#endif
}

ISR(PCINT2_vect, ISR_NOBLOCK) // input pin on change service routine
{
    #if(LIMITS_ISR_ID==2)
	static uint8_t prev_limits = 0;
	uint8_t limits = mcu_get_limits();
	if(prev_limits != limits)
	{
		dio_limits_isr(limits);
		prev_limits = limits;
	}
	#endif

	#if(CONTROLS_ISR_ID==2)
	static uint8_t prev_controls = 0;
	uint8_t controls = mcu_get_controls();
	if(prev_controls != controls)
	{
		dio_controls_isr(controls);
		prev_controls = controls;
	}
	#endif
}

ISR(PCINT3_vect, ISR_NOBLOCK) // input pin on change service routine
{
    #if(LIMITS_ISR_ID==3)
	static uint8_t prev_limits = 0;
	uint8_t limits = mcu_get_limits();
	if(prev_limits != limits)
	{
		dio_limits_isr(limits);
		prev_limits = limits;
	}
	#endif

	#if(CONTROLS_ISR_ID==3)
	static uint8_t prev_controls = 0;
	uint8_t controls = mcu_get_controls();
	if(prev_controls != controls)
	{
		dio_controls_isr(controls);
		prev_controls = controls;
	}
	#endif
}

ISR(USART_RX_vect, ISR_BLOCK)
{
	uint8_t c = UDR0;
	serial_rx_isr(c);
}

ISR(USART_UDRE_vect, ISR_BLOCK)
{
	if(serial_tx_is_empty())
	{
		UCSR0B &= ~(1<<UDRIE0);
		return;
	}
	
	UDR0 = serial_tx_isr();
}

void mcu_init()
{
    IO_REGISTER reg = {};
    
    #ifdef __PERFSTATS__
	mcu_perf_step = 0;
	mcu_perf_step_reset = 0;
	#endif
	
	//disable WDT
	wdt_reset();
    MCUSR &= ~(1<<WDRF);
	WDTCSR |= (1<<WDCE) | (1<<WDE);
	WDTCSR = 0x00;

    //sets all outputs and inputs
    //inputs
    #ifdef CONTROLS_DIRREG
        CONTROLS_DIRREG = 0;
    #endif
    
	#ifdef LIMITS_DIRREG
        LIMITS_DIRREG = 0;
    #endif
    
    #ifdef PROBE_DIRREG
        PROBE_DIRREG = 0;
    #endif
    
	#ifdef COM_DIRREG
        COM_DIRREG = 0;
    #endif
    
    #ifdef DINS_LOW_DIRREG
        DINS_LOW_DIRREG = 0;
    #endif
    
    #ifdef DINS_HIGH_DIRREG
        DINS_HIGH_DIRREG = 0;
    #endif

	#ifdef ANALOG_DIRREG
		ANALOG_DIRREG = 0;
	#endif
    
    //pull-ups
    #ifdef CONTROLS_PULLUPREG
        CONTROLS_PULLUPREG |= CONTROLS_PULLUP_MASK;
    #endif
    
	#ifdef LIMITS_PULLUPREG
        LIMITS_PULLUPREG |= LIMITS_PULLUP_MASK;
    #endif
    
    #ifdef PROBE_PULLUPREG
        PROBE_PULLUPREG |= PROBE_PULLUP_MASK;
    #endif

    #ifdef DINS_LOW_PULLUPREG
        DINS_LOW_PULLUPREG |= DINS_LOW_PULLUP_MASK;
    #endif
    
    #ifdef DINS_HIGH_PULLUPREG
        DINS_HIGH_PULLUPREG |= DINS_HIGH_PULLUP_MASK;
    #endif
    
    //outputs
    #ifdef STEPS_DIRREG
        STEPS_DIRREG |= STEPS_MASK;
    #endif
    
	#ifdef DIRS_DIRREG
        DIRS_DIRREG |= DIRS_MASK;
    #endif
    
    #ifdef COM_DIRREG
        COM_DIRREG |= TX_MASK;
    #endif
    
	#ifdef DOUTS_LOW_DIRREG
        DOUTS_LOW_DIRREG |= DOUTS_LOW_MASK;
    #endif
    
    #ifdef DOUTS_HIGH_DIRREG
        DOUTS_HIGH_DIRREG |= DOUTS_HIGH_MASK;
    #endif

    //activate Pin on change interrupt
    PCICR |= ((1<<LIMITS_ISR_ID) | (1<<CONTROLS_ISR_ID));
    
    #ifdef LIMITS_ISRREG
    	LIMITS_ISRREG |= LIMITS_MASK;
    #endif
    #ifdef CONTROLS_ISRREG
    	CONTROLS_ISRREG |= CONTROLS_MASK;
    #endif

    stdout = &g_mcu_streamout;

	//PWM's
	//TCCRXA Mode 1 - Phase correct with TOP 0xFF
	//TCCRXB Prescaller 64
	#ifdef PWM0
		PWM0_DIRREG |= PWM0_MASK;
		#if(PWM0_OCREG==A)
			PWM0_TMRAREG |= 0x41;
		#elif(PWM0_OCREG==B)
			PWM0_TMRAREG |= 0x11;
		#endif
		PWM0_TMRBREG = 3;
		PWM0_CNTREG = 0;
	#endif
	#ifdef PWM1
		PWM1_DIRREG |= PWM1_MASK;
		#if(PWM1_OCREG==A)
			PWM1_TMRAREG |= 0x41;
		#elif(PWM1_OCREG==B)
			PWM1_TMRAREG |= 0x11;
		#endif
		PWM1_TMRBREG = 3;
		PWM1_CNTREG = 0;
	#endif
	#ifdef PWM2
		PWM2_DIRREG |= PWM2_MASK;
		#if(PWM2_OCREG==A)
			PWM2_TMRAREG |= 0x41;
		#elif(PWM2_OCREG==B)
			PWM2_TMRAREG |= 0x11;
		#endif
		PWM2_TMRBREG = 3;
		PWM2_CNTREG = 0;
	#endif
	#ifdef PWM3
		PWM3_DIRREG |= PWM3_MASK;
		#if(PWM3_OCREG==A)
			PWM3_TMRAREG |= 0x41;
		#elif(PWM3_OCREG==B)
			PWM3_TMRAREG |= 0x11;
		#endif
		PWM3_TMRBREG = 3;
		PWM3_CNTREG = 0;
	#endif

    // Set baud rate
    #if BAUD < 57600
      uint16_t UBRR0_value = ((F_CPU / (8L * BAUD)) - 1)/2 ;
      UCSR0A &= ~(1 << U2X0); // baud doubler off  - Only needed on Uno XXX
    #else
      uint16_t UBRR0_value = ((F_CPU / (4L * BAUD)) - 1)/2;
      UCSR0A |= (1 << U2X0);  // baud doubler on for high baud rates, i.e. 115200
    #endif
    UBRR0H = UBRR0_value >> 8;
    UBRR0L = UBRR0_value;
  
    // enable rx, tx, and interrupt on complete reception of a byte and UDR empty
    UCSR0B |= (1<<RXEN0 | 1<<TXEN0 | 1<<RXCIE0);
    
	//enable interrupts
	sei();
}

//IO functions    
//Inputs  
uint16_t mcu_get_inputs()
{
	IO_REGISTER reg;
	reg.r = 0;
	#ifdef DINS_LOW
	reg.r0 = (DINS_LOW & DINS_LOW_MASK);
	#endif
	#ifdef DINS_HIGH
	reg.r1 = (DINS_HIGH & DINS_HIGH_MASK);
	#endif
	return reg.rl;	
}

uint8_t mcu_get_controls()
{
	return (CONTROLS_INREG & CONTROLS_MASK);
}

uint8_t mcu_get_limits()
{
	return (LIMITS_INREG & LIMITS_MASK);
}

uint8_t mcu_get_analog(uint8_t channel)
{
	ADMUX = (0x42 | channel); //VRef = Vcc with reading left aligned
	ADCSRA = 0xC7; //Start read with ADC with 128 prescaller
	while(CHECKBIT(ADCSRA,ADSC));
	uint8_t result = ADCH;
	ADCSRA = 0; //switch adc off
	ADMUX = 0; //switch adc off

	return result;
}

//outputs

//sets all step pins
void mcu_set_steps(uint8_t value)
{
	STEPS_OUTREG = (~STEPS_MASK & STEPS_OUTREG) | value;
}
//sets all dir pins
void mcu_set_dirs(uint8_t value)
{
	DIRS_OUTREG = (~DIRS_MASK & DIRS_OUTREG) | value;
}

void mcu_set_outputs(uint16_t value)
{
	IO_REGISTER reg = {};
	reg.rl = value;
	
	#ifdef DOUTS_LOW_OUTREG
		DOUTS_LOW_OUTREG = (~DOUTS_LOW_MASK & DOUTS_LOW_OUTREG) | reg.r0;
	#endif
	#ifdef DOUTS_HIGH_OUTREG
		DOUTS_HIGH_OUTREG = (~DOUTS_HIGH_MASK & DOUTS_HIGH_OUTREG) | reg.r1;
	#endif
}

void mcu_set_pwm(uint8_t pwm, uint8_t value)
{
	switch(pwm)
	{
		case 0:
			#ifdef PWM0
			PWM0_CNTREG = value;
			if(value != 0)
			{
				#if(PWM0_OCREG==A)
					SETFLAG(PWM0_TMRAREG,0x40);
				#elif(PWM0_OCREG==B)
					SETFLAG(PWM0_TMRAREG,0x10);
				#endif
			}
			else
			{
				if(value != 0)
				{
					#if(PWM0_OCREG==A)
						CLEARFLAG(PWM0_TMRAREG,0x40);
					#elif(PWM0_OCREG==B)
						CLEARFLAG(PWM0_TMRAREG,0x10);
					#endif
				}
			}
			#endif
			break;
		case 1:
			#ifdef PWM1
			PWM1_CNTREG = value;
			if(value != 0)
			{
				#if(PWM1_OCREG==A)
					SETFLAG(PWM1_TMRAREG,0x40);
				#elif(PWM1_OCREG==B)
					SETFLAG(PWM1_TMRAREG,0x10);
				#endif
			}
			else
			{
				if(value != 0)
				{
					#if(PWM1_OCREG==A)
						CLEARFLAG(PWM1_TMRAREG,0x40);
					#elif(PWM1_OCREG==B)
						CLEARFLAG(PWM1_TMRAREG,0x10);
					#endif
				}
			}
			#endif
			break;
		case 2:
			#ifdef PWM2
			PWM2_CNTREG = value;
			if(value != 0)
			{
				#if(PWM2_OCREG==A)
					SETFLAG(PWM2_TMRAREG,0x40);
				#elif(PWM2_OCREG==B)
					SETFLAG(PWM2_TMRAREG,0x10);
				#endif
			}
			else
			{
				if(value != 0)
				{
					#if(PWM2_OCREG==A)
						CLEARFLAG(PWM2_TMRAREG,0x40);
					#elif(PWM2_OCREG==B)
						CLEARFLAG(PWM2_TMRAREG,0x10);
					#endif
				}
			}
			#endif
			break;
		case 3:
			#ifdef PWM3
			PWM3_CNTREG = value;
			if(value != 0)
			{
				#if(PWM3_OCREG==A)
					SETFLAG(PWM3_TMRAREG,0x40);
				#elif(PWM3_OCREG==B)
					SETFLAG(PWM3_TMRAREG,0x10);
				#endif
			}
			else
			{
				if(value != 0)
				{
					#if(PWM3_OCREG==A)
						CLEARFLAG(PWM3_TMRAREG,0x40);
					#elif(PWM3_OCREG==B)
						CLEARFLAG(PWM3_TMRAREG,0x10);
					#endif
				}
			}
			#endif
			break;
	}
}

void mcu_enable_interrupts()
{
	sei();
}
void mcu_disable_interrupts()
{
	cli();
}

//internal redirect of stdout
int mcu_putchar(char c, FILE* stream)
{
	mcu_putc(c);
	return c;
}

void mcu_start_send()
{
	SETBIT(UCSR0B,UDRIE0);
}

void mcu_putc(char c)
{
	loop_until_bit_is_set(UCSR0A, UDRE0);
	UDR0 = c;
}

bool mcu_is_tx_ready()
{
	return CHECKBIT(UCSR0A, UDRE0);
}

char mcu_getc()
{
	loop_until_bit_is_set(UCSR0A, RXC0);
    return UDR0;
}

//RealTime
void mcu_freq_to_clocks(float frequency, uint16_t* ticks, uint8_t* prescaller)
{
	if(frequency < F_STEP_MIN)
		frequency = F_STEP_MIN;
	if(frequency > F_STEP_MAX)
		frequency = F_STEP_MAX;
		
	float clockcounter = F_CPU;
		
	if(frequency >= 245)
	{
		*prescaller = 9;
		
	}
	else if(frequency >= 31)
	{
		*prescaller = 10;
		clockcounter *= 0.125f;
	}
	else if(frequency >= 4)
	{
		*prescaller = 11;
		clockcounter *= 0.015625f;
		
	}
	else if(frequency >= 1)
	{
		*prescaller = 12;
		clockcounter *= 0.00390625f;
	}
	else
	{
		*prescaller = 13;
		clockcounter *= 0.0009765625f;
	}

	*ticks = floorf((clockcounter/frequency)) - 1;
}
/*
	initializes the pulse ISR
	In Arduino this is done in TIMER1
	The frequency range is from 4Hz to F_PULSE
*/
void mcu_start_step_ISR(uint16_t clocks_speed, uint8_t prescaller)
{
	//stops timer
	TCCR1B = 0;
	//CTC mode
    TCCR1A = 0;
    //resets counter
    TCNT1 = 0;
    //set step clock
    OCR1A = clocks_speed;
	//sets OCR0B to half
	//this will allways fire step_reset between pulses
    OCR1B = OCR1A>>1;
	TIFR1 = 0;
	// enable timer interrupts on both match registers
    TIMSK1 |= (1 << OCIE1B) | (1 << OCIE1A);
    
    //start timer in CTC mode with the correct prescaler
    TCCR1B = prescaller;
}

// se implementar amass deixo de necessitar de prescaler
void mcu_change_step_ISR(uint16_t clocks_speed, uint8_t prescaller)
{
	//stops timer
	//TCCR1B = 0;
	OCR1B = clocks_speed>>1;
	OCR1A = clocks_speed;
	//sets OCR0B to half
	//this will allways fire step_reset between pulses
    
	//reset timer
    //TCNT1 = 0;
	//start timer in CTC mode with the correct prescaler
    TCCR1B = prescaller;
}

void mcu_step_stop_ISR()
{
	TCCR1B = 0;
    TIMSK1 &= ~((1 << OCIE1B) | (1 << OCIE1A));
}

/*#define MCU_1MS_LOOP F_CPU/1000000
static __attribute__((always_inline)) void mcu_delay_1ms() 
{
	uint16_t loop = MCU_1MS_LOOP;
	do{
	}while(--loop);
}*/

void mcu_delay_ms(uint16_t miliseconds)
{
	do{
		_delay_ms(1);
	}while(--miliseconds);
	
}

#ifndef EEPE
		#define EEPE  EEWE  //!< EEPROM program/write enable.
		#define EEMPE EEMWE //!< EEPROM master program/write enable.
#endif

/* These two are unfortunately not defined in the device include files. */
#define EEPM1 5 //!< EEPROM Programming Mode Bit 1.
#define EEPM0 4 //!< EEPROM Programming Mode Bit 0.

uint8_t mcu_eeprom_getc(uint16_t address)
{
	do {} while( EECR & (1<<EEPE) ); // Wait for completion of previous write.
	EEAR = address; // Set EEPROM address register.
	EECR = (1<<EERE); // Start EEPROM read operation.
	return EEDR; // Return the byte read from EEPROM.
}

//taken from grbl
uint8_t mcu_eeprom_putc(uint16_t address, uint8_t value)
{
	char old_value; // Old EEPROM value.
	char diff_mask; // Difference mask, i.e. old value XOR new value.

	cli(); // Ensure atomic operation for the write operation.
	
	do {} while( EECR & (1<<EEPE) ); // Wait for completion of previous write
	do {} while( SPMCSR & (1<<SELFPRGEN) ); // Wait for completion of SPM.
	
	EEAR = address; // Set EEPROM address register.
	EECR = (1<<EERE); // Start EEPROM read operation.
	old_value = EEDR; // Get old EEPROM value.
	diff_mask = old_value ^ value; // Get bit differences.
	
	// Check if any bits are changed to '1' in the new value.
	if( diff_mask & value ) {
		// Now we know that _some_ bits need to be erased to '1'.
		
		// Check if any bits in the new value are '0'.
		if( value != 0xff ) {
			// Now we know that some bits need to be programmed to '0' also.
			
			EEDR = value; // Set EEPROM data register.
			EECR = (1<<EEMPE) | // Set Master Write Enable bit...
			       (0<<EEPM1) | (0<<EEPM0); // ...and Erase+Write mode.
			EECR |= (1<<EEPE);  // Start Erase+Write operation.
		} else {
			// Now we know that all bits should be erased.

			EECR = (1<<EEMPE) | // Set Master Write Enable bit...
			       (1<<EEPM0);  // ...and Erase-only mode.
			EECR |= (1<<EEPE);  // Start Erase-only operation.
		}
	} else {
		// Now we know that _no_ bits need to be erased to '1'.
		
		// Check if any bits are changed from '1' in the old value.
		if( diff_mask ) {
			// Now we know that _some_ bits need to the programmed to '0'.
			
			EEDR = value;   // Set EEPROM data register.
			EECR = (1<<EEMPE) | // Set Master Write Enable bit...
			       (1<<EEPM1);  // ...and Write-only mode.
			EECR |= (1<<EEPE);  // Start Write-only operation.
		}
	}
	
	sei(); // Restore interrupt flag state.
}

#endif