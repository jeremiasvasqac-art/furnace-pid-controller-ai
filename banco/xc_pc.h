/*
 * xc_pc.h - Cabecera falsa de XC8 para probar el firmware en la PC.
 *
 * Reemplaza a <xc.h> cuando se compila con -DBANCO_PC. Modela:
 *   - Registros normales como variables (LATx y su versión por bits comparten byte).
 *   - EEPROM: arreglo eemem[256], con lectura por RD y escritura por WR que exige
 *     WREN = 1, GIE = 0 y la secuencia 0x55 / 0xAA en EECON2.
 *   - Serial: cada byte escrito en TXREG llega a banco_tx().
 *   - MAX6675: cada lectura de PORTAbits.RA2 entrega el siguiente bit del sensor
 *     que tenga su CS en bajo (ver banco.c).
 *   - ADC: al poner GO_nDONE se entrega la lectura de la FVR para el VDD simulado.
 *   - Tiempo: CLRWDT() y __delay_ms() llaman al banco (ahí avanza el reloj).
 *
 * Todo vive en una sola unidad de compilación (banco.c incluye el firmware),
 * por eso los registros son "static".
 */
#ifndef XC_PC_H
#define XC_PC_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __interrupt()
void banco_delay_ms(unsigned ms);
void banco_clrwdt(void);
#define __delay_ms(x) banco_delay_ms(x)
#define __delay_us(x) ((void)0)
#define CLRWDT() banco_clrwdt()

typedef unsigned char ubit;

typedef union { uint8_t b; struct { ubit RA0:1, RA1:1, RA2:1, RA3:1, RA4:1, RA5:1, RA6:1, RA7:1; } s; } PORTA_u;
typedef union { uint8_t b; struct { ubit LATA0:1, LATA1:1, LATA2:1, LATA3:1, LATA4:1, LATA5:1, LATA6:1, LATA7:1; } s; } LATA_u;
typedef union { uint8_t b; struct { ubit LATB0:1, LATB1:1, LATB2:1, LATB3:1, LATB4:1, LATB5:1, LATB6:1, LATB7:1; } s; } LATB_u;
typedef union { uint8_t b; struct { ubit TMR1IF:1, TMR2IF:1, CCP1IF:1, SSP1IF:1, TXIF:1, RCIF:1, ADIF:1, TMR1GIF:1; } s; } PIR1_u;
typedef union { uint8_t b; struct { ubit TMR1IE:1, TMR2IE:1, CCP1IE:1, SSP1IE:1, TXIE:1, RCIE:1, ADIE:1, TMR1GIE:1; } s; } PIE1_u;
typedef union { uint8_t b; struct { ubit IOCIF:1, INTF:1, TMR0IF:1, IOCIE:1, INTE:1, TMR0IE:1, PEIE:1, GIE:1; } s; } INTCON_u;
typedef union { uint8_t b; struct { ubit TX9D:1, TRMT:1, BRGH:1, SENDB:1, SYNC:1, TXEN:1, TX9:1, CSRC:1; } s; } TXSTA_u;
typedef union { uint8_t b; struct { ubit RX9D:1, OERR:1, FERR:1, ADDEN:1, CREN:1, SREN:1, RX9:1, SPEN:1; } s; } RCSTA_u;
typedef union { uint8_t b; struct { ubit RD:1, WR:1, WREN:1, WRERR:1, FREE:1, LWLO:1, CFGS:1, EEPGD:1; } s; } EECON1_u;
typedef union { uint8_t b; struct { ubit nBOR:1, nPOR:1, nRI:1, nRMCLR:1, u4:1, u5:1, STKUNF:1, STKOVF:1; } s; } PCON_u;
typedef union { uint8_t b; struct { ubit C:1, DC:1, Z:1, nPD:1, nTO:1, u5:1, u6:1, u7:1; } s; } STATUS_u;
typedef union { uint8_t b; struct { ubit ADON:1, GO_nDONE:1, CHS:5, u7:1; } s; } ADCON0_u;
typedef union { uint8_t b; struct { ubit ADFVR:2, CDAFVR:2, TSRNG:1, TSEN:1, FVRRDY:1, FVREN:1; } s; } FVRCON_u;

static PORTA_u  r_porta;
static LATA_u   r_lata;
static LATB_u   r_latb;
static PIR1_u   r_pir1;
static PIE1_u   r_pie1;
static INTCON_u r_intcon;
static TXSTA_u  r_txsta = { 0x02 };      // TRMT = 1: el transmisor siempre está libre
static RCSTA_u  r_rcsta;
static EECON1_u r_eecon1;
static PCON_u   r_pcon;
static STATUS_u r_status;
static ADCON0_u r_adcon0;
static FVRCON_u r_fvrcon;

// Accesos con modelo (se definen en banco.c)
void       *banco_porta(void);
void       *banco_fvrcon(void);
static void *banco_txsta(void) { r_txsta.s.TRMT = 1; return &r_txsta; }
void       *banco_eecon1(void);
uint8_t    *banco_eedat(void);
uint8_t    *banco_eecon2(void);
void       *banco_adcon0(void);
char       *banco_txreg(void);

#define PORTAbits   (((PORTA_u *)banco_porta())->s)
#define LATAbits    (r_lata.s)
#define LATBbits    (r_latb.s)
#define LATA        (r_lata.b)
#define LATB        (r_latb.b)
#define PIR1bits    (r_pir1.s)
#define PIE1bits    (r_pie1.s)
#define INTCONbits  (r_intcon.s)
#define TXSTAbits   (((TXSTA_u *)banco_txsta())->s)   /* TRMT es de solo lectura: siempre 1 */
#define TXSTA       (r_txsta.b)
#define RCSTAbits   (r_rcsta.s)
#define RCSTA       (r_rcsta.b)
#define EECON1bits  (((EECON1_u *)banco_eecon1())->s)
#define EEDATL      (*banco_eedat())
#define EECON2      (*banco_eecon2())
#define PCONbits    (r_pcon.s)
#define PCON        (r_pcon.b)
#define STATUSbits  (r_status.s)
#define ADCON0bits  (((ADCON0_u *)banco_adcon0())->s)
#define ADCON0      (r_adcon0.b)
#define FVRCONbits  (((FVRCON_u *)banco_fvrcon())->s)
#define FVRCON      (r_fvrcon.b)
#define TXREG       (*banco_txreg())

static volatile uint8_t TRISA, TRISB, OPTION_REG, PR2, CCPR1L, CCP1CON, CCPTMRS, T2CON, T1CON,
    TMR1H, TMR1L, RCREG, EEADRL, ANSELA, ANSELB, CM1CON0, CM2CON0, APFCON0, APFCON1,
    WDTCON, BAUDCON, SPBRGH, SPBRGL, ADCON1, ADRESH, ADRESL;

#endif
