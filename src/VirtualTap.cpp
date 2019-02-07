/*
 * ZeroTier SDK - Network Virtualization Everywhere
 * Copyright (C) 2011-2019  ZeroTier, Inc.  https://www.zerotier.com/
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
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * You can be released from the requirements of the license by purchasing
 * a commercial license. Buying such a license is mandatory as soon as you
 * develop commercial closed-source software that incorporates or links
 * directly against ZeroTier software without disclosing the source code
 * of your own application.
 */

/**
 * @file
 *
 * Virtual Ethernet tap device
 */

#include "VirtualTap.hpp"
#include "Phy.hpp"
#include "Node.hpp"
#include "OSUtils.hpp"

#include "Service.hpp"
#include "Mutex.hpp"
#include "lwipDriver.hpp"
#include "libzt.h"

#ifdef _MSC_VER
#include "Synchapi.h"
#endif

namespace ZeroTier {

class VirtualTap;
extern OneService *service;
extern void postEvent(uint64_t id, int eventCode);

/**
 * A virtual tap device. The ZeroTier core service creates one of these for each
 * virtual network joined. It will be destroyed upon leave().
 */
VirtualTap::VirtualTap(
	const char *homePath,
	const MAC &mac,
	unsigned int mtu,
	unsigned int metric,
	uint64_t nwid,
	const char *friendlyName,
	void (*handler)(void *,void*,uint64_t,const MAC &,const MAC &,
		unsigned int,unsigned int,const void *,unsigned int),
	void *arg) :
		_handler(handler),
		_homePath(homePath),
		_arg(arg),
		_initialized(false),
		_enabled(true),
		_run(true),
		_mac(mac),
		_mtu(mtu),
		_nwid(nwid),
		_unixListenSocket((PhySocket *)0),
		_phy(this,false,true)
{
	memset(vtap_full_name, 0, sizeof(vtap_full_name));
	snprintf(vtap_full_name, sizeof(vtap_full_name), "libzt%llx", (unsigned long long)_nwid);
	_dev = vtap_full_name;
	::pipe(_shutdownSignalPipe);
	// Start virtual tap thread and stack I/O loops
	_thread = Thread::start(this);
}

VirtualTap::~VirtualTap()
{
	postEvent(_nwid, ZTS_EVENT_NETWORK_DOWN);
	_run = false;
	::write(_shutdownSignalPipe[1],"\0",1);
	_phy.whack();
	lwip_dispose_of_netifs(this);
	Thread::join(_thread);
	::close(_shutdownSignalPipe[0]);
	::close(_shutdownSignalPipe[1]);
}

uint64_t VirtualTap::recognizeLowerLevelInterfaceStateChange(void *n)
{
	if (!n) {
		return ZTS_EVENT_NONE;
	}
	if (n == netif4) {
		if (netif4WasUpLastCheck && !lwip_is_netif_up(netif4)) {
			netif4WasUpLastCheck = false;
			return ZTS_EVENT_NETIF_DOWN_IP4;
		}
		if (!netif4WasUpLastCheck && lwip_is_netif_up(netif4)) {
			netif4WasUpLastCheck = true;
			return ZTS_EVENT_NETIF_UP_IP4;
		}
	}
	if (n == netif6) {
		if (netif6WasUpLastCheck && !lwip_is_netif_up(netif6)) {
			netif6WasUpLastCheck = false;
			return ZTS_EVENT_NETIF_DOWN_IP6;
		}
		if (!netif6WasUpLastCheck && lwip_is_netif_up(netif6)) {
			netif6WasUpLastCheck = true;
			return ZTS_EVENT_NETIF_UP_IP6;
		}
	}
	return ZTS_EVENT_NONE;
}

void VirtualTap::lastConfigUpdate(uint64_t lastConfigUpdateTime)
{
	_lastConfigUpdateTime = lastConfigUpdateTime;
}

void VirtualTap::setEnabled(bool en)
{
	_enabled = en;
}

bool VirtualTap::enabled() const
{
	return _enabled;
}

bool VirtualTap::addIp(const InetAddress &ip)
{
	char ipbuf[INET6_ADDRSTRLEN];
	Mutex::Lock _l(_ips_m);
	lwip_init_interface((void*)this, this->_mac, ip);
	if (std::find(_ips.begin(),_ips.end(),ip) == _ips.end()) {
		_ips.push_back(ip);
		std::sort(_ips.begin(),_ips.end());
	}
	return true;
}

bool VirtualTap::removeIp(const InetAddress &ip)
{
	Mutex::Lock _l(_ips_m);
	std::vector<InetAddress>::iterator i(std::find(_ips.begin(),_ips.end(),ip));
	//if (i == _ips.end()) {
	//	return false;
	//}
	_ips.erase(i);
	if (ip.isV4()) {
		// FIXME: De-register from network stack
	}
	if (ip.isV6()) {
		// FIXME: De-register from network stack
	}
	return true;
}

std::vector<InetAddress> VirtualTap::ips() const
{
	Mutex::Lock _l(_ips_m);
	return _ips;
}

void VirtualTap::put(const MAC &from,const MAC &to,unsigned int etherType,
	const void *data,unsigned int len)
{
	lwip_eth_rx(this, from, to, etherType, data, len);
}

std::string VirtualTap::deviceName() const
{
	return _dev;
}

std::string VirtualTap::nodeId() const
{
	if (service) {
		char id[16];
		memset(id, 0, sizeof(id));
		sprintf(id, "%llx", (unsigned long long)((OneService *)service)->getNode()->address());
		return std::string(id);
	}
	else {
		return std::string("----------");
	}
}

void VirtualTap::setFriendlyName(const char *friendlyName)
{
	DEBUG_INFO("%s", friendlyName);
}

void VirtualTap::scanMulticastGroups(std::vector<MulticastGroup> &added,
	std::vector<MulticastGroup> &removed)
{
	std::vector<MulticastGroup> newGroups;
	Mutex::Lock _l(_multicastGroups_m);
	// TODO: get multicast subscriptions
	std::vector<InetAddress> allIps(ips());
	for (std::vector<InetAddress>::iterator ip(allIps.begin());ip!=allIps.end();++ip)
		newGroups.push_back(MulticastGroup::deriveMulticastGroupForAddressResolution(*ip));

	std::sort(newGroups.begin(),newGroups.end());
	std::unique(newGroups.begin(),newGroups.end());

	for (std::vector<MulticastGroup>::iterator m(newGroups.begin());m!=newGroups.end();++m) {
		if (!std::binary_search(_multicastGroups.begin(),_multicastGroups.end(),*m))
			added.push_back(*m);
	}
	for (std::vector<MulticastGroup>::iterator m(_multicastGroups.begin());m!=_multicastGroups.end();++m) {
		if (!std::binary_search(newGroups.begin(),newGroups.end(),*m))
			removed.push_back(*m);
	}
	_multicastGroups.swap(newGroups);
}

void VirtualTap::setMtu(unsigned int mtu)
{
	_mtu = mtu;
}

void VirtualTap::threadMain()
	throw()
{
	fd_set readfds,nullfds;
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	FD_ZERO(&readfds);
	FD_ZERO(&nullfds);
	int nfds = (int)std::max(_shutdownSignalPipe[0],0) + 1;
#if defined(__linux__)
	pthread_setname_np(pthread_self(), vtap_full_name);
#endif
#if defined(__APPLE__)
	pthread_setname_np(vtap_full_name);
#endif
	while (true) {
		FD_SET(_shutdownSignalPipe[0],&readfds);
		select(nfds,&readfds,&nullfds,&nullfds,&tv);
		// writes to shutdown pipe terminate thread
		if (FD_ISSET(_shutdownSignalPipe[0],&readfds)) {
			break;
		}
#if defined(_WIN32)
		Sleep(ZTS_TAP_THREAD_POLLING_INTERVAL);
#else
		struct timespec sleepValue = {0};
		sleepValue.tv_nsec = ZTS_TAP_THREAD_POLLING_INTERVAL * 500000;
		nanosleep(&sleepValue, NULL);
#endif
	}
}

void VirtualTap::Housekeeping()
{
    //
}

void VirtualTap::phyOnDatagram(PhySocket *sock,void **uptr,const struct sockaddr *local_address,
	const struct sockaddr *from,void *data,unsigned long len) {}
void VirtualTap::phyOnTcpConnect(PhySocket *sock,void **uptr,bool success) {}
void VirtualTap::phyOnTcpAccept(PhySocket *sockL,PhySocket *sockN,void **uptrL,void **uptrN,
	const struct sockaddr *from) {}
void VirtualTap::phyOnTcpClose(PhySocket *sock,void **uptr) {}
void VirtualTap::phyOnTcpData(PhySocket *sock,void **uptr,void *data,unsigned long len) {}
void VirtualTap::phyOnTcpWritable(PhySocket *sock,void **uptr) {}

} // namespace ZeroTier