/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <locale.h>
#include <langinfo.h>
#include <wchar.h>
#include <assert.h>
#include <wctype.h>
#include <iostream>
#include <typeinfo>

#if defined(__APPLE__)
# include <util.h>
#else
# include <pty.h>
#endif

#include "parser.h"
#include "swrite.h"

const size_t buf_size = 1024;

void emulate_terminal( int fd );
int copy( int src, int dest );
int vt_parser( int fd, Parser::UTF8Parser *parser );

int main( int argc __attribute__((unused)),
	  char *argv[] __attribute__((unused)),
	  char *envp[] )
{
  int master;
  struct termios saved_termios, raw_termios, child_termios;

  if ( NULL == setlocale( LC_ALL, "" ) ) {
    perror( "setlocale" );
    exit( 1 );
  }

  if ( strcmp( nl_langinfo( CODESET ), "UTF-8" ) != 0 ) {
    fprintf( stderr, "stm requires a UTF-8 locale.\n" );
    exit( 1 );
  }

  if ( tcgetattr( STDIN_FILENO, &saved_termios ) < 0 ) {
    perror( "tcgetattr" );
    exit( 1 );
  }

  child_termios = saved_termios;

  if ( !(child_termios.c_iflag & IUTF8) ) {
    fprintf( stderr, "Warning: Locale is UTF-8 but termios IUTF8 flag not set. Setting IUTF8 flag.\n" );
    child_termios.c_iflag |= IUTF8;
  }

  pid_t child = forkpty( &master, NULL, &child_termios, NULL );

  if ( child == -1 ) {
    perror( "forkpty" );
    exit( 1 );
  }

  if ( child == 0 ) {
    /* child */
    char *my_argv[ 2 ];
    my_argv[ 0 ] = strdup( "/bin/bash" );
    assert( my_argv[ 0 ] );

    my_argv[ 1 ] = NULL;

    if ( execve( "/bin/bash", my_argv, envp ) < 0 ) {
      perror( "execve" );
      exit( 1 );
    }
    exit( 0 );
  } else {
    /* parent */
    raw_termios = saved_termios;

    cfmakeraw( &raw_termios );

    if ( tcsetattr( STDIN_FILENO, TCSANOW, &raw_termios ) < 0 ) {
      perror( "tcsetattr" );
      exit( 1 );
    }

    emulate_terminal( master );

    if ( tcsetattr( STDIN_FILENO, TCSANOW, &saved_termios ) < 0 ) {
      perror( "tcsetattr" );
      exit( 1 );
    }
  }

  return 0;
}

void emulate_terminal( int fd )
{
  Parser::UTF8Parser parser;
  struct pollfd pollfds[ 2 ];

  pollfds[ 0 ].fd = STDIN_FILENO;
  pollfds[ 0 ].events = POLLIN;

  pollfds[ 1 ].fd = fd;
  pollfds[ 1 ].events = POLLIN;

  while ( 1 ) {
    int active_fds = poll( pollfds, 2, -1 );
    if ( active_fds <= 0 ) {
      perror( "poll" );
      return;
    }

    if ( pollfds[ 0 ].revents & POLLIN ) {
      if ( copy( STDIN_FILENO, fd ) < 0 ) {
	return;
      }
    } else if ( pollfds[ 1 ].revents & POLLIN ) {
      if ( vt_parser( fd, &parser ) < 0 ) {
	return;
      }
    } else if ( (pollfds[ 0 ].revents | pollfds[ 1 ].revents)
		& (POLLERR | POLLHUP | POLLNVAL) ) {
      return;
    } else {
      fprintf( stderr, "poll mysteriously woken up\n" );
    }
  }
}

int copy( int src, int dest )
{
  char buf[ buf_size ];

  ssize_t bytes_read = read( src, buf, buf_size );
  if ( bytes_read == 0 ) { /* EOF */
    return -1;
  } else if ( bytes_read < 0 ) {
    perror( "read" );
    return -1;
  }

  return swrite( dest, buf, bytes_read );
}

int vt_parser( int fd, Parser::UTF8Parser *parser )
{
  char buf[ buf_size ];

  /* fill buffer if possible */
  ssize_t bytes_read = read( fd, buf, buf_size );
  if ( bytes_read == 0 ) { /* EOF */
    return -1;
  } else if ( bytes_read < 0 ) {
    perror( "read" );
    return -1;
  }

  /* feed to parser */
  for ( int i = 0; i < bytes_read; i++ ) {
    std::list<Parser::Action *> actions = parser->input( buf[ i ] );
    for ( std::list<Parser::Action *>::iterator j = actions.begin();
	  j != actions.end();
	  j++ ) {

      Parser::Action *act = *j;
      assert( act );

      if ( act->char_present ) {
	if ( iswprint( act->ch ) ) {
	  printf( "%s(0x%02x=%lc) ", act->name().c_str(), act->ch, act->ch );
	} else {
	  printf( "%s(0x%02x) ", act->name().c_str(), act->ch );
	}
      } else {
	printf( "[%s] ", act->name().c_str() );
      }

      delete act;

      fflush( stdout );
    }
  }

  return 0;
}
