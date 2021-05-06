#!/usr/bin/tcc -run

/*
 * mouse.c: a minimal application to print raw
 * escape codes from mouse/keyboard input
 *
 * Requires tcc (for "C script")
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>

struct termios tio, raw;

void shutdown(int signo){
  tcsetattr(1, TCSANOW, &tio);

  printf("\x1b[?1000l");
  exit(0);
}

int main(){
  int nread, i;
  char tmp[256];

  tcgetattr(1, &tio);
  raw = tio;
  raw.c_lflag &= ~(ECHO | ICANON);
  tcsetattr(1, TCSANOW, &raw);

  printf("\x1b[?1000hPress Ctrl+C to exit.\n");
  signal(SIGINT, shutdown);
  while((nread=read(0, tmp, sizeof(tmp))) > 0){
    for(i=0;i<nread;i++){
      printf("0x%.2x ", tmp[i]);
    }
    printf("\n");
  }
  return 0;
}
