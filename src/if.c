/* This file is part of Netsukuku
 * (c) Copyright 2004 Andrea Lo Pumo aka AlpT <alpt@freaknet.org>
 *
 * This source code is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Public License as published 
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This source code is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * Please refer to the GNU Public License for more details.
 *
 * You should have received a copy of the GNU Public License along with
 * this source code; if not, write to:
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <string.h>
#include <errno.h>
#include <net/if.h>
#include <asm/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
 
#include "ll_map.c"
#include "inet.h"
#include "xmalloc.h"
#include "log.h"

extern int errno;

/* get_dev: It returs the first dev it finds up and sets `*dev_ids' to the
 * device's index. On error NULL is returned.*/
char *get_dev(int *dev_idx) 
{
	int idx;

	if((idx=ll_first_up_if()) == -1) {
		error("Couldn't find \"up\" devices. Set one dev \"up\", or specify the device name in the options.");
		return 0;
	}
	if(dev_idx)
		*dev_idx=idx;
	return ll_index_to_name(idx);
}

int set_dev_up(char *dev)
{
	u32 mask=0, flags=0;
	
	mask |= IFF_UP;
	flags |= IFF_UP;
	return set_flags(dev, flags, mask);
}

int set_dev_down(char *dev)
{
	u32 mask=0, flags=0;
	
	mask |= IFF_UP;
	flags &= ~IFF_UP;
	return set_flags(dev, flags, mask);
}

int set_flags(char *dev, u32 flags, u32 mask)
{
	struct ifreq ifr;
	int s;

	strcpy(ifr.ifr_name, dev);
	if((s=new_socket(AF_INET))) {
		error("Error while setting \"%s\" flags: Cannot open socket", dev);
		return -1;
	}

	if(ioctl(s, SIOCGIFFLAGS, &ifr)) {
		error("Error while setting \"%s\" flags: %s", dev, strerror(errno));
		close(s);
		return -1;
	}

	ifr.ifr_flags &= ~mask;
	ifr.ifr_flags |= mask&flags;
	ioctl(s, SIOCSIFFLAGS, &ifr){
		error("Error while setting \"%s\" flags: %s", dev, strerror(errno));
		close(s);
		return -1;
	}
	close(s);
	return 0;
}

/*if_init: It initializes the if to be used by Netsukuku.
 * It sets `*dev_idx' to the relative idx of `dev' and returns the device name.
 * If an error occured it returns NULL.*/
char *if_init(char *dev, int *dev_idx)
{
	struct rtnl_handle rth;
	int idx;

	ll_init_map(&rth);

	if(dev) {
		if ((*dev_idx = idx = ll_name_to_index(dev)) == 0) {
			error("if_init: Cannot find device \"%s\"\n", dev);
			return NULL;
		}
	} else 
		dev=get_dev(dev_idx);
	
	if(set_dev_up(dev))
		return NULL;	
	
	return dev;
}	

int set_dev_ip(inet_prefix ip, char *dev)
{
	int s;

	if(ip.family == AF_INET) {
		struct ifreq req;

		if((s=new_socket(AF_INET)) < 0) {
			error("Error while setting \"%s\" ip: Cannot open socket", dev);
			return -1;
		}

		strncpy(&req.ifr_name, dev, IFNAMSIZ);
		req.ifr_addr.family=ip.sa_family;
		memcpy(&req.ifr_addr.sa_data + 2, ip.data, ip.len);


		if(ioctl(s, SIOCSIFADDR, &req)) {
			error("Error while setting \"%s\" ip: %s", dev, strerror(errno));
			close(s);
			return -1;
		}
	} 
	else if(ip.family == AF_INET6) {
		struct in6_ifreq req6;

		req6.ifr6_ifindex=ll_name_to_index(dev);
		ifr6.ifr6_prefixlen=0;
		memcpy(&req6.ifr6_addr, ip.data. ip.len);

		if(ioctl(s, SIOCSIFADDR, &req6)) {
			error("Error while setting \"%s\" ip: %s", dev, strerror(errno));
			close(s);
			return -1;
		}

	}

	close(s);
	return 0;
}
