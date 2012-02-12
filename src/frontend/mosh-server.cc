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

#include <locale.h>
#include <string.h>
#include <langinfo.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <pwd.h>
#include <typeinfo>
#include <signal.h>
#if defined(__APPLE__)
# include <util.h>
# include <crt_externs.h>
# define environ *_NSGetEnviron()
#else
# include <pty.h>
# include <sys/signalfd.h>
#endif

#include "completeterminal.h"
#include "swrite.h"
#include "user.h"

#include "networktransport.cc"

void serve( int host_fd, const char *desired_ip );

using namespace std;

int main( int argc, char *argv[] )
{
  char *desired_ip = NULL;
  if ( argc == 1 ) {
    desired_ip = NULL;
  } else if ( argc == 2 ) {
    desired_ip = argv[ 1 ];
  } else {
    fprintf( stderr, "Usage: %s [LOCALADDR]\n", argv[ 0 ] );
    exit( 1 );
  }

  int master;
  struct termios child_termios;

  /* Adopt implementation locale */
  if ( NULL == setlocale( LC_ALL, "" ) ) {
    perror( "setlocale" );
    exit( 1 );
  }

  /* Verify locale calls for UTF-8 */
  if ( strcmp( nl_langinfo( CODESET ), "UTF-8" ) != 0 ) {
    fprintf( stderr, "mosh requires a UTF-8 locale.\n" );
    exit( 1 );
  }

  /* Verify terminal configuration */
  if ( tcgetattr( STDIN_FILENO, &child_termios ) < 0 ) {
    perror( "tcgetattr" );
    exit( 1 );
  }

  if ( !(child_termios.c_iflag & IUTF8) ) {
    /* SSH should also convey IUTF8 across connection. */
    //    fprintf( stderr, "Warning: Locale is UTF-8 but termios IUTF8 flag not set. Setting IUTF8 flag.\n" );
    child_termios.c_iflag |= IUTF8;
  }

  /* Fork child process */
  pid_t child = forkpty( &master, NULL, &child_termios, NULL );

  if ( child == -1 ) {
    perror( "forkpty" );
    exit( 1 );
  }

  if ( child == 0 ) {
    /* child */
    if ( setenv( "TERM", "xterm", true ) < 0 ) {
      perror( "setenv" );
      exit( 1 );
    }

    /* ask ncurses to send UTF-8 instead of ISO 2022 for line-drawing chars */
    if ( setenv( "NCURSES_NO_UTF8_ACS", "1", true ) < 0 ) {
      perror( "setenv" );
      exit( 1 );
    }

    /* clear STY environment variable so GNU screen regards us as top level */
    if ( unsetenv( "STY" ) < 0 ) {
      perror( "unsetenv" );
      exit( 1 );
    }

    /* get shell name */
    struct passwd *pw = getpwuid( geteuid() );
    if ( pw == NULL ) {
      perror( "getpwuid" );
      exit( 1 );
    }

    char *my_argv[ 2 ];
    my_argv[ 0 ] = strdup( pw->pw_shell );
    assert( my_argv[ 0 ] );
    my_argv[ 1 ] = NULL;
    
    if ( execve( pw->pw_shell, my_argv, environ ) < 0 ) {
      perror( "execve" );
      exit( 1 );
    }
    exit( 0 );
  } else {
    /* parent */
    try {
      serve( master, desired_ip );
    } catch ( Network::NetworkException e ) {
      fprintf( stderr, "Network exception: %s: %s\n",
	       e.function.c_str(), strerror( e.the_errno ) );
    } catch ( Crypto::CryptoException e ) {
      fprintf( stderr, "Crypto exception: %s\n",
	       e.text.c_str() );
    }

    if ( close( master ) < 0 ) {
      perror( "close" );
      exit( 1 );
    }
  }

  printf( "\n[mosh-server is exiting.]\n" );

  return 0;
}

void serve( int host_fd, const char *desired_ip )
{
  /* establish fd for shutdown signals */
  sigset_t signal_mask;

  assert( sigemptyset( &signal_mask ) == 0 );
  assert( sigaddset( &signal_mask, SIGTERM ) == 0 );
  assert( sigaddset( &signal_mask, SIGINT ) == 0 );

  sigset_t signals_to_block = signal_mask;

  assert( sigaddset( &signals_to_block, SIGHUP ) == 0 );
  assert( sigaddset( &signals_to_block, SIGPIPE ) == 0 );

  /* don't let signals kill us */
  assert( sigprocmask( SIG_BLOCK, &signals_to_block, NULL ) == 0 );

#if !defined(__APPLE__)
  // TODO: Must fix this.
  int shutdown_signal_fd = signalfd( -1, &signal_mask, 0 );
  if ( shutdown_signal_fd < 0 ) {
    perror( "signalfd" );
    return;
  }
#endif

  /* get initial window size */
  struct winsize window_size;
  if ( ioctl( STDIN_FILENO, TIOCGWINSZ, &window_size ) < 0 ) {
    perror( "ioctl TIOCGWINSZ" );
    return;
  }

  /* tell child process */
  if ( ioctl( host_fd, TIOCSWINSZ, &window_size ) < 0 ) {
    perror( "ioctl TIOCSWINSZ" );
    return;
  }

  /* open parser and terminal */
  Terminal::Complete terminal( window_size.ws_col, window_size.ws_row );

  /* open network */
  Network::UserStream blank;
  Network::Transport< Terminal::Complete, Network::UserStream > network( terminal, blank, desired_ip );

  /* network.set_verbose(); */

  printf( "MOSH CONNECT %d %s\n", network.port(), network.get_key().c_str() );
  fflush( stdout );

  /* detach from terminal */
  pid_t the_pid = fork();
  if ( the_pid < 0 ) {
    perror( "fork" );
  } else if ( the_pid > 0 ) {
    _exit( 0 );
  }

  fprintf( stderr, "[mosh-server detached, pid=%d.]\n", (int)getpid() );

  /* prepare to poll for events */
  struct pollfd pollfds[ 3 ];

  pollfds[ 0 ].fd = network.fd();
  pollfds[ 0 ].events = POLLIN;

  pollfds[ 1 ].fd = host_fd;
  pollfds[ 1 ].events = POLLIN;

#if !defined(__APPLE__)
  // TODO: Must fix this.
  pollfds[ 2 ].fd = shutdown_signal_fd;
  pollfds[ 2 ].events = POLLIN;
#endif

  uint64_t last_remote_num = network.get_remote_state_num();

  while ( 1 ) {
    try {
      int active_fds = poll( pollfds, 3, network.wait_time() );
      if ( active_fds < 0 ) {
	perror( "poll" );
	break;
      }

      if ( pollfds[ 0 ].revents & POLLIN ) {
	/* packet received from the network */
	network.recv();
	
	/* is new user input available for the terminal? */
	if ( network.get_remote_state_num() != last_remote_num ) {
	  string terminal_to_host;
	  
	  Network::UserStream us;
	  us.apply_string( network.get_remote_diff() );
	  /* apply userstream to terminal */
	  for ( size_t i = 0; i < us.size(); i++ ) {
	    terminal_to_host += terminal.act( us.get_action( i ) );
	    if ( typeid( *us.get_action( i ) ) == typeid( Parser::Resize ) ) {
	      /* tell child process of resize */
	      const Parser::Resize *res = static_cast<const Parser::Resize *>( us.get_action( i ) );
	      window_size.ws_col = res->width;
	      window_size.ws_row = res->height;
	      if ( ioctl( host_fd, TIOCSWINSZ, &window_size ) < 0 ) {
		perror( "ioctl TIOCSWINSZ" );
		return;
	      }
	    }
	  }

	  /* update client with new state of terminal */
	  if ( !network.shutdown_in_progress() ) {
	    network.set_current_state( terminal );
	  }
	  
	  /* write any writeback octets back to the host */
	  if ( swrite( host_fd, terminal_to_host.c_str(), terminal_to_host.length() ) < 0 ) {
	    break;
	  }
	}
      }
      
      if ( pollfds[ 1 ].revents & POLLIN ) {
	/* input from the host needs to be fed to the terminal */
	const int buf_size = 16384;
	char buf[ buf_size ];
	
	/* fill buffer if possible */
	ssize_t bytes_read = read( pollfds[ 1 ].fd, buf, buf_size );
	if ( bytes_read == 0 ) { /* EOF */
	  return;
	} else if ( bytes_read < 0 ) {
	  perror( "read" );
	  return;
	}
	
	string terminal_to_host = terminal.act( string( buf, bytes_read ) );
	
	/* update client with new state of terminal */
	if ( !network.shutdown_in_progress() ) {
	  network.set_current_state( terminal );
	}

	/* write any writeback octets back to the host */
	if ( swrite( host_fd, terminal_to_host.c_str(), terminal_to_host.length() ) < 0 ) {
	  break;
	}
      }

#if !defined(__APPLE__)
  // TODO: Must fix this.
      if ( pollfds[ 2 ].revents & POLLIN ) {
	/* shutdown signal */
	struct signalfd_siginfo the_siginfo;
	ssize_t bytes_read = read( pollfds[ 2 ].fd, &the_siginfo, sizeof( the_siginfo ) );
	if ( bytes_read == 0 ) {
	  break;
	} else if ( bytes_read < 0 ) {
	  perror( "read" );
	  break;
	}

	if ( network.attached() && (!network.shutdown_in_progress()) ) {
	  network.start_shutdown();
	} else {
	  break;
	}
      }
#endif
      
      if ( (pollfds[ 0 ].revents)
	   & (POLLERR | POLLHUP | POLLNVAL) ) {
	/* network problem */
	break;
      }

      if ( (pollfds[ 1 ].revents)
	   & (POLLERR | POLLHUP | POLLNVAL) ) {
	/* host problem */
	if ( network.attached() ) {
	  network.start_shutdown();
	} else {
	  break;
	}
      }

      /* quit if our shutdown has been acknowledged */
      if ( network.shutdown_in_progress() && network.shutdown_acknowledged() ) {
	break;
      }

      /* quit after shutdown acknowledgement timeout */
      if ( network.shutdown_in_progress() && network.shutdown_ack_timed_out() ) {
	break;
      }

      /* quit if we received and acknowledged a shutdown request */
      if ( network.counterparty_shutdown_ack_sent() ) {
	break;
      }

      network.tick();
    } catch ( Network::NetworkException e ) {
      fprintf( stderr, "%s: %s\n", e.function.c_str(), strerror( e.the_errno ) );
      sleep( 1 );
    } catch ( Crypto::CryptoException e ) {
      fprintf( stderr, "Crypto exception: %s\n", e.text.c_str() );
    }
  }
}
