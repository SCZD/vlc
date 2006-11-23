/*****************************************************************************
 * ipv4.c: IPv4 network abstraction layer
 *****************************************************************************
 * Copyright (C) 2001-2006 the VideoLAN team
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *          Mathias Kretschmer <mathias@research.att.com>
 *          Alexis de Lattre <alexis@via.ecp.fr>
 *          Rémi Denis-Courmont <rem # videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>
#include <string.h>

#include <vlc/vlc.h>
#include <errno.h>

#ifdef HAVE_SYS_TYPES_H
#   include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#   include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#   include <fcntl.h>
#endif

#ifdef HAVE_UNISTD_H
#   include <unistd.h>
#endif

#if defined(WIN32) || defined(UNDER_CE)
#   if defined(UNDER_CE) && defined(sockaddr_storage)
#       undef sockaddr_storage
#   endif
#   include <winsock2.h>
#   include <ws2tcpip.h>
#   include <iphlpapi.h>
#   define close closesocket
#   if defined(UNDER_CE)
#       undef IP_MULTICAST_TTL
#       define IP_MULTICAST_TTL 3
#       undef IP_ADD_MEMBERSHIP
#       define IP_ADD_MEMBERSHIP 5
#   endif
#else
#   include <netdb.h>                                         /* hostent ... */
#   include <sys/socket.h>
#   include <netinet/in.h>
#   ifdef HAVE_ARPA_INET_H
#       include <arpa/inet.h>                    /* inet_ntoa(), inet_aton() */
#   endif
#endif

#include "network.h"

#ifndef INADDR_ANY
#   define INADDR_ANY  0x00000000
#endif
#ifndef INADDR_NONE
#   define INADDR_NONE 0xFFFFFFFF
#endif
#ifndef IN_MULTICAST
#   define IN_MULTICAST(a) IN_CLASSD(a)
#endif


/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int OpenUDP( vlc_object_t * );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
vlc_module_begin();
    set_shortname( "IPv4" );
    set_description( _("UDP/IPv4 network abstraction layer") );
    set_capability( "network", 50 );
    set_category( CAT_INPUT );
    set_subcategory( SUBCAT_INPUT_GENERAL );
    set_callbacks( OpenUDP, NULL );
vlc_module_end();

/*****************************************************************************
 * BuildAddr: utility function to build a struct sockaddr_in
 *****************************************************************************/
static int BuildAddr( vlc_object_t *p_obj, struct sockaddr_in * p_socket,
                      const char * psz_address, int i_port )
{
    struct addrinfo hints, *res;
    int i_val;

    memset( &hints, 0, sizeof( hints ) );
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    msg_Dbg( p_obj, "resolving %s:%d...", psz_address, i_port );
    i_val = vlc_getaddrinfo( p_obj, psz_address, i_port, &hints, &res );
    if( i_val )
    {
        msg_Warn( p_obj, "%s: %s", psz_address, vlc_gai_strerror( i_val ) );
        return -1;
    }

    /* Copy the first address of the host in the socket address */
    memcpy( p_socket, res->ai_addr, sizeof( *p_socket ) );
    vlc_freeaddrinfo( res );

    return( 0 );
}

#if defined(WIN32) || defined(UNDER_CE)
# define WINSOCK_STRERROR_SIZE 20
static const char *winsock_strerror( char *buf )
{
    snprintf( buf, WINSOCK_STRERROR_SIZE, "Winsock error %d",
              WSAGetLastError( ) );
    buf[WINSOCK_STRERROR_SIZE - 1] = '\0';
    return buf;
}
#endif


/*****************************************************************************
 * OpenUDP: open a UDP socket
 *****************************************************************************
 * psz_bind_addr, i_bind_port : address and port used for the bind()
 *   system call. If psz_bind_addr == "", the socket is bound to
 *   INADDR_ANY and broadcast reception is enabled. If psz_bind_addr is a
 *   multicast (class D) address, join the multicast group.
 * psz_server_addr, i_server_port : address and port used for the connect()
 *   system call. It can avoid receiving packets from unauthorized IPs.
 *   Its use leads to great confusion and is currently discouraged.
 * This function returns -1 in case of error.
 *****************************************************************************/
static int OpenUDP( vlc_object_t * p_this )
{
    network_socket_t * p_socket = p_this->p_private;
    const char * psz_bind_addr = p_socket->psz_bind_addr;
    int i_bind_port = p_socket->i_bind_port;
    const char * psz_server_addr = p_socket->psz_server_addr;
    int i_server_port = p_socket->i_server_port;

    int i_handle, i_opt;
    struct sockaddr_in sock;
    vlc_value_t val;
#if defined(WIN32) || defined(UNDER_CE)
    char strerror_buf[WINSOCK_STRERROR_SIZE];
# define strerror( x ) winsock_strerror( strerror_buf )
#endif

    p_socket->i_handle = -1;

    /* Build the local socket */
    if( BuildAddr( p_this, &sock, psz_bind_addr, i_bind_port ) == -1 )
        return VLC_EGENERIC;

    /* Open a SOCK_DGRAM (UDP) socket, in the AF_INET domain, automatic (0)
     * protocol */
    if( (i_handle = socket( AF_INET, SOCK_DGRAM, 0 )) == -1 )
    {
        msg_Err( p_this, "cannot create socket (%s)", strerror(errno) );
        return VLC_EGENERIC;
    }

    /* We may want to reuse an already used socket */
    i_opt = 1;
    setsockopt( i_handle, SOL_SOCKET, SO_REUSEADDR, (void *) &i_opt,
                    sizeof( i_opt ) );
#ifdef SO_REUSEPORT
    i_opt = 1;
    setsockopt( i_handle, SOL_SOCKET, SO_REUSEPORT, (void *) &i_opt,
                    sizeof( i_opt ) );
#endif

    /* Increase the receive buffer size to 1/2MB (8Mb/s during 1/2s) to avoid
     * packet loss caused by scheduling problems */
#ifdef SO_RCVBUF
    i_opt = 0x80000;
    if( setsockopt( i_handle, SOL_SOCKET, SO_RCVBUF, (void *) &i_opt,
                    sizeof( i_opt ) ) == -1 )
        msg_Dbg( p_this, "cannot configure socket (SO_RCVBUF: %s)",
                          strerror(errno));
    i_opt = 0x80000;
    if( setsockopt( i_handle, SOL_SOCKET, SO_SNDBUF, (void *) &i_opt,
                    sizeof( i_opt ) ) == -1 )
        msg_Dbg( p_this, "cannot configure socket (SO_SNDBUF: %s)",
                          strerror(errno));
#endif

#if defined( WIN32 ) || defined( UNDER_CE )
    /*
     * Under Win32 and for multicasting, we bind to INADDR_ANY.
     * This is of course a severe bug, since the socket would logically
     * receive unicast traffic, and multicast traffic of groups subscribed
     * to via other sockets. How this actually works in Winsock, I don't
     * know.
     */
    if( IN_MULTICAST( ntohl( sock.sin_addr.s_addr ) ) )
    {
        struct sockaddr_in stupid = sock;
        stupid.sin_addr.s_addr = INADDR_ANY;

        if( bind( i_handle, (struct sockaddr *)&stupid, sizeof( stupid ) ) < 0 )
        {
            msg_Err( p_this, "cannot bind socket (%d)", WSAGetLastError() );
            goto error;
        }
    }
    else
#endif
    /* Bind it */
    if( bind( i_handle, (struct sockaddr *)&sock, sizeof( sock ) ) < 0 )
    {
        msg_Err( p_this, "cannot bind socket (%s)", strerror(errno) );
        goto error;
    }

#if !defined( SYS_BEOS )
    /* Allow broadcast reception if we bound on INADDR_ANY */
    if( !*psz_bind_addr )
    {
        i_opt = 1;
        if( setsockopt( i_handle, SOL_SOCKET, SO_BROADCAST, (void*) &i_opt,
                        sizeof( i_opt ) ) == -1 )
            msg_Warn( p_this, "cannot configure socket (SO_BROADCAST: %s)",
                       strerror(errno) );
    }
#endif

#ifdef IP_ADD_SOURCE_MEMBERSHIP
    /* Join the multicast group if the socket is a multicast address */
    if( IN_MULTICAST( ntohl(sock.sin_addr.s_addr) ) )
    {
        /* Determine interface to be used for multicast */
        char * psz_if_addr = config_GetPsz( p_this, "miface-addr" );

        /* If we have a source address, we use IP_ADD_SOURCE_MEMBERSHIP
           so that IGMPv3 aware OSes running on IGMPv3 aware networks
           will do an IGMPv3 query on the network */
        struct ip_mreq_source imr =
        {
            .imr_multiaddr.s_addr = sock.sin_addr.s_addr,
            .imr_sourceaddr.s_addr = inet_addr(psz_server_addr)
        };

        if( psz_if_addr != NULL && *psz_if_addr
            && inet_addr(psz_if_addr) != INADDR_NONE )
            imr.imr_interface.s_addr = inet_addr(psz_if_addr);
        else
            imr.imr_interface.s_addr = INADDR_ANY;

        if( psz_if_addr != NULL )
            free( psz_if_addr );

        msg_Dbg( p_this, "IP_ADD_SOURCE_MEMBERSHIP multicast request" );

        /* Join Multicast group with source filter */
        if( setsockopt( i_handle, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP,
                        (void*)&imr,
                        sizeof(struct ip_mreq_source) ) == -1 )
        {
            msg_Err( p_this, "Source specific multicast failed (%s) -"
                              "check if your OS really supports IGMPv3",
                      strerror(errno) );
            goto error;
        }
    }
    else
#endif
    {
        /* Build socket for remote connection */
        if ( BuildAddr( p_this, &sock, psz_server_addr, i_server_port ) == -1 )
        {
            msg_Err( p_this, "cannot build remote address" );
            goto error;
        }

        /* Connect the socket */
        if( connect( i_handle, (struct sockaddr *) &sock,
                     sizeof( sock ) ) == (-1) )
        {
            msg_Err( p_this, "cannot connect socket (%s)", strerror(errno) );
            goto error;
        }

#ifdef IP_MULTICAST_IF
        if( IN_MULTICAST( ntohl(inet_addr(psz_server_addr) ) ) )
        {
            /* set the time-to-live */
            int i_ttl = p_socket->i_ttl;

            /* set the multicast interface */
            char * psz_mif_addr = config_GetPsz( p_this, "miface-addr" );
            if( psz_mif_addr )
            {
                struct in_addr intf;
                intf.s_addr = inet_addr(psz_mif_addr);
                free( psz_mif_addr  );

                if( setsockopt( i_handle, IPPROTO_IP, IP_MULTICAST_IF,
                                &intf, sizeof( intf ) ) < 0 )
                {
                    msg_Err( p_this, "failed to set multicast interface (%s).", strerror(errno) );
                    goto error;
                }
            }

            if( i_ttl <= 0 )
                i_ttl = config_GetInt( p_this, "ttl" );

            if( i_ttl > 0 )
            {
                unsigned char ttl = (unsigned char) i_ttl;

                /* There is some confusion in the world whether IP_MULTICAST_TTL 
                * takes a byte or an int as an argument.
                * BSD seems to indicate byte so we are going with that and use
                * int as a fallback to be safe */
                if( setsockopt( i_handle, IPPROTO_IP, IP_MULTICAST_TTL,
                                &ttl, sizeof( ttl ) ) < 0 )
                {
                    msg_Dbg( p_this, "failed to set ttl (%s). Let's try it "
                            "the integer way.", strerror(errno) );
                    if( setsockopt( i_handle, IPPROTO_IP, IP_MULTICAST_TTL,
                                    &i_ttl, sizeof( i_ttl ) ) <0 )
                    {
                        msg_Err( p_this, "failed to set ttl (%s)",
                                strerror(errno) );
                        goto error;
                    }
                }
            }
        }
#endif
    }

    p_socket->i_handle = i_handle;

    if( var_Get( p_this, "mtu", &val ) != VLC_SUCCESS )
    {
        var_Create( p_this, "mtu", VLC_VAR_INTEGER | VLC_VAR_DOINHERIT );
        var_Get( p_this, "mtu", &val );
    }
    p_socket->i_mtu = val.i_int;

    return 0;

error:
    close (i_handle);
    return VLC_EGENERIC;
}
