/*
 * malt.c: a terminal multiplexer
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <signal.h>
#include <wchar.h>
#include <poll.h>
#include <pty.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include "libspool.h"

#define BUFSIZE 4096
#define MAXPTYS 16
#define MAXROW  100
#define MAXCOL  400
#define TIMEOUT 10*1000

#define GLYPH(x, y) p->buf[(y*p->box_w)+x]
#define RUNE(x, y)  GLYPH(x, y).rune
#define FMT(x, y)   GLYPH(x, y).fmt
#define PUTGLYPH(x, y) printf("\x1b[%i;%iH%s%s", y+p->box_y, x+p->box_x, FMT(x, y), RUNE(x, y))
#define PUTCURSOR(x, y) printf("\x1b[%i;%iH", y+p->box_y, x+p->box_x)

#define CURSOR_GLYPH GLYPH(p->cur_x, p->cur_y)
#define CURSOR_RUNE  CURSOR_GLYPH.rune
#define CURSOR_FMT   CURSOR_GLYPH.fmt

#define CURSOR_ADVANCE_CHAR() p->cur_x++;if(p->cur_x>p->box_w){CURSOR_ADVANCE_LINE();}
#define CURSOR_ADVANCE_LINE() p->cur_x=1;p->cur_y++;if(p->cur_y+1>p->box_h){p->cur_y=1;}
//memmove(p->buf, p->buf+(p->box_w*sizeof(glyph)), (p->box_w*(p->box_h-1))*sizeof(glyph));p->cur_y--;malt_draw(full, p);}

#define ESC_NO_ARG -1

enum parser_state {
  await,
  esc,
  arg,
  finish
};

typedef struct glyph {
  unsigned char rune[6];
  char fmt[64];
} glyph;

typedef struct pty {
  int   fd, ind;
  struct pollfd *pfd;
  struct winsize ws;
  pid_t pid;
  glyph buf[MAXROW*MAXCOL];
  char  fmt[64];
  int   cur_x, cur_y;
  int   box_x, box_y;
  int   box_w, box_h;
  char  mouse, cursor;
} pty;

enum {
  line,
  full,
  deco
};

struct pollfd fds[MAXPTYS+1];
struct termios tio, raw;
struct winsize ws;
pool *pool_ptys;
pty *pty_cur;
int npty = 0, doflush = 1;
int esc_state = await,
    arg_ind = 0,
    args[16] = {0};

void sig(int s __attribute__((unused))){
  kill(s, pty_cur->pid);
}

static inline void utf8ify(unsigned char *buf, int *ind, unsigned char *rune){
  rune[0] = buf[*ind];

  if(buf[*ind] <= 0x7f){
    rune[1] = '\0';
    return;
  }

  rune[1] = buf[(*ind)+1];
  if((buf[*ind] & 0xe0) == 0xe0){
    rune[2] = buf[(*ind)+2];
    if((buf[*ind] & 0xf0) == 0xf0){
      rune[3] = buf[(*ind)+3];
      rune[4] = '\0';
      *ind += 3;
    } else {
      rune[3] = '\0';
      *ind += 2;
    }
  } else {
    *ind += 1;
  }
}

void malt_draw(int type, pty *p){
  int x, y;

  if(type == line){
    for(x=1;x<p->box_w;x++){
      PUTGLYPH(x, p->cur_y);
    }
    printf("\x1b[%i;%iH\x1b[0m\x1b[?25%c", pty_cur->cur_y+pty_cur->box_y, pty_cur->cur_x+pty_cur->box_x, pty_cur->cursor);
  } else if(type == full){
    puts("\x1b[?25l");
    for(y=1;y<p->box_h;y++){
      for(x=1;x<p->box_w;x++){
        PUTGLYPH(x, y);
      }
    }
    malt_draw(deco, p);
  } else if(type == deco){
    if(pty_cur == p) puts("\x1b[48;2;0;128;64m\x1b[?25l");
    else {
      if(p->cursor == 'h') printf("\x1b[48;2;128;128;128m\x1b[%i;%iH ", p->cur_y+p->box_y, p->cur_x+p->box_x);
      puts("\x1b[48;2;64;64;64m\x1b[?25l");
    }
    for(x=p->box_x;x<=p->box_x+p->box_w;x++){
      printf("\x1b[%i;%iH \x1b[%i;%iH ", p->box_y, x, p->box_y+p->box_h, x);
    }
    for(y=p->box_y;y<p->box_y+p->box_h;y++){
      printf("\x1b[%i;%iH \x1b[%i;%iH ", y, p->box_x, y, p->box_x+p->box_w);
    }
    printf("\x1b[%i;%iH\x1b[0m\x1b[?25%c", pty_cur->cur_y+pty_cur->box_y, pty_cur->cur_x+pty_cur->box_x, pty_cur->cursor);
  }
  if(doflush) fflush(stdout);
}

void malt_tile(){
  int count = pool_count(pool_ptys),
      rows, cols,
      col_h, col_w,
      row_n, col_n,
      i, ind;
  pty *p;

  if(count == 0) return;

  for(cols=0;cols<=count/2;cols++){
    if(cols*cols >= count) break;
  }

  rows = count / cols;

  col_h = ws.ws_row;
  col_w = ws.ws_col / (cols ? cols : 1);
  col_n = 0;
  row_n = 0;
  i = 0;

  doflush = 0;
  puts("\x1b[2J");
  pool_foreach_nodecl(pool_ptys){
    p = pool_get(ind, pool_ptys);

    if(i / rows + 1 > cols - count % cols){
      rows = count / cols + 1;
    }

    p->box_x = (col_n * col_w) + 1;
    p->box_y = ((row_n * col_h) / rows) + 1;
    p->box_w = col_w - 2;
    p->box_h = (col_h / rows) - 2;

    p->ws.ws_row = p->box_h;
    p->ws.ws_col = p->box_w;
    ioctl(p->fd, TIOCSWINSZ, &p->ws);

    malt_draw(full, p);

    if(++row_n >= rows){
      row_n = 0;
      col_n++;
    }
    i++;
  }
  doflush = 1;
  fflush(stdout);
}

/*
 * Simple goto-based finite-state machine,
 * inspired by sajson
 */
void esc_parse(unsigned char *in, int n, pty *p){
  int i = -1, j = 0;
  char intermed = '\0';

  switch(esc_state){
    case esc:
      goto csi_or_esc;
    case arg:
      goto args;
    case finish:
      i = 0;
      goto finish_seq;
    default:
      arg_ind = 0;
      args[0] = ESC_NO_ARG;
      break;
  }

await_esc:;
  esc_state = await;
  if(++i >= n) return;

  switch(in[i]){
    case '\x1b':
      goto csi_or_esc;
    case '\n':
      CURSOR_ADVANCE_LINE();
      break;
    case '\r':
      p->cur_x = 1;
      break;
    case '\b':
      if(p->cur_x > 1) p->cur_x--;
      malt_draw(line, p);
      break;
    case '\a':
      /* bell */
      break;
    default:
      utf8ify(in, &i, CURSOR_RUNE);
      strcpy(CURSOR_FMT, p->fmt);
      PUTGLYPH(p->cur_x, p->cur_y);
      CURSOR_ADVANCE_CHAR();
      break;
  }
  goto await_esc;

csi_or_esc:;
  esc_state = esc;
  if(in[++i] == '['){
    goto args;
  } else if(in[i] >= '@' &&
            in[i] <= '_'){
    if(in[i] == 'c'){
      for(
        j=0;
        j<p->box_w*p->box_h;
        j++
      ){
        p->buf[j].rune[0] = '\0';
        p->buf[j].fmt[0] = '\0';
      }
      p->cur_x = 1;
      p->cur_y = 1;
      p->fmt[0] = '\0';
      malt_draw(full, p);
    }
  }
  i--;
  goto await_esc;

args:;
  esc_state = arg;
  if(in[++i] >= '0' &&
     in[i]   <= '9'){
    if(args[arg_ind] == ESC_NO_ARG) args[arg_ind] = in[i] - '0';
    else {
      args[arg_ind] *= 10;
      args[arg_ind] += in[i] - '0';
    }
  } else if(in[i] == ';'){
    args[++arg_ind] = ESC_NO_ARG;
  } else if(in[i] == '?'){
    intermed = '?';
  } else {
    arg_ind++;
    goto finish_seq;
  }
  goto args;

finish_seq:;
  esc_state = finish;
  if(in[i] >= 'A' &&
     in[i] <= 'H'){
    if(args[0] == ESC_NO_ARG) args[0] = 1;
    if(args[1] == ESC_NO_ARG) args[1] = 1;
  }
  switch(in[i]){
    case 'A':
      if(p->cur_y > 1) p->cur_y -= args[0];
      break;
    case 'B':
      if(p->cur_y < p->box_h) p->cur_y += args[0];
      break;
    case 'C':
      if(p->cur_x < p->box_w) p->cur_x += args[0];
      break;
    case 'D':
      if(p->cur_x > 1) p->cur_x -= args[0];
      break;
    case 'E':
      p->cur_x = 1;
      p->cur_y += args[0];
      break;
    case 'F':
      p->cur_x = 1;
      p->cur_y -= args[0];
      break;
    case 'G':
      p->cur_x = args[0];
      break;
    case 'H':
      p->cur_y = args[0];
      if(arg_ind == 0) p->cur_x = args[0];
      else if(arg_ind == 2) p->cur_x = args[1];

      if(p->cur_x > p->box_w) p->cur_x = p->box_w;
      if(p->cur_y > p->box_h) p->cur_y = p->box_h;
      break;
    case 'J':
      if(args[0] == ESC_NO_ARG) args[0] = 0;
      if(arg_ind == 0 ||
         args[0] == 0){
        for(
          j=(p->cur_y*p->box_w)+p->cur_x;
          j<((p->cur_y*p->box_w)+p->cur_x)+(((p->box_h-p->cur_y)*p->box_w)-p->cur_x);
          j++
        ){
          p->buf[j].rune[0] = ' ';
          p->buf[j].rune[1] = '\0';
          p->buf[j].fmt[0] = '\0';
        }
      } else if(args[0] == 1){
        for(
          j=0;
          j<((p->cur_y*p->box_w)-p->cur_x);
          j++
        ){
          p->buf[j].rune[0] = ' ';
          p->buf[j].rune[1] = '\0';
          p->buf[j].fmt[0] = '\0';
        }
      } else if(args[0] == 2){
        for(
          j=0;
          j<p->box_w*p->box_h;
          j++
        ){
          p->buf[j].rune[0] = ' ';
          p->buf[j].rune[1] = '\0';
          p->buf[j].fmt[0] = '\0';
        }
      }
      malt_draw(full, p);
      break;
    case 'K':
      if(args[0] == ESC_NO_ARG) args[0] = 0;
      if(arg_ind == 0 ||
         args[0] == 0){
        for(
          j=(p->cur_y*p->box_w)+p->cur_x;
          j<(p->cur_y*p->box_w)+p->cur_x+(p->box_w-p->cur_x);
          j++
        ){
          p->buf[j].rune[0] = ' ';
          p->buf[j].rune[1] = '\0';
          p->buf[j].fmt[0] = '\0';
        }
      } else if(args[0] == 1){
        for(
          j=(p->cur_y*p->box_w);
          j<(p->cur_y*p->box_w)+p->cur_x;
          j++
        ){
          p->buf[j].rune[0] = ' ';
          p->buf[j].rune[1] = '\0';
          p->buf[j].fmt[0] = '\0';
        }
      } else if(args[0] == 2){
        for(
          j=(p->cur_y*p->box_w);
          j<(p->cur_y*p->box_w)+p->box_w;
          j++
        ){
          p->buf[j].rune[0] = ' ';
          p->buf[j].rune[1] = '\0';
          p->buf[j].fmt[0] = '\0';
        }
      }
      malt_draw(line, p);
      break;
    case 'h':
    case 'l':
      if(intermed == '?' && arg_ind > 0){
        if(args[0] >= 1000 && args[0] <= 1006){
          p->mouse = in[i];
        } else if(args[0] == 25){
          p->cursor = in[i];
        }
      }
      break;
    case 'm':
      strcpy(p->fmt, "\x1b[");
      for(j=0;j<arg_ind;j++){
        if(j > 0) strcat(p->fmt, ";");
        if(args[j] == ESC_NO_ARG) args[j] = 0;
        sprintf(p->fmt+strlen(p->fmt), "%i", args[j]);
      }
      strcat(p->fmt, "m");
      break;
  }
  if(in[i] >= 'A' &&
     in[i] <= 'H'){
    malt_draw(deco, p);
  } else if(in[i] >= 'J' &&
            in[i] <= 'K'){
    malt_draw(full, p);
  }
  intermed = '\0';
  args[(arg_ind=0)] = 0;
  goto await_esc;
}

void pty_add(){
  pty *p;

  if(!pool_ptys){
    pool_ptys = pool_init(MAXPTYS);

    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
  }

  p = malloc(sizeof(pty));

  if((p->pid
        = forkpty(&(p->fd), NULL, NULL, NULL))
      == 0){
    setsid();
    ioctl(p->fd, TIOCSCTTY, NULL);
    execvp("bash", NULL);
  } else {
    pty_cur = p;

    p->cur_x = 1;
    p->cur_y = 1;

    p->mouse = 'l';
    p->cursor = 'h';

    p->ind = pool_push(p, pool_ptys);
    malt_tile();

    fds[++npty].fd = p->fd;
    fds[npty].events = POLLIN;
    p->pfd = &fds[npty];
  }
}

void pty_del(int s __attribute__((unused))){
  pid_t child;
  int status;
  pty *p;

  while((child=waitpid(-1, &status, WNOHANG)) > 0){
    pool_foreach(pool_ptys){
      p = pool_get(ind, pool_ptys);
      if(p->pid == child) goto hit;
    }
    return;
hit:;
    pool_pop(ind, pool_ptys);
    if(pool_count(pool_ptys) == 0){
      pool_foreach(pool_ptys){
        free(pool_get(ind, pool_ptys));
      }
      pool_free(pool_ptys);
      tcsetattr(STDOUT_FILENO, TCSANOW, &tio);
      printf("\x1b[2J\x1b[?1000l\x1b[?25h\x1b[0;0H");
      exit(0);
    } else {
      if(pty_cur == p) pty_cur = pool_get(pool_adj(ind, DIR_RIGHT, pool_ptys), pool_ptys);
      free(p);
      malt_tile();
    }
  }
}

_Noreturn void malt_loop(){
  int nread, cmd = 0;
  unsigned char tmp[BUFSIZE];
  pty *p, *pty_old;

  for(;;){
    if(poll(fds, npty+1, TIMEOUT) > 0){
      if(fds[0].revents & POLLIN){
        nread = read(STDIN_FILENO, tmp, BUFSIZE);
        if(nread > 0){
          if(tmp[0] == '\x1b' &&
             tmp[1] == '[' &&
             tmp[2] == 'M'){ /* Mouse down */
            pool_foreach(pool_ptys){
              p = pool_get(ind, pool_ptys);
              if(p->box_x <= (tmp[4]&0xff) - 0x21 &&
                 p->box_x+p->box_w >= (tmp[4]&0xff) - 0x21 &&
                 p->box_y <= (tmp[5]&0xff) - 0x21 &&
                 p->box_y+p->box_h >= (tmp[5]&0xff) - 0x21){
                pty_old = pty_cur;
                pty_cur = p;
                malt_draw(deco, pty_old);
                malt_draw(deco, pty_cur);
                break;
              }
            }
            if(p->mouse == 'l') goto skip_write;
          } else if(tmp[0] == 0x01){ /* Ctrl+A */
            cmd = 1;
            goto skip_write;
          }

          if(cmd){
            cmd = 0;
            switch(tmp[0]){
              case 'c':
              case 'C':
                pty_add();
                break;
              case 'q': /* XXX: Debug */
                exit(0);
                break;
              case '\x1b':
                if(tmp[1] == '['){
                  pty_old = NULL;
                  switch(tmp[2]){
                    case 'A':
                    case 'C':
                      pty_old = pty_cur;
                      pty_cur = pool_get(pool_adj(pty_cur->ind, DIR_RIGHT, pool_ptys), pool_ptys);
                      break;
                    case 'B':
                    case 'D':
                      pty_old = pty_cur;
                      pty_cur = pool_get(pool_adj(pty_cur->ind, DIR_LEFT, pool_ptys), pool_ptys);
                      break;
                  }
                  if(pty_old != NULL){
                    malt_draw(deco, pty_old);
                    malt_draw(deco, pty_cur);
                  }
                }
                break;
            }
            goto skip_write;
          }

          write(pty_cur->fd, tmp, nread);
skip_write:;
          memset(tmp, '\0', BUFSIZE);
        }
      }

      pool_foreach(pool_ptys){
        p = pool_get(ind, pool_ptys);
        if(p->pfd->revents & POLLIN){
          nread = read(p->fd, tmp, BUFSIZE);
          if(nread > 0){
            esc_parse(tmp, nread, p);
            fflush(stdout);
            memset(tmp, '\0', BUFSIZE);
          }
        }
      }
    }
  }
}

int main(){
  tcgetattr(STDOUT_FILENO, &tio);
  raw = tio;
  raw.c_lflag &= ~(ECHO | ICANON);
  tcsetattr(STDOUT_FILENO, TCSANOW, &raw);
  ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
  setvbuf(stdout, NULL, _IOFBF, 4096);

  signal(SIGINT,  sig);
  signal(SIGTERM, sig);
  signal(SIGQUIT, sig);
  signal(SIGCHLD, pty_del);

  puts("\x1b[2J\x1b[?1000h");
  fflush(stdout);

  pty_add();

  malt_loop();

  return 0;
}
