/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* termios.h Terminal I/O system types */
#ifndef _TERMIOS_H_INCLUDED
#define _TERMIOS_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef __TYPES_H_INCLUDED
#include <sys/types.h>
#endif

#define NCCS 40

typedef unsigned char cc_t;
typedef long speed_t;
typedef unsigned long tcflag_t;
struct termios {
    tcflag_t c_iflag; /* Input Modes */
    tcflag_t c_oflag; /* Ouput modes */
    tcflag_t c_cflag; /* Control Modes */
    tcflag_t c_lflag; /* Local Modes */
    cc_t c_cc[NCCS];  /* Control Characters */
    uint32_t reserved[3];
    speed_t c_ispeed; /* Input Baud rate */
    speed_t c_ospeed; /* Output baud rate */
};
/* Input modes */

#define IGNBRK 0x00000001
#define BRKINT 0x00000002
#define IGNPAR 0x00000004
#define PARMRK 0x00000008
#define INPCK 0x00000010
#define ISTRIP 0x00000020
#define INLCR 0x00000040
#define IGNCR 0x00000080
#define ICRNL 0x00000100 /* map CR to NL on output (CRMOD) */
#define IXON 0x00000400
#define IXOFF 0x00001000
/* Ouput Modes */

#define OPOST 0x00000001
#define ONLCR 0x00000004 /* map NL to CRNL on output (CRMOD) */
/* Control Modes */

#define CSIZE 0x00000030
#define CS5 0x00
#define CS6 0x10
#define CS7 0x20
#define CS8 0x30
#define CSTOPB 0x00000040
#define CREAD 0x00000080
#define PARENB 0x00000100
#define PARODD 0x00000200
#define HUPCL 0x00000400
#define CLOCAL 0x00000800

/* Local Modes */

#define ISIG 0x00000001
#define ICANON 0x00000002
#define ECHO 0x00000008
#define ECHOE 0x00000010
#define ECHOK 0x00000020
#define ECHONL 0x00000040
#define NOFLSH 0x00000080
#define TOSTOP 0x00000100
#define IEXTEN 0x00008000

/* Special Control Character indices into c_cc[] */

#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VEOL 5
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define VMIN 16
#define VTIME 17

/* Pre-Defined Baud rates used by cfgetospeed(), etc. */

#define B0 0
#define B50 50
#define B75 75
#define B110 110
#define B134 134
#define B150 150
#define B200 200
#define B300 300
#define B600 600
#define B1200 1200
#define B1800 1800
#define B2400 2400
#define B4800 4800
#define B9600 9600
#define B19200 19200
#define B38400 38400u
#define B57600 57600u
#define B76800 76800L
#define B115200 115200L

/* Optional Actions for tcsetattr() */

#define TCSANOW 0x0001
#define TCSADRAIN 0x0002
#define TCSAFLUSH 0x0004

/* queue_selectors for tcflush() */

#define TCIFLUSH 0x0000
#define TCOFLUSH 0x0001
#define TCIOFLUSH 0x0002

/* Actions for tcflow() */

#define TCOOFF 0x0000
#define TCOON 0x0001
#define TCIOFF 0x0002
#define TCION 0x0003

__BEGIN_DECLS
/* POSIX 1003.1 Prototypes. */
extern speed_t cfgetispeed(const struct termios *__termios_p);
extern speed_t cfgetospeed(const struct termios *__termios_p);
extern int cfsetispeed(struct termios *__termios_p, speed_t __speed);
extern int cfsetospeed(struct termios *__termios_p, speed_t __speed);
extern int tcdrain(int __fildes);
extern int tcflow(int __fildes, int __action);
extern int tcflush(int __fildes, int __queue_selector);
extern int tcgetattr(int __fildes, struct termios *__termios_p);
extern int tcsendbreak(int __fildes, int __duration);
extern int tcsetattr(int __fildes,
                     int __opt_act,
                     const struct termios *__termios_p);

__END_DECLS
#endif
