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
#include "libnetlink.h"
#include "map.h"
#include "gmap.h"
#include "route.h"
#include "netsukuku.h"
#include "xmalloc.h"
#include "log.h"

extern struct current me;
extern int my_family;

void ctr_add(ct_entry *ct, ct_route *ctr)
{
	list_add(ct->ctr, ctr);
}

u_char rt_find_table(ct_route *ctr, u32 dst, u32 gw)
{
	ct_route *i;
	u_char tables[MAX_ROUTE_TABLES];
	int l;
	
	memset(tables, '\0', MAX_ROUTE_TABLES);
	
	for(i=ctr; i; i=i->next) {
		if(i->dst==dst) {
			if(i->gw==gw)
				return i->ct_table;
			else 
				tables[i->ct_table]=1;
		}
	}

	for(l=1; l<MAX_ROUTE_TABLES; l++)
		if(!tables[l])
			return l;

	return 0xff; /*This shouldn't happen!*/
}

void krnl_update_node(void *void_node, u_char level)
{
	map_node *node=void_node;
	map_gnode *gnode=void_node;
	map_gnode *gto;	/*Great Teacher Onizuka*/
	
	struct nexthop *nh=0;
	inet_prefix to;
	int i, node_pos;

	if(!level) {
		nh=xmalloc(sizeof(struct nexthop)*(node->links+1));
		memset(nh, '\0', sizeof(nexthop), node->links+1);

		maptoip(me.int_map, node, me.cur_quadg.ipstart[1], &to);

		for(i=0; i<node->links; i++) {
#ifdef QMAP_STYLE_I
			maptoip(me.int_map, get_gw_node(node, i), me.cur_quadg.ipstart[1], &nh[i].gw);
#else /*QMAP_STYLE_II*/
			maptoip(me.int_map, node.r_node[i].r_node, me.cur_quadg.ipstart[1], &nh[i].gw);
#endif
			nh[i].dev=me.cur_dev;
			nh[i].hops=255-i;
		}
		node_pos=pos_from_node(node, me.int_map);
	} else {
		node=&gnode->g;
		node_pos=pos_from_gnode(gnode, me.ext_map[_EL(level)]);
		gnodetoip(me.ext_map, &me.cur_quadg, gnode, level, &to);
		nh=0;
	}
	
	if(node.flags & MAP_VOID) {
		/*Ok, let's delete it*/
		if(route_del(to, nh, me.cur_dev, 0))
			error("WARNING: Cannot delete the route entry for the ",
					"%d %cnode!", node_pos, !level ? ' ' : 'g');
	} else
		if(route_replace(to, nh, me.cur_dev, 0))
			error("WARNING: Cannot update the route entry for the "
					"%d %cnode!", node_pos, !level ? ' ' : 'g');
	if(nh)
		xfree(nh);
}

void rt_update(void)
{
	u_short i;
	
	for(i=0; i<MAXGROUPNODE; i++) {
		if(me.int_map[i].flags & MAP_UPDATE && !(me.int_map[i].flags & MAP_RNODE)) {
			krnl_update_node(&me.int_map[i]);
			me.int_map[i].flags&=~MAP_UPDATE;
		}
	}
	route_flush_cache(my_family);
}

int rt_add_def_gw(char *dev) 
{
	inet_prefix to;
	
	if(inet_setip_anyaddr(&to)) {
		error("rt_add_def_gw(): Cannot use INADRR_ANY for the %d family\n", to.family);
		return -1;
	}
	return route_add(to, 0, dev, 0);
}
