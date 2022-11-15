/* ########################################################################

   simple serial sniffer using tty0tty kernel module

   ########################################################################

   Copyright (c) : 2022  Luis Claudio Gambôa Lopes

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

   For e-mail suggestions :  lcgamboa@yahoo.com
   ######################################################################## */

#include "/usr/include/asm-generic/ioctls.h"
#include "/usr/include/asm-generic/termbits.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

// color defines
#define BLACK 0
#define RED 1
#define GREEN 2
#define YELLOW 3
#define BLUE 4
#define MAGNETA 5
#define CYAN 6
#define WHITE 7
#define ANSI_DEFAULT() printf("\033[0m")
#define ANSI_COLOR(f, b) printf("\033[%d;%dm", (f) + 30, (b) + 40)
#define ANSI_FG_LCOLOR(f) printf("\033[0;%dm", (f) + 30)
#define ANSI_FG_HCOLOR(f) printf("\033[1;%dm", (f) + 30)

static void updatectrl(const int hardware, const int virtual,
                       const int splitter, const int force);

static int SerialOpen(const char *portname);
static int SerialClose(const int serialfd);
static int SerialConfig(const int serialfd, const unsigned int speed);
static void SerialSetModem(const int serialfd, const unsigned long data);
static unsigned int SerialGetModem(const int serialfd);
static int SerialSendBuff(const int serialfd, unsigned char *c, const int size);
static int SerialReceiveBuff(const int serialfd, unsigned char *c,
                             const int size);

static void printformated(const char *portname, const unsigned char *buff,
                          const int size, const int color);

void intHandler(int signal);

#define SHARDWARE 0
#define SVIRTUAL 1
#define SVBAUD 2
#define SVSPLITTER 3
#define SVSBAUD 4

static int exitflag = 0;
static int mode_color = 0;
static int mode_splitter = 0;
static int baud = 115200;
static int sbaud = 0;

int main(int argc, char **argv) {
  int cnt;
  char attrData[100];
  int doctrl;
  int tntn, tntn_;
  char tntname[100];
  char tntdevice[100];
  int spn, spn_;
  char splittername[100];
  char splitterdevice[100];
  struct pollfd fdsa[5];
  int fdsc = 3;

  if ((argc < 3) || (argc > 4)) {
    printf("\nusage:%s harware_port virtual_port_number [mode]\n", argv[0]);
    printf("  Arguments:\n");
    printf("      hardware_port      : Real device port. ex /dev/ttyUSB0\n");
    printf("      virtual_port_number: Virtual tty0tty port number [0..7]\n");
    printf("      mode               : color      - add support to use colors "
           "in console output\n");
    printf("                           ysplitter[0..7]  - for redirect data "
           "to a "
           "third virtual port\n\n");
    return -1;
  }

  if (!(fdsa[SHARDWARE].fd = SerialOpen(argv[1]))) {
    return -1;
  }

  sscanf(argv[2], "%i", &tntn);
  sprintf(tntname, "/dev/tnt%i", tntn);
  tntn_ = (tntn & 1) ? tntn - 1 : tntn + 1;

  if (!(fdsa[SVIRTUAL].fd = SerialOpen(tntname))) {
    SerialClose(fdsa[SHARDWARE].fd);
    return -1;
  }

  printf("Connect application on port: /dev/tnt%i\n", tntn_);

  sprintf(tntdevice, "/sys/devices/virtual/tty/tnt%i/baudrate", tntn_);
  if ((fdsa[SVBAUD].fd = open(tntdevice, O_RDONLY)) < 0) {
    perror("Unable to open baudrate");
    exit(1);
  }

  if (argc == 4) {
    if (!strcmp(argv[3], "color")) {
      mode_color = 1;
    }
    if (!strncmp(argv[3], "ysplitter", 9)) {
      mode_splitter = 1;
      sscanf(argv[3] + 9, "%i", &spn);
      sprintf(splittername, "/dev/tnt%i", spn);
      spn_ = (spn & 1) ? spn - 1 : spn + 1;

      if (!(fdsa[SVSPLITTER].fd = SerialOpen(splittername))) {
        SerialClose(fdsa[SVSPLITTER].fd);
        return -1;
      }

      printf("Connect second application on port: /dev/tnt%i\n", spn_);

      sprintf(splitterdevice, "/sys/devices/virtual/tty/tnt%i/baudrate", spn_);
      if ((fdsa[SVSBAUD].fd = open(splitterdevice, O_RDONLY)) < 0) {
        perror("Unable to open baudrate");
        exit(1);
      }
      fdsc += 2;
    }
  }

  fdsa[SHARDWARE].events = POLLIN;
  fdsa[SVIRTUAL].events = POLLIN;
  fdsa[SVBAUD].events = POLLPRI | POLLERR;
  fdsa[SVSPLITTER].events = POLLIN;
  fdsa[SVSBAUD].events = POLLPRI | POLLERR;

  fdsa[SVSPLITTER].revents = 0;
  fdsa[SVSBAUD].revents = 0;

  // read baudrate
  if (mode_splitter) {
    cnt = read(fdsa[SVSBAUD].fd, attrData, 99);
    if ((lseek(fdsa[SVSBAUD].fd, 0L, SEEK_SET)) < 0) {
      fprintf(stderr, "Failed to set pointer\n");
      exit(2);
    }
    attrData[cnt] = 0;
    sscanf(attrData, "%i", &sbaud);
  }
  cnt = read(fdsa[SVBAUD].fd, attrData, 99);
  if ((lseek(fdsa[SVBAUD].fd, 0L, SEEK_SET)) < 0) {
    fprintf(stderr, "Failed to set pointer\n");
    exit(2);
  }
  attrData[cnt] = 0;
  sscanf(attrData, "%i", &baud);
  if (baud == 0) {
    if (sbaud == 0) {
      SerialConfig(fdsa[SHARDWARE].fd, 115200); // default value
    } else {
      SerialConfig(fdsa[SHARDWARE].fd, sbaud);
    }
  } else {
    SerialConfig(fdsa[SHARDWARE].fd, baud);
  }

  signal(SIGINT, intHandler); // catch ctrl+c

  updatectrl(fdsa[SHARDWARE].fd, fdsa[SVIRTUAL].fd, fdsa[SVSPLITTER].fd, 1);

  while (!exitflag) {
    if ((poll(fdsa, fdsc, 100)) < 0) {
      perror("poll error");
      break;
    }

    doctrl = 1;

    if ((fdsa[SHARDWARE].revents & POLLIN) ||
        (fdsa[SVIRTUAL].revents & POLLIN) ||
        (fdsa[SVSPLITTER].revents & POLLIN)) {
      do {
        cnt = 0;
        int size;
        unsigned char buffer[32];
        if ((size = SerialReceiveBuff(fdsa[SHARDWARE].fd, buffer, 32)) > 0) {
          if (!mode_splitter)
            printformated(argv[1], buffer, size, RED);
          SerialSendBuff(fdsa[SVIRTUAL].fd, buffer, size);
          if (mode_splitter)
            SerialSendBuff(fdsa[SVSPLITTER].fd, buffer, size);
          cnt++;
        }
        doctrl = 0;

        if ((size = SerialReceiveBuff(fdsa[SVIRTUAL].fd, buffer, 32)) > 0) {
          if (!mode_splitter)
            printformated(tntname, buffer, size, BLUE);
          SerialSendBuff(fdsa[SHARDWARE].fd, buffer, size);
          if (mode_splitter)
            SerialSendBuff(fdsa[SVSPLITTER].fd, buffer, size);
          cnt++;
        }
        if (mode_splitter) {
          if ((size = SerialReceiveBuff(fdsa[SVSPLITTER].fd, buffer, 32)) > 0) {
            SerialSendBuff(fdsa[SHARDWARE].fd, buffer, size);
            SerialSendBuff(fdsa[SVIRTUAL].fd, buffer, size);
            cnt++;
          }
        }

        updatectrl(fdsa[SHARDWARE].fd, fdsa[SVIRTUAL].fd, fdsa[SVSPLITTER].fd,
                   0);
      } while (cnt);
      doctrl = 0;
    }

    if (fdsa[SVBAUD].revents & POLLPRI) {
      cnt = read(fdsa[SVBAUD].fd, attrData, 99);
      if ((lseek(fdsa[SVBAUD].fd, 0L, SEEK_SET)) < 0) {
        fprintf(stderr, "Failed to set pointer\n");
        exit(2);
      }
      attrData[cnt] = 0;
      sscanf(attrData, "%i", &baud);
      if (mode_color)
        ANSI_FG_HCOLOR(GREEN);
      if (baud > 0) {
        printf("Baudrate speed set to (%i)\n", baud);
        SerialConfig(fdsa[SHARDWARE].fd, baud);
      } else {
        printf("Port Closed !!!\n");
        if (sbaud > 0) {
          printf("Baudrate speed set to (%i)\n", sbaud);
          SerialConfig(fdsa[SHARDWARE].fd, sbaud);
        }
      }
      if (mode_color)
        ANSI_DEFAULT();
      updatectrl(fdsa[SHARDWARE].fd, fdsa[SVIRTUAL].fd, fdsa[SVSPLITTER].fd, 1);
      doctrl = 0;
    }
    if (fdsa[SVSBAUD].revents & POLLPRI) {
      cnt = read(fdsa[SVSBAUD].fd, attrData, 99);
      if ((lseek(fdsa[SVSBAUD].fd, 0L, SEEK_SET)) < 0) {
        fprintf(stderr, "Failed to set pointer\n");
        exit(2);
      }
      attrData[cnt] = 0;
      sscanf(attrData, "%i", &sbaud);
      if (mode_color)
        ANSI_FG_HCOLOR(GREEN);
      if (baud == 0) { // no primary port connected
        if (sbaud > 0) {
          printf("Baudrate speed set to (%i)\n", sbaud);
          SerialConfig(fdsa[SHARDWARE].fd, sbaud);
        } else {
          printf("Splitter port Closed !!!\n");
        }
      }
      if (mode_color)
        ANSI_DEFAULT();
      updatectrl(fdsa[SHARDWARE].fd, fdsa[SVIRTUAL].fd, fdsa[SVSPLITTER].fd, 1);
      doctrl = 0;
    }
    if (doctrl) {
      updatectrl(fdsa[SHARDWARE].fd, fdsa[SVIRTUAL].fd, fdsa[SVSPLITTER].fd, 0);
    }
  }
  SerialClose(fdsa[SHARDWARE].fd);
  SerialClose(fdsa[SVIRTUAL].fd);
  close(fdsa[SVBAUD].fd);

  if (mode_splitter) {
    SerialClose(fdsa[SVSPLITTER].fd);
    close(fdsa[SVSBAUD].fd);
  }

  ANSI_DEFAULT();
  return 0;
}

/*
RTS -->  CTS
DTR -->  DSR
    |->  CD

CTS <-   RTS
DSR <--  DTR
CD  <-|
*/

static void updatectrl(const int hardware, const int virtual,
                       const int splitter, const int force) {

  static unsigned long vmodem_old = 0XFFFFFFFF;
  static unsigned long hmodem_old = 0XFFFFFFFF;
  static unsigned long vsmodem_old = 0XFFFFFFFF;

  const unsigned long mask = TIOCM_CTS | TIOCM_DSR;

  unsigned long hmodem = SerialGetModem(hardware);
  unsigned long vmodem = SerialGetModem(virtual);
  unsigned long vsmodem = 0;

  if (baud > 0) {
    if (((vmodem & mask) != (vmodem_old & mask)) || force) {
      if (vmodem & TIOCM_CTS) {
        hmodem |= TIOCM_RTS;
      } else {
        hmodem &= ~TIOCM_RTS;
      }

      if (vmodem & TIOCM_DSR) {
        hmodem |= TIOCM_DTR;
      } else {
        hmodem &= ~TIOCM_DTR;
      }

      if (mode_color)
        ANSI_FG_HCOLOR(YELLOW);
      printf("Output modem signal changed: RTS=%i DTR=%i \n",
             (vmodem & TIOCM_CTS) > 0, (vmodem & TIOCM_DSR) > 0);
      if (mode_color)
        ANSI_DEFAULT();
    }
    vmodem_old = vmodem;
    vsmodem_old = 0XFFFFFFFF;
    SerialSetModem(hardware, hmodem);
  } else if (sbaud > 0) {
    vsmodem = SerialGetModem(splitter);
    if (((vsmodem & mask) != (vsmodem_old & mask)) || force) {
      if (vsmodem & TIOCM_CTS) {
        hmodem |= TIOCM_RTS;
      } else {
        hmodem &= ~TIOCM_RTS;
      }

      if (vsmodem & TIOCM_DSR) {
        hmodem |= TIOCM_DTR;
      } else {
        hmodem &= ~TIOCM_DTR;
      }
      printf("Output modem signal changed: RTS=%i DTR=%i \n",
             (vsmodem & TIOCM_CTS) > 0, (vsmodem & TIOCM_DSR) > 0);
    }
    vsmodem_old = vsmodem;
    vmodem_old = 0XFFFFFFFF;
    SerialSetModem(hardware, hmodem);
  }

  if ((hmodem & mask) != (hmodem_old & mask)) {
    if (hmodem & TIOCM_CTS) {
      vmodem |= TIOCM_RTS;
      vsmodem |= TIOCM_RTS;
    } else {
      vmodem &= ~TIOCM_RTS;
      vsmodem &= ~TIOCM_RTS;
    }

    if (hmodem & TIOCM_DSR) {
      vmodem |= TIOCM_DTR;
      vsmodem |= TIOCM_DTR;
    } else {
      vmodem &= ~TIOCM_DTR;
      vsmodem &= ~TIOCM_DTR;
    }

    if (mode_color)
      ANSI_FG_HCOLOR(CYAN);
    printf("Input modem signal changed: CTS=%i DSR=%i \n",
           (hmodem & TIOCM_CTS) > 0, (hmodem & TIOCM_DSR) > 0);
    if (mode_color)
      ANSI_DEFAULT();
  }
  hmodem_old = hmodem;
  SerialSetModem(virtual, vmodem);
  if (sbaud > 0) {
    SerialSetModem(splitter, vsmodem);
  }
}

static void printformated(const char *portname, const unsigned char *buff,
                          const int size, const int color) {
  if (!size)
    return;

  int ptr = 0;
  int n;
  do {
    if (mode_color)
      ANSI_FG_HCOLOR(color);
    printf("%15s: ", portname);
    if (mode_color)
      ANSI_DEFAULT();

    for (int i = 0; i < 8; i++) {
      n = i + ptr;
      if (n < size)
        printf("%02X ", buff[n]);
      else
        printf("   ");
    }
    printf(" ");
    for (int i = 0; i < 8; i++) {
      n = 8 + i + ptr;
      if (n < size)
        printf("%02X ", buff[n]);
      else
        printf("   ");
    }
    printf(" | ");
    for (int i = 0; i < 16; i++) {
      n = i + ptr;
      if (n < size) {
        if (((buff[n] > 0x20) && (buff[n] < 0x7F)) || (buff[n] > 0xA0)) {
          printf("%c", buff[n]);
        } else {
          printf(".");
        }
      } else
        printf(" ");
    }
    printf("\n");
    ptr += 16;
  } while (ptr < size);
}

// serial port functions

static int SerialOpen(const char *portname) {
  int serialfd = open(portname, O_RDWR | O_NOCTTY | O_NONBLOCK);

  if (serialfd < 0) {
    perror(portname);
    printf("Erro on Port Open:%s!\n", portname);
    return 0;
  }

  printf("Port Open:%s!\n", portname);
  return serialfd;
}

static int SerialClose(int serialfd) {
  if (serialfd != 0) {
    close(serialfd);
    serialfd = 0;
  }
  return 0;
}

static int SerialConfig(int serialfd, unsigned int speed) {
  struct termios2 tio;
  ioctl(serialfd, TCGETS2, &tio);
  tio.c_cflag &= ~CBAUD;
  tio.c_cflag |= BOTHER;
  tio.c_cflag |= CS8 | CLOCAL | CREAD;
  tio.c_ispeed = speed;
  tio.c_ospeed = speed;
  tio.c_iflag = 0;
  tio.c_oflag = 0;
  tio.c_lflag = 0;
  tio.c_cc[VTIME] = 0; /* inter-character timer unused */
  tio.c_cc[VMIN] = 0;  /* blocking read until 5 chars received */
  return ioctl(serialfd, TCSETS2, &tio);
}

static void SerialSetModem(const int serialfd, const unsigned long data) {
  ioctl(serialfd, TIOCMSET, &data);
}

static unsigned int SerialGetModem(const int serialfd) {
  unsigned long state;
  ioctl(serialfd, TIOCMGET, &state);
  return state;
}

static int SerialSendBuff(const int serialfd, unsigned char *c,
                          const int size) {
  if (serialfd) {
    return write(serialfd, c, size);
  } else
    return 0;
}

static int SerialReceiveBuff(const int serialfd, unsigned char *c,
                             const int size) {
  if (serialfd) {
    int ret = read(serialfd, c, size);
    return ret;
  } else
    return 0;
}

void intHandler(int signal) { exitflag = 1; }
