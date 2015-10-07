/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2015  ZeroTier, Inc.
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * ZeroTier may be used and distributed under the terms of the GPLv3, which
 * are available at: http://www.gnu.org/licenses/gpl-3.0.html
 *
 * If you would like to embed ZeroTier into a commercial application or
 * redistribute it in a modified binary form, please contact ZeroTier Networks
 * LLC. Start here: http://www.zerotier.com/
 */

#ifdef ZT_ENABLE_NETCON

#include <algorithm>
#include <utility>
#include <dlfcn.h>
//#include <sys/types.h>

#include "NetconEthernetTap.hpp"

#include "../node/Utils.hpp"
#include "../osdep/OSUtils.hpp"
#include "../osdep/Phy.hpp"

#include "lwip/tcp_impl.h"
#include "netif/etharp.h"
#include "lwip/ip.h"
#include "lwip/ip_addr.h"
#include "lwip/ip_frag.h"
#include "lwip/tcp.h"

#include "LWIPStack.hpp"
#include "NetconService.hpp"
#include "Intercept.h"
#include "NetconUtilities.hpp"

#define APPLICATION_POLL_FREQ 1

namespace ZeroTier {


NetconEthernetTap::NetconEthernetTap(
	const char *homePath,
	const MAC &mac,
	unsigned int mtu,
	unsigned int metric,
	uint64_t nwid,
	const char *friendlyName,
	void (*handler)(void *,uint64_t,const MAC &,const MAC &,unsigned int,unsigned int,const void *,unsigned int),
	void *arg) :
	_phy(this,false,true),
	_unixListenSocket((PhySocket *)0),
	_handler(handler),
	_arg(arg),
	_nwid(nwid),
	_mac(mac),
	_homePath(homePath),
	_mtu(mtu),
	_enabled(true),
	_run(true)
{
	char sockPath[4096];
	Utils::snprintf(sockPath,sizeof(sockPath),"/tmp/.ztnc_%.16llx",(unsigned long long)nwid);
	_dev = sockPath;

	lwipstack = new LWIPStack("ext/bin/lwip/liblwip.so"); // ext/bin/liblwip.so.debug for debug symbols
	if(!lwipstack) // TODO double check this check
		throw std::runtime_error("unable to load lwip lib.");
	lwipstack->lwip_init();

	_unixListenSocket = _phy.unixListen(sockPath,(void *)this);
	if (!_unixListenSocket)
		throw std::runtime_error(std::string("unable to bind to ")+sockPath);
	_thread = Thread::start(this);
}

NetconEthernetTap::~NetconEthernetTap()
{
	_run = false;
	_phy.whack();
	_phy.whack();
	Thread::join(_thread);
	_phy.close(_unixListenSocket,false);
	delete lwipstack;
}

void NetconEthernetTap::setEnabled(bool en)
{
	_enabled = en;
}

bool NetconEthernetTap::enabled() const
{
	return _enabled;
}

bool NetconEthernetTap::addIp(const InetAddress &ip)
{
	Mutex::Lock _l(_ips_m);
	if (std::find(_ips.begin(),_ips.end(),ip) == _ips.end()) {
		_ips.push_back(ip);
		std::sort(_ips.begin(),_ips.end());

		if (ip.isV4()) {
			// Set IP
			static ip_addr_t ipaddr, netmask, gw;
			IP4_ADDR(&gw,192,168,0,1);
			ipaddr.addr = *((u32_t *)ip.rawIpData());
			netmask.addr = *((u32_t *)ip.netmask().rawIpData());

			// Set up the lwip-netif for LWIP's sake
			lwipstack->netif_add(&interface,&ipaddr, &netmask, &gw, NULL, tapif_init, lwipstack->_ethernet_input);
			interface.state = this;
			interface.output = lwipstack->_etharp_output;
			_mac.copyTo(interface.hwaddr, 6);
			interface.mtu = _mtu;
			interface.name[0] = 't';
			interface.name[1] = 'p';
			interface.linkoutput = low_level_output;
			interface.hwaddr_len = 6;
			interface.flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP;
			lwipstack->netif_set_default(&interface);
			lwipstack->netif_set_up(&interface);
		}
	}
	return true;
}

bool NetconEthernetTap::removeIp(const InetAddress &ip)
{
	Mutex::Lock _l(_ips_m);
	std::vector<InetAddress>::iterator i(std::find(_ips.begin(),_ips.end(),ip));
	if (i == _ips.end())
		return false;

	_ips.erase(i);

	if (ip.isV4()) {
		// TODO: dealloc from LWIP
	}

	return true;
}

std::vector<InetAddress> NetconEthernetTap::ips() const
{
	Mutex::Lock _l(_ips_m);
	return _ips;
}

void NetconEthernetTap::put(const MAC &from,const MAC &to,unsigned int etherType,const void *data,unsigned int len)
{
	struct pbuf *p,*q;
	//fprintf(stderr, "_put(%s,%s,%.4x,[data],%u)\n",from.toString().c_str(),to.toString().c_str(),etherType,len);
	if (!_enabled)
		return;

	//printf(">> %.4x %s\n",etherType,Utils::hex(data,len).c_str());
	struct eth_hdr ethhdr;
	from.copyTo(ethhdr.src.addr, 6);
	to.copyTo(ethhdr.dest.addr, 6);
	ethhdr.type = Utils::hton((uint16_t)etherType);

	// We allocate a pbuf chain of pbufs from the pool.
	p = lwipstack->pbuf_alloc(PBUF_RAW, len+sizeof(struct eth_hdr), PBUF_POOL);

	if (p != NULL) {
		const char *dataptr = reinterpret_cast<const char *>(data);

		// First pbuf gets ethernet header at start
		q = p;
		if (q->len < sizeof(ethhdr)) {
			fprintf(stderr,"_put(): Dropped packet: first pbuf smaller than ethernet header\n");
			return;
		}
		memcpy(q->payload,&ethhdr,sizeof(ethhdr));
		memcpy(q->payload + sizeof(ethhdr),dataptr,q->len - sizeof(ethhdr));
		dataptr += q->len - sizeof(ethhdr);

		// Remaining pbufs (if any) get rest of data
		while ((q = q->next)) {
			memcpy(q->payload,dataptr,q->len);
			dataptr += q->len;
		}
	} else {
		fprintf(stderr, "_put(): Dropped packet: no pbufs available\n");
		return;
	}

	//printf("p->len == %u, p->payload == %s\n",p->len,Utils::hex(p->payload,p->len).c_str());
	{
		Mutex::Lock _l2(lwipstack->_lock);
		if(interface.input(p, &interface) != ERR_OK) {
			fprintf(stderr, "_put(): Error while RXing packet (netif->input)\n");
		}
	}
}

std::string NetconEthernetTap::deviceName() const
{
	return _dev;
}

void NetconEthernetTap::setFriendlyName(const char *friendlyName)
{
}

void NetconEthernetTap::scanMulticastGroups(std::vector<MulticastGroup> &added,std::vector<MulticastGroup> &removed)
{
	std::vector<MulticastGroup> newGroups;
	Mutex::Lock _l(_multicastGroups_m);

	// TODO: get multicast subscriptions from LWIP

	std::vector<InetAddress> allIps(ips());
	for(std::vector<InetAddress>::iterator ip(allIps.begin());ip!=allIps.end();++ip)
		newGroups.push_back(MulticastGroup::deriveMulticastGroupForAddressResolution(*ip));

	std::sort(newGroups.begin(),newGroups.end());
	std::unique(newGroups.begin(),newGroups.end());

	for(std::vector<MulticastGroup>::iterator m(newGroups.begin());m!=newGroups.end();++m) {
		if (!std::binary_search(_multicastGroups.begin(),_multicastGroups.end(),*m))
			added.push_back(*m);
	}
	for(std::vector<MulticastGroup>::iterator m(_multicastGroups.begin());m!=_multicastGroups.end();++m) {
		if (!std::binary_search(newGroups.begin(),newGroups.end(),*m))
			removed.push_back(*m);
	}

	_multicastGroups.swap(newGroups);
}

TcpConnection *NetconEthernetTap::getConnectionByPCB(struct tcp_pcb *pcb)
{
	for(size_t i=0; i<tcp_connections.size(); i++) {
		if(tcp_connections[i]->pcb == pcb)
			return tcp_connections[i];
	}
	return NULL;
}

TcpConnection *NetconEthernetTap::getConnectionByTheirFD(int fd)
{
	for(size_t i=0; i<tcp_connections.size(); i++) {
		if(tcp_connections[i]->perceived_fd == fd)
			return tcp_connections[i];
	}
	return NULL;
}

/*
 * Closes a TcpConnection and associated LWIP PCB strcuture.
 */
void NetconEthernetTap::closeConnection(TcpConnection *conn)
{
	//fprintf(stderr, "closeConnection(): closing: conn->type = %d, fd=%d\n", conn->type, _phy.getDescriptor(conn->sock));
	lwipstack->_tcp_arg(conn->pcb, NULL);
  lwipstack->_tcp_sent(conn->pcb, NULL);
  lwipstack->_tcp_recv(conn->pcb, NULL);
  lwipstack->_tcp_err(conn->pcb, NULL);
  lwipstack->_tcp_poll(conn->pcb, NULL, 0);
	lwipstack->_tcp_close(conn->pcb);
	close(_phy.getDescriptor(conn->dataSock));
	close(conn->their_fd);
	_phy.close(conn->dataSock);

	for(int i=0; i<tcp_connections.size(); i++) {
		if(tcp_connections[i] == conn) {
			tcp_connections.erase(tcp_connections.begin() + i);
		}
	}
	delete conn;
}

void NetconEthernetTap::closeClient(PhySocket *sock)
{
	for(int i=0; i<rpc_sockets.size(); i++) {
		if(rpc_sockets[i] == sock)
			rpc_sockets.erase(rpc_sockets.begin() + i);
	}
	close(_phy.getDescriptor(sock));
  _phy.close(sock);
}

void NetconEthernetTap::closeAll()
{
	while(rpc_sockets.size())
		closeClient(rpc_sockets.front());
	while(tcp_connections.size())
		closeConnection(tcp_connections.front());
}

#define ZT_LWIP_TCP_TIMER_INTERVAL 5

void NetconEthernetTap::threadMain()
	throw()
{
	fprintf(stderr, "_threadMain()\n");
	uint64_t prev_tcp_time = 0;
	uint64_t prev_etharp_time = 0;

	fprintf(stderr, "- MEM_SIZE = %dM\n", MEM_SIZE / (1024*1024));
	fprintf(stderr, "- TCP_SND_BUF = %dK\n", TCP_SND_BUF / 1024);
	fprintf(stderr, "- MEMP_NUM_PBUF = %d\n", MEMP_NUM_PBUF);
	fprintf(stderr, "- MEMP_NUM_TCP_PCB = %d\n", MEMP_NUM_TCP_PCB);
	fprintf(stderr, "- MEMP_NUM_TCP_PCB_LISTEN = %d\n", MEMP_NUM_TCP_PCB_LISTEN);
	fprintf(stderr, "- MEMP_NUM_TCP_SEG = %d\n", MEMP_NUM_TCP_SEG);
	fprintf(stderr, "- PBUF_POOL_SIZE = %d\n", PBUF_POOL_SIZE);
	fprintf(stderr, "- TCP_SND_QUEUELEN = %d\n", TCP_SND_QUEUELEN);
	fprintf(stderr, "- IP_REASSEMBLY = %d\n", IP_REASSEMBLY);
	fprintf(stderr, "- TCP_WND = %d\n", TCP_WND);
	fprintf(stderr, "- TCP_MSS = %d\n", TCP_MSS);
	fprintf(stderr, "- ARP_TMR_INTERVAL = %d\n", ARP_TMR_INTERVAL);
	fprintf(stderr, "- TCP_TMR_INTERVAL = %d\n", TCP_TMR_INTERVAL);
	fprintf(stderr, "- IP_TMR_INTERVAL  = %d\n", IP_TMR_INTERVAL);

	// Main timer loop
	while (_run) {
		uint64_t now = OSUtils::now();

		uint64_t since_tcp = now - prev_tcp_time;
		uint64_t since_etharp = now - prev_etharp_time;

		uint64_t tcp_remaining = ZT_LWIP_TCP_TIMER_INTERVAL;
		uint64_t etharp_remaining = ARP_TMR_INTERVAL;

		if (since_tcp >= ZT_LWIP_TCP_TIMER_INTERVAL) {
			prev_tcp_time = now;
			lwipstack->tcp_tmr();
		} else {
			tcp_remaining = ZT_LWIP_TCP_TIMER_INTERVAL - since_tcp;
		}
		if (since_etharp >= ARP_TMR_INTERVAL) {
			prev_etharp_time = now;
			lwipstack->etharp_tmr();
		} else {
			etharp_remaining = ARP_TMR_INTERVAL - since_etharp;
		}
		_phy.poll((unsigned long)std::min(tcp_remaining,etharp_remaining));
	}
	closeAll();
	// TODO: cleanup -- destroy LWIP state, kill any clients, unload .so, etc.
}

void NetconEthernetTap::phyOnUnixClose(PhySocket *sock,void **uptr)
{
	//fprintf(stderr, "phyOnUnixClose() CLOSING: %d\n", _phy.getDescriptor(sock));
	//closeClient(sock);
	// FIXME:
}

/*
 * Handles data on a client's data buffer. Data is sent to LWIP to be enqueued.
 */
void NetconEthernetTap::phyOnFileDescriptorActivity(PhySocket *sock,void **uptr,bool readable,bool writable)
{
	if(readable) {
		float max = (float)TCP_SND_BUF;
		int r;
		TcpConnection *conn = (TcpConnection*)*uptr;

		if(!conn) {
			fprintf(stderr, "phyOnFileDescriptorActivity(): could not locate connection for this fd\n");
			return;
		}
		if(conn->idx < max) {
			Mutex::Lock _l(lwipstack->_lock);
			int sndbuf = conn->pcb->snd_buf; // How much we are currently allowed to write to the connection

			/*
			float avail = (float)sndbuf;
			float load = 1.0 - (avail / max);

			if(load >= 0.80) {
				fprintf(stderr, "load too high\n");
				return;
			}
			*/

			/* PCB send buffer is full,turn off readability notifications for the
			corresponding PhySocket until nc_sent() is called and confirms that there is
			now space on the buffer */
			if(sndbuf == 0) {
				_phy.setNotifyReadable(sock, false);
				return;
			}

			int read_fd = _phy.getDescriptor(sock);

			if((r = read(read_fd, (&conn->buf)+conn->idx, sndbuf)) > 0) {
				conn->idx += r;
				/* Writes data pulled from the client's socket buffer to LWIP. This merely sends the
				 * data to LWIP to be enqueued and eventually sent to the network. */
				if(r > 0) {
					int sz;
					// NOTE: this assumes that lwipstack->_lock is locked, either
					// because we are in a callback or have locked it manually.
					//fprintf(stderr, "phyOnFileDescriptorActivity(): Can read %d bytes, did read %d bytes\n", sndbuf, r);
					int err = lwipstack->_tcp_write(conn->pcb, &conn->buf, r, TCP_WRITE_FLAG_COPY);
					if(err != ERR_OK) {
						fprintf(stderr, "phyOnFileDescriptorActivity(): error while writing to PCB\n");
						return;
					}
					else {
						sz = (conn->idx)-r;
						if(sz) {
							memmove(&conn->buf, (conn->buf+r), sz);
						}
						conn->idx -= r;
						return;
					}
				}
				else {
					fprintf(stderr, "phyOnFileDescriptorActivity(): LWIP stack full\n");
					return;
				}
			}
			else {
				fprintf(stderr, "phyOnFileDescriptorActivity(): could not read from PhySocket for this connection\n");
			}
		}
	}
	else {
		fprintf(stderr, "phyOnFileDescriptorActivity(): PhySocket not readable\n");
	}
}

// Unused -- no UDP or TCP from this thread/Phy<>
void NetconEthernetTap::phyOnDatagram(PhySocket *sock,void **uptr,const struct sockaddr *from,void *data,unsigned long len) {}
void NetconEthernetTap::phyOnTcpConnect(PhySocket *sock,void **uptr,bool success) {}
void NetconEthernetTap::phyOnTcpAccept(PhySocket *sockL,PhySocket *sockN,void **uptrL,void **uptrN,const struct sockaddr *from) {}
void NetconEthernetTap::phyOnTcpClose(PhySocket *sock,void **uptr) {}
void NetconEthernetTap::phyOnTcpData(PhySocket *sock,void **uptr,void *data,unsigned long len) {}
void NetconEthernetTap::phyOnTcpWritable(PhySocket *sock,void **uptr) {}

/*
 * Creates a new NetconClient for the accepted RPC connection (unix domain socket)
 *
 * Subsequent socket connections from this client will be associated with this
 * NetconClient object.
 */
void NetconEthernetTap::phyOnUnixAccept(PhySocket *sockL,PhySocket *sockN,void **uptrL,void **uptrN)
{
	//fprintf(stderr, "phyOnUnixAccept() NEW CLIENT RPC: %d\n", _phy.getDescriptor(sockN));
	rpc_sockets.push_back(sockN);
}

/*
 * Processes incoming data on a client-specific RPC connection
 */
void NetconEthernetTap::phyOnUnixData(PhySocket *sock,void **uptr,void *data,unsigned long len)
{
	unsigned char *buf = (unsigned char*)data;
	switch(buf[0])
	{
		case RPC_SOCKET:
			fprintf(stderr, "RPC_SOCKET\n");
	    struct socket_st socket_rpc;
	    memcpy(&socket_rpc, &buf[1], sizeof(struct socket_st));
	    handle_socket(sock, uptr, &socket_rpc);
			break;
	  case RPC_LISTEN:
			fprintf(stderr, "RPC_LISTEN\n");
	    struct listen_st listen_rpc;
	    memcpy(&listen_rpc, &buf[1], sizeof(struct listen_st));
	    handle_listen(sock, uptr, &listen_rpc);
			break;
	  case RPC_BIND:
			fprintf(stderr, "RPC_BIND\n");
	    struct bind_st bind_rpc;
	    memcpy(&bind_rpc, &buf[1], sizeof(struct bind_st));
	    handle_bind(sock, uptr, &bind_rpc);
			break;
	  case RPC_KILL_INTERCEPT:
			fprintf(stderr, "RPC_KILL_INTERCEPT\n");
	    //scloseClient(sock);
			break;
  	case RPC_CONNECT:
			fprintf(stderr, "RPC_CONNECT\n");
	    struct connect_st connect_rpc;
	    memcpy(&connect_rpc, &buf[1], sizeof(struct connect_st));
	    handle_connect(sock, uptr, &connect_rpc);
			break;
	  case RPC_FD_MAP_COMPLETION:
			fprintf(stderr, "RPC_FD_MAP_COMPLETION\n");
	    handle_retval(sock, uptr, buf);
			break;
		default:
			break;
	}
}

/*
 * Send a return value to the client for an RPC
 */
int NetconEthernetTap::send_return_value(TcpConnection *conn, int retval)
{
  char retmsg[4];
  memset(&retmsg, '\0', sizeof(retmsg));
  retmsg[0]=RPC_RETVAL;
  memcpy(&retmsg[1], &retval, sizeof(retval));
  int n = write(_phy.getDescriptor(conn->rpcSock), &retmsg, sizeof(retmsg));
  if(n > 0) {
		// signal that we've satisfied this requirement
    conn->pending = false;
  }
  else {
    fprintf(stderr, "unable to send return value to the intercept\n");
		closeConnection(conn);
  }
  return n;
}

/*------------------------------------------------------------------------------
--------------------------------- LWIP callbacks -------------------------------
------------------------------------------------------------------------------*/

// NOTE: these are called from within LWIP, meaning that lwipstack->_lock is ALREADY
// locked in this case!

/*
 * Callback from LWIP to do whatever work we might need to do.
 *
 * @param associated service state object
 * @param PCB we're polling on
 * @return ERR_OK if everything is ok, -1 otherwise
 *
 */
err_t NetconEthernetTap::nc_poll(void* arg, struct tcp_pcb *tpcb)
{
	//fprintf(stderr, "nc_poll\n");
	/*
	Larg *l = (Larg*)arg;
	TcpConnection *conn = l->conn;
	NetconEthernetTap *tap = l->tap;
	if(conn && conn->idx) // if valid connection and non-zero index (indicating data present)
		tap->handle_write(conn);
	*/
	return ERR_OK;
}

/*
 * Callback from LWIP for when a connection has been accepted and the PCB has been
 * put into an ACCEPT state.
 *
 * A socketpair is created, one end is kept and wrapped into a PhySocket object
 * for use in the main ZT I/O loop, and one end is sent to the client. The client
 * is then required to tell the service what new file descriptor it has allocated
 * for this connection. After the mapping is complete, the accepted socket can be
 * used.
 *
 * @param associated service state object
 * @param newly allocated PCB
 * @param error code
 * @return ERR_OK if everything is ok, -1 otherwise
 *
 */
err_t NetconEthernetTap::nc_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
	fprintf(stderr, "nc_accept()\n");
	Larg *l = (Larg*)arg;
	TcpConnection *conn = l->conn;
	NetconEthernetTap *tap = l->tap;
	int larg_fd = tap->_phy.getDescriptor(conn->dataSock);

  if(conn) {
		ZT_PHY_SOCKFD_TYPE fds[2];
		socketpair(PF_LOCAL, SOCK_STREAM, 0, fds);

		TcpConnection *new_tcp_conn = new TcpConnection();
		new_tcp_conn->dataSock = tap->_phy.wrapSocket(fds[0], new_tcp_conn);
		new_tcp_conn->rpcSock = conn->rpcSock;
		new_tcp_conn->pcb = newpcb;
		new_tcp_conn->their_fd = fds[1];
		tap->tcp_connections.push_back(new_tcp_conn);

		int send_fd = tap->_phy.getDescriptor(conn->rpcSock);

		int n = write(larg_fd, "z", 1);
    if(n > 0) {
			if(sock_fd_write(send_fd, fds[1]) > 0) {
				new_tcp_conn->pending = true;
				fprintf(stderr, "nc_accept(): socketpair = { our=%d, their=%d}\n", fds[0], fds[1]);
			}
			else {
				fprintf(stderr, "nc_accept(%d): unable to send fd to client\n", larg_fd);
			}
    }
    else {
      fprintf(stderr, "nc_accept(%d): error writing signal byte (send_fd = %d, perceived_fd = %d)\n", larg_fd, send_fd, fds[1]);
      return -1;
    }
    tap->lwipstack->_tcp_arg(newpcb, new Larg(tap, new_tcp_conn));
    tap->lwipstack->_tcp_recv(newpcb, nc_recved);
    tap->lwipstack->_tcp_err(newpcb, nc_err);
    tap->lwipstack->_tcp_sent(newpcb, nc_sent);
    tap->lwipstack->_tcp_poll(newpcb, nc_poll, 1);
    tcp_accepted(conn->pcb);
		return ERR_OK;
  }
  else {
    fprintf(stderr, "nc_accept(%d): can't locate Connection object for PCB.\n", larg_fd);
  }
  return -1;
}

/*
 * Callback from LWIP for when data is available to be read from the network.
 *
 * Data is in the form of a linked list of struct pbufs, it is then recombined and
 * send to the client over the associated unix socket.
 *
 * @param associated service state object
 * @param allocated PCB
 * @param chain of pbufs
 * @param error code
 * @return ERR_OK if everything is ok, -1 otherwise
 *
 */
err_t NetconEthernetTap::nc_recved(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
	fprintf(stderr, "nc_recved()\n");
	Larg *l = (Larg*)arg;
	int n;
  struct pbuf* q = p;

  if(!l->conn) {
		fprintf(stderr, "nc_recved(): no connection object\n");
    return ERR_OK; // ?
  }
  if(p == NULL) {
    if(l->conn) {
			fprintf(stderr, "nc_recved(): closing connection\n");
			l->tap->closeConnection(l->conn);
			exit(0);
    }
    else {
      fprintf(stderr, "nc_recved(): can't locate connection via (arg)\n");
    }
    return err;
  }
  q = p;
  while(p != NULL) { // Cycle through pbufs and write them to the socket
    if(p->len <= 0)
      break; // ?
    if((n = l->tap->_phy.streamSend(l->conn->dataSock,p->payload, p->len)) > 0) {
      if(n < p->len) {
        fprintf(stderr, "nc_recved(): unable to write entire pbuf to buffer\n");
      }
      l->tap->lwipstack->_tcp_recved(tpcb, n); // TODO: would it be more efficient to call this once at the end?
    }
    else {
      fprintf(stderr, "nc_recved(): No data written to intercept buffer\n");
    }
    p = p->next;
  }
  l->tap->lwipstack->_pbuf_free(q); // free pbufs
  return ERR_OK;
}

/*
 * Callback from LWIP when an internal error is associtated with the given (arg)
 *
 * Since the PCB related to this error might no longer exist, only its perviously
 * associated (arg) is provided to us.
 *
 * @param associated service state object
 * @param error code
 *
 */
void NetconEthernetTap::nc_err(void *arg, err_t err)
{
	//fprintf(stderr, "nc_err\n");
	Larg *l = (Larg*)arg;
  if(l->conn) {
		fprintf(stderr, "nc_err(): closing connection\n");
    l->tap->closeConnection(l->conn);
  }
  else {
    fprintf(stderr, "nc_err(): can't locate connection object for PCB\n");
  }
}

/*
 * Callback from LWIP to signal that 'len' bytes have successfully been sent.
 * As a result, we should put our socket back into a notify-on-readability state
 * since there is now room on the PCB buffer to write to.
 *
 * NOTE: This could be used to track the amount of data sent by a connection.
 *
 * @param associated service state object
 * @param relevant PCB
 * @param length of data sent
 * @return ERR_OK if everything is ok, -1 otherwise
 *
 */
err_t NetconEthernetTap::nc_sent(void* arg, struct tcp_pcb *tpcb, u16_t len)
{
	Larg *l = (Larg*)arg;
	if(len) {
		//fprintf(stderr, "len = %d\n", len);
		l->tap->_phy.setNotifyReadable(l->conn->dataSock, true);
	}
	return ERR_OK;
}

/*
 * Callback from LWIP which sends a return value to the client to signal that
 * a connection was established for this PCB
 *
 * @param associated service state object
 * @param relevant PCB
 * @param error code
 * @return ERR_OK if everything is ok, -1 otherwise
 *
 */
err_t NetconEthernetTap::nc_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
	//fprintf(stderr, "nc_connected\n");
	Larg *l = (Larg*)arg;
	l->tap->send_return_value(l->conn, err);
	return ERR_OK;
}

/*------------------------------------------------------------------------------
----------------------------- RPC Handler functions ----------------------------
------------------------------------------------------------------------------*/

/*
 * Handles an RPC to bind an LWIP PCB to a given address and port
 *
 * @param Client that is making the RPC
 * @param structure containing the data and parameters for this client's RPC
 *
 */
void NetconEthernetTap::handle_bind(PhySocket *sock, void **uptr, struct bind_st *bind_rpc)
{
  struct sockaddr_in *connaddr;
  connaddr = (struct sockaddr_in *) &bind_rpc->addr;
  int conn_port = lwipstack->ntohs(connaddr->sin_port);
  ip_addr_t conn_addr;
	conn_addr.addr = *((u32_t *)_ips[0].rawIpData());

	TcpConnection *conn = getConnectionByTheirFD(bind_rpc->sockfd);

  if(conn) {
    if(conn->pcb->state == CLOSED){
      int err = lwipstack->tcp_bind(conn->pcb, &conn_addr, conn_port);
      if(err != ERR_OK) {
				int ip = connaddr->sin_addr.s_addr;
				unsigned char d[4];
				d[0] = ip & 0xFF;
				d[1] = (ip >> 8) & 0xFF;
				d[2] = (ip >> 16) & 0xFF;
				d[3] = (ip >> 24) & 0xFF;
				fprintf(stderr, "handle_bind(): error binding to %d.%d.%d.%d : %d\n", d[0],d[1],d[2],d[3], conn_port);
      }
    }
    else fprintf(stderr, "handle_bind(): PCB not in CLOSED state. Ignoring BIND request.\n");
  }
  else fprintf(stderr, "handle_bind(): can't locate connection for PCB\n");
}

/*
 * Handles an RPC to put an LWIP PCB into LISTEN mode
 *
 * @param Client that is making the RPC
 * @param structure containing the data and parameters for this client's RPC
 *
 */
void NetconEthernetTap::handle_listen(PhySocket *sock, void **uptr, struct listen_st *listen_rpc)
{
	TcpConnection *conn = getConnectionByTheirFD(listen_rpc->sockfd);
  if(conn) {
    if(conn->pcb->state == LISTEN) {
      fprintf(stderr, "handle_listen(): PCB is already in listening state.\n");
      return;
    }
    struct tcp_pcb* listening_pcb = lwipstack->tcp_listen(conn->pcb);
    if(listening_pcb != NULL) {
      conn->pcb = listening_pcb;
      lwipstack->tcp_accept(listening_pcb, nc_accept);
			lwipstack->tcp_arg(listening_pcb, new Larg(this, conn));
			/* we need to wait for the client to send us the fd allocated on their end
			for this listening socket */
      conn->pending = true;
    }
    else {
			fprintf(stderr, "handle_listen(): unable to allocate memory for new listening PCB\n");
    }
  }
  else {
    fprintf(stderr, "handle_listen(): can't locate connection for PCB\n");
  }
}

/**
 * Handles a return value (client's perceived fd) and completes a mapping
 * so that we know what connection an RPC call should be associated with.
 *
 * @param Client that is making the RPC
 * @param structure containing the data and parameters for this client's RPC
 *
 */
void NetconEthernetTap::handle_retval(PhySocket *sock, void **uptr, unsigned char* buf)
{
	TcpConnection *conn = (TcpConnection*)*uptr;
	if(conn->pending) {
		memcpy(&(conn->perceived_fd), &buf[1], sizeof(int));
		//fprintf(stderr, "handle_retval(): Mapping [our=%d -> their=%d]\n",
		//_phy.getDescriptor(conn->dataSock), conn->perceived_fd);
		conn->pending = false;
	}
}

/*
 * Handles an RPC to create a socket (LWIP PCB and associated socketpair)
 *
 * A socketpair is created, one end is kept and wrapped into a PhySocket object
 * for use in the main ZT I/O loop, and one end is sent to the client. The client
 * is then required to tell the service what new file descriptor it has allocated
 * for this connection. After the mapping is complete, the socket can be used.
 *
 * @param Client that is making the RPC
 * @param structure containing the data and parameters for this client's RPC
 *
 */
void NetconEthernetTap::handle_socket(PhySocket *sock, void **uptr, struct socket_st* socket_rpc)
{
	struct tcp_pcb *newpcb = lwipstack->tcp_new();
  if(newpcb != NULL) {
		ZT_PHY_SOCKFD_TYPE fds[2];
		socketpair(PF_LOCAL, SOCK_STREAM, 0, fds);
		TcpConnection *new_conn = new TcpConnection();
		new_conn->dataSock = _phy.wrapSocket(fds[0], new_conn);
		*uptr = new_conn;
		new_conn->rpcSock = sock;
		new_conn->pcb = newpcb;
	  new_conn->their_fd = fds[1];
		tcp_connections.push_back(new_conn);
    sock_fd_write(_phy.getDescriptor(sock), fds[1]);
		//fprintf(stderr, "handle_socket(): socketpair = { our=%d, their=%d}\n", fds[0], fds[1]);
		/* Once the client tells us what its fd is for the other end,
		we can then complete the mapping */
    new_conn->pending = true;
  }
  else {
    fprintf(stderr, "handle_socket(): Memory not available for new PCB\n");
  }
}

/*
 * Handles an RPC to connect to a given address and port
 *
 * @param Client that is making the RPC
 * @param structure containing the data and parameters for this client's RPC
 *
 */
void NetconEthernetTap::handle_connect(PhySocket *sock, void **uptr, struct connect_st* connect_rpc)
{
	TcpConnection *conn = (TcpConnection*)*uptr;
	struct sockaddr_in *connaddr;
	connaddr = (struct sockaddr_in *) &connect_rpc->__addr;
	int conn_port = lwipstack->ntohs(connaddr->sin_port);
	ip_addr_t conn_addr = convert_ip((struct sockaddr_in *)&connect_rpc->__addr);

	if(conn != NULL) {
		lwipstack->tcp_sent(conn->pcb, nc_sent); // FIXME: Move?
		lwipstack->tcp_recv(conn->pcb, nc_recved);
		lwipstack->tcp_err(conn->pcb, nc_err);
		lwipstack->tcp_poll(conn->pcb, nc_poll, APPLICATION_POLL_FREQ);
		lwipstack->tcp_arg(conn->pcb, new Larg(this, conn));

		int err = 0;
		if((err = lwipstack->tcp_connect(conn->pcb,&conn_addr,conn_port, nc_connected)) < 0)
		{
			fprintf(stderr, "handle_connect(): unable to connect\n");
			// We should only return a value if failure happens immediately
			// Otherwise, we still need to wait for a callback from lwIP.
			// - This is because an ERR_OK from tcp_connect() only verifies
			//   that the SYN packet was enqueued onto the stack properly,
			//   that's it!
			// - Most instances of a retval for a connect() should happen
			//   in the nc_connect() and nc_err() callbacks!
			send_return_value(conn, err);
		}
		// Everything seems to be ok, but we don't have enough info to retval
		conn->pending=true;
	}
	else {
		fprintf(stderr, "could not locate PCB based on their fd\n");
	}
}
} // namespace ZeroTier

#endif // ZT_ENABLE_NETCON
