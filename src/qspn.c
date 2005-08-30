/* This file is part of Netsukuku
 * (c) Copyright 2005 Andrea Lo Pumo aka AlpT <alpt@freaknet.org>
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
 *
 * --
 * qspn.c:
 * Here there is the code that implements the Quantum Shortest Path Netsukuku
 * meta-algorithm, the heart of Netsukuku.
 */

#include "includes.h"

#include "llist.c"
#include "inet.h"
#include "map.h"
#include "gmap.h"
#include "bmap.h"
#include "request.h"
#include "pkts.h"
#include "tracer.h"
#include "qspn.h"
#include "netsukuku.h"
#include "log.h"
#include "xmalloc.h"
#include "misc.h"

void qspn_set_map_vars(u_char level, map_node **map, map_node **root_node, 
		int *root_node_pos, map_gnode **gmap)
{
	if(!level) {
		if(map)
			*map=me.int_map;
		if(root_node)
			*root_node=me.cur_node;
		if(root_node_pos)
			*root_node_pos=pos_from_node(me.cur_node, me.int_map);
	} else {
		if(map)
			*map=(map_node *)me.ext_map[_EL(level)];
		if(gmap)
			*gmap=me.ext_map[_EL(level)];
		if(root_node)
			*root_node=&me.cur_quadg.gnode[_EL(level)]->g;
		if(root_node_pos)
			*root_node_pos=me.cur_quadg.gid[level];
	}
}

/* 
 * qspn_time_reset: Reset the qspn time of all the levels that go from
 * `start_level' to `end_level'. The total number of effective levels is
 * specified in `levels'.
 */
void qspn_time_reset(int start_level, int end_level, int levels)
{
	struct timeval cur_t;
	int i;

	if(end_level <= start_level)
		end_level = start_level+1;

	/* 
	 * We fake the cur_qspn_time, so qspn_round_left thinks that a
	 * qspn_round was already sent 
	 */
	gettimeofday(&cur_t, 0);
	cur_t.tv_sec-=QSPN_WAIT_ROUND_LVL(levels)*2;
	
	for(i=start_level; i < end_level; i++)
		memcpy(&me.cur_qspn_time[i], &cur_t, sizeof(struct timeval));
}

void qspn_init(u_char levels)
{
	/* register the qspn/tracer's ops in the pkt_op_table */
	add_pkt_op(TRACER_PKT, 	       SKT_UDP, ntk_udp_port, tracer_pkt_recv);
	add_pkt_op(TRACER_PKT_CONNECT, SKT_UDP, ntk_udp_port, tracer_pkt_recv);
	add_pkt_op(QSPN_CLOSE, SKT_UDP, ntk_udp_port, qspn_close);
	add_pkt_op(QSPN_OPEN,  SKT_UDP, ntk_udp_port, qspn_open);


	/* 
	 * Alloc the qspn stuff 
	 */
	
	qspn_b=xmalloc(sizeof(struct qspn_buffer *)*levels);
	memset(qspn_b, 0, sizeof(struct qspn_buffer *)*levels);
	
	qspn_send_mutex=xmalloc(sizeof(int)*levels);
	memset(qspn_send_mutex, 0, sizeof(int)*levels);
	
	me.cur_qspn_id=xmalloc(sizeof(int)*levels);
	memset(me.cur_qspn_id, 0, sizeof(int)*levels);
	
	me.cur_qspn_time=xmalloc(sizeof(struct timeval)*levels);

	qspn_time_reset(0, levels, levels);
}

void qspn_free(void)
{
	xfree(qspn_b);
	xfree(qspn_send_mutex);
	xfree(me.cur_qspn_id);
	xfree(me.cur_qspn_time);
}

void qspn_b_clean(u_char level)
{
	struct qspn_buffer *qb=qspn_b[level];
	list_for(qb) {
		if(!qb->replies)
			continue;

		if(qb->replier)
			xfree(qb->replier);
		if(qb->flags)
			xfree(qb->flags);
		qb->replies=0;
		qb->replier=0;
		qb->flags=0;
	}
}

/* 
 * qspn_b_add: It adds a new element in the qspn_b 'qb' buffer and returns its
 * position. 
 */
int qspn_b_add(struct qspn_buffer *qb, u_char replier, u_short flags)
{
	qb->replies++;
	qb->replier=xrealloc(qb->replier, sizeof(u_char)*qb->replies);
	qb->flags=xrealloc(qb->flags, sizeof(u_char)*qb->replies);
	
	qb->replier[qb->replies-1]=replier;
	qb->flags[qb->replies-1]=flags;
	
	return qb->replies-1;
}

struct qspn_buffer *
qspn_b_find_rnode(struct qspn_buffer *qb, map_node *rnode)
{
	if(!qb)
		return 0;
	
	list_for(qb)
		if(qb->rnode == rnode)
			return qb;

	return 0;
}

int qspn_b_find_reply(struct qspn_buffer *qb, int sub_id)
{
	int i;

	if(!qb)
		return -1;

	for(i=0; i<qb->replies; i++)
		if(qb->replier[i] == sub_id)
			return i;
	return -1;
}

/* 
 * qspn_round_left: It returns the milliseconds left before the QSPN_WAIT_ROUND
 * expires. If the round is expired it returns 0.
 */
int qspn_round_left(u_char level)
{
	struct timeval cur_t, t;
	int wait_round, cur_elapsed, diff;
	
	gettimeofday(&cur_t, 0);

	timersub(&cur_t, &me.cur_qspn_time[level], &t);

	if(t.tv_sec >= 1) {
		/* 
		 * There are still seconds left, so, let's not consider the
		 * millisec.
		 */
		wait_round = QSPN_WAIT_ROUND_LVL(level);
		cur_elapsed = t.tv_usec;
		diff = wait_round - cur_elapsed;
		
		/* 
		 * We need to return diff in millisec, be sure to not overflow
		 * the int
		 */
		if(diff > (INT_MAX/1000))
			diff=(INT_MAX/1000)*1000;
		else
			diff*=1000;
	} else {
		wait_round  = QSPN_WAIT_ROUND_MS_LVL(level);
		cur_elapsed = MILLISEC(t);
		diff = wait_round - cur_elapsed;
	}

	return cur_elapsed >= wait_round ? 0 : diff;
}


/* 
 * update_qspn_time: It updates me.cur_qspn_time;
 * Oh, sorry this code doesn't show consideration for the relativity time shit.
 * So you can't move at a velocity near the light's speed. I'm sorry.
 */
void update_qspn_time(u_char level, struct timeval *new_qspn_time)
{
	struct timeval cur_t, t;
	int ret;

	gettimeofday(&cur_t, 0);

	if(new_qspn_time) {
		timersub(&cur_t, new_qspn_time, &me.cur_qspn_time[level]);
		memcpy(&t, new_qspn_time, sizeof(struct timeval));
	} else
		timersub(&cur_t, &me.cur_qspn_time[level], &t);

	ret=QSPN_WAIT_ROUND_MS_LVL(level) - MILLISEC(t);

	if(ret < 0 && abs(ret) > QSPN_WAIT_ROUND_MS_LVL(level)) {
		ret*=-1;
		/* 
		 * we round `ret' to take off the time of the passed round, 
		 * then we can store in `ret' the number of ms passed since the
		 * latest round.
		 */
		ret=ret-(QSPN_WAIT_ROUND_MS_LVL(level)*(ret/QSPN_WAIT_ROUND_MS_LVL(level)));
		t.tv_sec=ret/1000;
		t.tv_usec=(ret - (ret/1000)*1000)*1000;
		
		/* 
		 * Now we can calculate when the last round has started, the
		 * result is stored in `me.cur_qspn_time[level]'
		 */
		timersub(&cur_t, &t, &me.cur_qspn_time[level]);
	}
}

/* 
 * qspn_new_round: It prepares all the buffers for the new qspn_round and 
 * removes the QSPN_OLD nodes from the map. The new qspn_round id is set 
 * to `new_qspn_id'. If `new_qspn_id' is zero then the id is incremented by one
 * If `new_qspn_time' is not null, the qspn_time[level] is set to the current
 * time minus `new_qspn_time'.
 */
void qspn_new_round(u_char level, int new_qspn_id, struct timeval *new_qspn_time)
{
	int bm, i, node_pos;
	map_node *map, *root_node, *node;
	map_gnode *gmap;
	
	qspn_set_map_vars(level, 0, &root_node, 0, &gmap);
	map=me.int_map;

	/* New round activated. Destroy the old one. beep. */
	if(new_qspn_id)
		me.cur_qspn_id[level]=new_qspn_id;
	else
		me.cur_qspn_id[level]++;

	if(new_qspn_time)
		update_qspn_time(level, new_qspn_time);
	else
		update_qspn_time(level, 0);
	
	qspn_b_clean(level);
	bmap_counter_reset(BMAP_LEVELS(me.cur_quadg.levels), 
			me.bmap_nodes_closed);
	bmap_counter_reset(BMAP_LEVELS(me.cur_quadg.levels),
			me.bmap_nodes_opened);
	 

	/* Clear the flags set during the previous qspn */
	root_node->flags&=~QSPN_STARTER & ~QSPN_CLOSED & ~QSPN_OPENED;
	for(i=0; i<root_node->links; i++) {
		node=(map_node *)root_node->r_node[i].r_node;
		node->flags &= ~QSPN_CLOSED & ~QSPN_OPENED & 
			       ~QSPN_STARTER & ~QSPN_OPENER;
	}

	/*
	 * How to remove the dead nodes from the map? How do we know which are 
	 * deads?
	 * Pretty simple, we can't know so we wait until the next qspn_round to
	 * break them if they didn't show in the while. 
	 */
	for(i=0; i<MAXGROUPNODE; i++) {
		node_pos=i;
		if(!level)
			node=(map_node *)&map[node_pos];
		else {
			node=(map_node *)&gmap[node_pos];
			if(gmap[node_pos].flags & GMAP_VOID)
				continue;
		}
			
		if(node->flags & MAP_ME || node->flags & MAP_VOID)
			continue;

		if((node->flags & QSPN_OLD)) {
			
			if((node->flags & MAP_BNODE) && level < me.cur_quadg.levels-1) {
				/* 
				 * The node is a border node, delete it from
				 * the bmap.
				 */
				bm=map_find_bnode(me.bnode_map[level],
						me.bmap_nodes[level], node_pos);
				if(bm != -1)
				    me.bnode_map[level] =
					    map_bnode_del(me.bnode_map[level], 
						&me.bmap_nodes[level],
						&me.bnode_map[level][bm]);
			}

			if(!level) {
				debug(DBG_NORMAL, "qspn: The node %d is dead", i);
				map_node_del(node);
			} else {
				debug(DBG_NORMAL,"The groupnode %d of level %d"
						" is dead", i, level);
				gmap_node_del((map_gnode *)node);
			}

			me.cur_quadg.gnode[level]->seeds--;
		} else
			node->flags|=QSPN_OLD;
	}
}

/* * *  Exclude functions. (see pkts.h)  * * */
int exclude_from_and_opened_and_glevel(TRACER_PKT_EXCLUDE_VARS)
{
	map_node *rn;
	struct qspn_buffer *qb, *qbp;
	int reply;
	u_char level;

	if(exclude_from_and_glevel(TRACER_PKT_EXCLUDE_VARS_NAME))
		                return 1;
	
	level=excl_level-1;
	
	qb=qspn_b[level];
	if(e_rnode && level-1 >= 0)
		rn=&e_rnode->quadg.gnode[_EL(excl_level-1)]->g;
	else
		rn=(map_node *)me.cur_node->r_node[pos].r_node;
	qbp=qspn_b_find_rnode(qb, rn);
	if(!qbp)
		return 0;

	reply=qspn_b_find_reply(qbp, sub_id);
	
	if(qbp->flags[reply] & QSPN_OPENED)
		return 1;
	return 0;
}

int exclude_from_and_glevel_and_closed(TRACER_PKT_EXCLUDE_VARS)
{
	if((node->flags & QSPN_CLOSED) || 
			exclude_from_and_glevel(TRACER_PKT_EXCLUDE_VARS_NAME))
		return 1;
	return 0;
}

int exclude_from_and_glevel_and_notstarter(TRACER_PKT_EXCLUDE_VARS)
{
	int level=excl_level-1;

	if(exclude_from_and_glevel(TRACER_PKT_EXCLUDE_VARS_NAME))
		return 1;

	
	if((!level || (node->flags & MAP_BNODE)) && !(node->flags & QSPN_STARTER))
		return 1;
	
	return 0;
}


/*
 * The Holy qspn_send. It is used to send a new qspn_round when something 
 * changes around the root_node (me).
 */
int qspn_send(u_char level)
{
	PACKET pkt;
	map_node *from;
	int round_ms, ret, ret_err, upper_gid, root_node_pos, qid;
	map_node *map, *root_node;
	map_gnode *gmap;
	u_char upper_level;

	qid=me.cur_qspn_id[level];
	from=me.cur_node;
	upper_level=level+1;
	qspn_set_map_vars(level, &map, &root_node, &root_node_pos, &gmap);


	/* 
	 * Now I explain how the level stuff in the qspn works. For example, if
	 * we want to propagate the qspn in the level 2, we store in qspn.level
	 * the upper level (3), and the gid of the upper_level which containts 
	 * the entire level 2. Simple no?
	 */

	
	/*If we aren't a bnode it's useless to send qspn in higher levels*/
	if(level && !(me.cur_node->flags & MAP_BNODE))
		return -1;
	
	if(qspn_send_mutex[level])
		return 0;
	else
		qspn_send_mutex[level]=1;

	/*
	 * We have to wait the finish of the old qspn_round to start the 
	 * new one.
	 */
	while((round_ms=qspn_round_left(level)) > 0) {
		debug(DBG_INSANE, "Waiting %dms to send a new qspn_round, lvl:"
				" %d", round_ms, level);
		usleep(round_ms*1000);
		update_qspn_time(level, 0);
	}

	/* 
	 * If, after the above wait, the old saved qspn_id (`qid') it's not the
	 * same of the current it means that we receveid already a new 
	 * qspn_round in this level, so forget about it ;) 
	 */
	if(qid != me.cur_qspn_id[level])
		return 0;
	
	qspn_new_round(level, 0, 0);
	root_node->flags|=QSPN_STARTER;

	upper_gid=me.cur_quadg.gid[upper_level];
	ret_err=tracer_pkt_build(QSPN_CLOSE, me.cur_qspn_id[level], root_node_pos, /*IDs*/
			 upper_gid,  level,
			 0,          0,         	    0, 		   /*Received tracer_pkt*/
			 0,          0,              	    0, 		   /*bnode_block*/
			 &pkt);						   /*Where the pkt is built*/
	if(ret_err) {
		debug(DBG_NOISE, "Cannot send the new qspn_round: "
				"tracer_pkt build failed.");
		ret=-1;
		goto finish;
	}

	/*... send the qspn_opened to our r_nodes*/
	flood_pkt_send(exclude_from_and_glevel_and_closed, upper_level, -1, 
			-1, pkt);

	debug(DBG_INSANE, "Qspn_round lvl: %d id: 0x%x sent", level, 
			me.cur_qspn_id[level]);

finish:
	qspn_send_mutex[level]=0;
	return ret;
}

/*
 * qspn_open_start: sends a new qspn_open when all the links are closed.
 * `from' is the node who sent the last qspn_close which closed the last 
 * not-closed link. 
 * `pkt_to_all' is the the last qspn_close pkt sent by from, which is an rnode
 * at the `from_rpos' position in the me.cur_node rnodes. `pkt_to_all' must 
 * be passed with the new tracer_pkt entry already added because it is 
 * sent as is.
 * `qspn_id', `root_node_pos', `gid' and `level' are the same parameters passed
 * to tracer_pkt_build to build the `pkt_to_all' pkt.
 * This functions is called only by qspn_close().
 */
int qspn_open_start(int from_rpos, PACKET pkt_to_all, int qspn_id, 
		int root_node_pos, int gid, int level)
{
	PACKET pkt_to_from;
	int upper_level, ret_err;

	upper_level=level+1;
	
	debug(DBG_INSANE, "Fwd %s(0x%x) lvl %d, to broadcast", 
			rq_to_str(QSPN_OPEN), qspn_id, level);
	
	/* 
	 * The `from' node doesn't need all the previous tracer_pkt entry 
	 * (which are kept in `pkt_to_all'), so we build a new tracer_pkt
	 * only for it.
	 */
	ret_err=tracer_pkt_build(QSPN_OPEN, qspn_id, root_node_pos, gid, level,
			0, 0, 0, 0, 0, 0, &pkt_to_from);
	if(ret_err)
		debug(DBG_NOISE, "Cannot send the new qspn_open: "
				"pkt build failed.");
	else
		/* Send the pkt to `from' */
		flood_pkt_send(exclude_all_but_notfrom, upper_level,
				-1, from_rpos, pkt_to_from);

	/* Send the `pkt_to_all' pkt to all the other rnodes (if any)*/
	if(me.cur_node->links > 1) {
		pkt_to_all.hdr.op=QSPN_OPEN;
		flood_pkt_send(exclude_from_and_glevel, upper_level, 
				-1, from_rpos, pkt_to_all);
	}

	return 0;
}


/* 
 * Damn, this function is so ugly, it's a real pain. 19 args. ARGH!
 * But without it I had to copy two times this code, and even if I choose to
 * use a struct to pass all the args, they are still too many and it will be
 * uglier than this.
 * I'm sorry.
 * Ah, yes, this function splits the unpacked qspn_pkt and returns a lot of
 * vars. * DO NOT TRY THIS AT HOME *
 */
int qspn_unpack_pkt(PACKET rpkt, brdcast_hdr **new_bcast_hdr, 
		tracer_hdr **new_tracer_hdr, tracer_chunk **new_tracer, 
		bnode_hdr **new_bhdr, size_t *new_bblock_sz, 
		quadro_group *rip_quadg, int *new_real_from_rpos,
		u_short *new_hops, u_char *new_upper_level, int *new_gid,
		map_node **new_from, map_node **new_root_node, 
		map_node **new_tracer_starter, int *new_sub_id, 
		int *new_root_node_pos, u_char *new_level, u_char *new_blevel,
		char *new_just_forward_it, char *new_do_real_qspn_action)
{	

	brdcast_hdr  *bcast_hdr;
	tracer_hdr   *trcr_hdr;
	tracer_chunk *tracer;
	bnode_hdr    *bhdr=0;
	size_t bblock_sz=0;
	int ret_err;
	u_short hops;

	map_node *from, *root_node, *tracer_starter;
	int gid, root_node_pos, real_from_rpos, sub_id;
	u_char level, upper_level, blevel;
	
	map_gnode *gfrom, *gtracer_starter;
	const char *ntop;
	char do_real_qspn_action=0, just_forward_it=0;

	if(server_opt.dbg_lvl) {
		ntop=inet_to_str(rpkt.from);
		debug(DBG_NOISE, "%s(0x%x) from %s", rq_to_str(rpkt.hdr.op),
				rpkt.hdr.id, ntop);
	}

	ret_err=tracer_unpack_pkt(rpkt, &bcast_hdr, &trcr_hdr, &tracer, &bhdr, 
			&bblock_sz, rip_quadg, &real_from_rpos);
	if(ret_err) {
		ntop=inet_to_str(rpkt.from);
		debug(DBG_NOISE, "qspn_unpack_pkt(): The %s node sent an "
				"invalid %s (0x%x) pkt here.", ntop,
				rq_to_str(rpkt.hdr.op), rpkt.hdr.id);
		return -1;
	}

	gid	    = bcast_hdr->g_node;
	upper_level = level = bcast_hdr->level;
	hops	    = trcr_hdr->hops;

	if(!level || level==1) {
		level=0;
		qspn_set_map_vars(level, 0, &root_node, &root_node_pos, 0);
		from		= node_from_pos(tracer[hops-1].node,
					me.int_map);
		tracer_starter	= node_from_pos(tracer[0].node, me.int_map);
	} else {
		level--;
		qspn_set_map_vars(level, 0, &root_node, &root_node_pos, 0);
		gfrom		= gnode_from_pos(tracer[hops-1].node, 
					me.ext_map[_EL(level)]);
		from		= &gfrom->g;
		gtracer_starter	= gnode_from_pos(tracer[0].node, 
					me.ext_map[_EL(level)]);
		tracer_starter	= &gtracer_starter->g;
	}

	blevel = level-1;
	from->flags&=~QSPN_OLD;
	sub_id=bcast_hdr->sub_id;

	/* Only if we are in the level 0, or if we are a bnode, we can do the
	 * real qspn actions, otherwise we simply forward the pkt.
	 * In other words:
	 * `just_forward_it'==0 means that we are a truly bnode, or that 
	 * level is 0.
	 * `do_real_qspn_action'==1 means that we are a bnode also at `level'
	 * or that level is 0
	 */
	if(level && !(me.cur_node->flags & MAP_BNODE))
		just_forward_it=1;
	if(!level || ((root_node->flags & MAP_BNODE) && !just_forward_it))
		do_real_qspn_action=1;
	

	/* Return all the load of pointers, Argh */

	*new_bcast_hdr=bcast_hdr;
	*new_tracer_hdr=trcr_hdr;
	*new_tracer=tracer;
	*new_bhdr=bhdr;
	*new_bblock_sz=bblock_sz;

	*new_hops=hops;
	*new_upper_level=upper_level;
	*new_gid=gid;
	*new_sub_id=sub_id;

	*new_from=from;
	*new_root_node=root_node;
	*new_tracer_starter=tracer_starter;
	*new_gid=gid;
	*new_root_node_pos=root_node_pos;
	*new_real_from_rpos=real_from_rpos;
	*new_level=level;
	*new_blevel=blevel;
	*new_upper_level=upper_level;

	*new_just_forward_it=just_forward_it;
	*new_do_real_qspn_action=do_real_qspn_action;

	return 0;
}


/* 
 * qspn_close: It receive a QSPN_CLOSE pkt, analyzes it, stores the routes,
 * closes the rpkt.from link and then keeps forwarding it to all the non 
 * closed links. If all the links are closed, a qspn_open will be sent.
 */
int qspn_close(PACKET rpkt)
{
	PACKET pkt;
	brdcast_hdr  *bcast_hdr;
	tracer_hdr   *trcr_hdr;
	tracer_chunk *tracer;
	bnode_hdr    *bhdr=0;
	size_t bblock_sz=0, old_bblock_sz;
	int i, not_closed=0, ret=0, ret_err;
	u_short hops, old_bchunks=0;
	const char *ntop;
	char *old_bblock=0;
	char do_real_qspn_action=0, just_forward_it=0, int_qspn_starter=0;
	char all_bnodes_are_closed=0;

	map_node *from, *root_node, *tracer_starter, *node;
	quadro_group rip_quadg;
	struct timeval trtt;
	int gid, root_node_pos, real_from_rpos, sub_id;
	u_char level, upper_level, blevel;


	/*
	 * * Unpack the qspn pkt and split it * *
	 */
	ret_err=qspn_unpack_pkt(rpkt, &bcast_hdr, &trcr_hdr, &tracer, &bhdr,
			&bblock_sz, &rip_quadg, &real_from_rpos,
			&hops, &upper_level, &gid,
			&from, &root_node,
			&tracer_starter, &sub_id,
			&root_node_pos, &level, &blevel,
			&just_forward_it, &do_real_qspn_action);
	if(ret_err < 0) {
		ret = -1;
		goto finish;
	}

#ifdef DEBUG
	debug(DBG_INSANE, "QSPN_CLOSE(0x%x, lvl %d): node[0]: %d, node[1]: %d, hops: %d", 
			rpkt.hdr.id, level, tracer[0].node, 
			trcr_hdr->hops > 1 ? tracer[1].node : -1 ,
			trcr_hdr->hops);
#endif


	/*
	 *   * * Verify the qspn_close pkt * *
	 */
	
	/* If the rpkt is the same qspn_close we sent we can drop it */
	if( ( !level || (do_real_qspn_action && 
					(root_node->flags & QSPN_STARTER)) )
			&& tracer_starter == root_node) {
		ntop=inet_to_str(rpkt.from);
		debug(DBG_NOISE, "qspn_close(0x%x): Dropped qspn_close from "
				"%s: we are the qspn_starter of that pkt!"
				" (hops: %d)", rpkt.hdr.id, ntop,
				trcr_hdr->hops);
		ret=-1;
		goto finish;
	} 
	
	/* 
	 * Check if the qspn_round is old or if it is the new one. 
	 */
	if(rpkt.hdr.id >= me.cur_qspn_id[level]+1) {
		/* Happy new round */
		tracer_get_trtt(real_from_rpos, trcr_hdr, tracer, &trtt);
		debug(DBG_NOISE, "New qspn_round 0x%x lvl %d received,"
				" new qspn_time: %dms",	rpkt.hdr.id,
				level, MILLISEC(trtt));
		qspn_new_round(level, rpkt.hdr.id, &trtt);

	} else if(rpkt.hdr.id < me.cur_qspn_id[level]) {
		/* Reject it, it's old */
		ntop=inet_to_str(rpkt.from);
		debug(DBG_NOISE, "qspn_close(): %s sent a qspn_close"
				" with a wrong qspn_id(0x%x,lvl %d)"
				"qid 0x%x", ntop, rpkt.hdr.id, level, 
				me.cur_qspn_id[level]);
		ret=-1;
		goto finish;
	}

	/* Some bnode, which is in the same gnode where we are, sent a
	 * qspn_close, so we are a qspn_starter too */
	if(level && tracer_starter == root_node && hops == 1 &&
			do_real_qspn_action) {
		root_node->flags|=QSPN_STARTER;

		/* This flag indicates that the new qspn_round we received was
		 * sent from our gnode, so it is an internal qspn starter.*/
		int_qspn_starter=1;
	}

	/* We have only to forward it, nothing more */
	if(level && from == root_node)
		just_forward_it=1;
	
	/* Time to update our maps */
	tracer_store_pkt(rpkt.from, &rip_quadg, level, trcr_hdr, tracer,
			(void *)bhdr, bblock_sz, &old_bchunks, &old_bblock,
			&old_bblock_sz);

	
	if(hops > 1 && !int_qspn_starter && (root_node->flags & QSPN_STARTER) &&
			!(from->flags & QSPN_STARTER)) {
		ntop=inet_to_str(rpkt.from);
		debug(DBG_NOISE, "qspn_close(): Dropped qspn_close from %s: we"
				" are a qspn_starter, the pkts has (hops=%d)>1"
				" and was forwarded by a non qspn_starter",
				ntop, hops);
		goto finish;
	}

	if(bcast_hdr->flags & QSPN_BNODE_CLOSED) {
		if(from == root_node) {
			/* 
			 * This pkt passed through a bnode which has all its
			 * links closed. Increment the counter.
			 */
			me.bmap_nodes_closed[blevel]++;
		} else 
			bcast_hdr->flags &= ~QSPN_BNODE_CLOSED;
	}


	if(!level || me.bmap_nodes_closed[blevel] >= (me.bmap_nodes[blevel]-1))
		all_bnodes_are_closed=1;
	
	not_closed=0;
	if(do_real_qspn_action && !just_forward_it) {
		/*
		 * We close the from node and we see there are any links still 
		 * `not_closed'.
		 */
		for(i=0; i<root_node->links; i++) {
			node=(map_node *)root_node->r_node[i].r_node;

			if(root_node->r_node[i].r_node == (int *)from) {
				debug(DBG_INSANE, "Closing %x [g]node", node);
				node->flags|=QSPN_CLOSED;
			}

			if(!(node->flags & QSPN_CLOSED))
				not_closed++;
		}

		/* If we are a starter then `from' is starter too */
		if(root_node->flags & QSPN_STARTER ) {
			from->flags|=QSPN_STARTER;
			bcast_hdr->flags|=BCAST_TRACER_STARTERS;
		}

		/* 
		 * If we have the links closed and we are in level > 0, set
		 * the flags to let the other bnodes know.
		 */
		if(!not_closed && level && !(root_node->flags & QSPN_CLOSED)) {
			bcast_hdr->flags|=QSPN_BNODE_CLOSED;
			root_node->flags|=QSPN_CLOSED;
		}
		
		/*We build d4 p4ck37...*/
		ret_err=tracer_pkt_build(QSPN_CLOSE, rpkt.hdr.id, root_node_pos, /*IDs*/
				gid,	level,
				bcast_hdr,  trcr_hdr,    tracer,        /*Received tracer_pkt*/
			   	old_bchunks,old_bblock,  old_bblock_sz, /*bnode_block*/
			        &pkt);				        /*Where the pkt is built*/
		if(ret_err) {
			debug(DBG_NOISE, "Cannot forward the qspn_close: "
					"pkt build failed.");
			ret=-1; 
			goto finish;
		}

	} else {
		/* 
		 * Increment the rtt of the last gnode chunk, because we
		 * aren't adding any entry, but we are just forwarding it.
		 */
		debug(DBG_INSANE, "qspn_close: Incrementing the last hops rtt.");
		ret_err=tracer_add_rtt(real_from_rpos, tracer, hops-1);
		if(ret_err < 0)
			debug(DBG_NOISE, "tracer_add_rtt(0x%x) hop %d failed",
					rpkt.hdr.id, hops-1);

		/* the pkt we're sending is a copy of the received one */
		pkt_copy(&pkt, &rpkt);
		pkt_clear(&pkt);
	}
	

	/*
	 * * Forward the new pkt * *
	 */
	
	if(!just_forward_it && !not_closed && 
		!(root_node->flags & QSPN_OPENER) &&
			!(root_node->flags & QSPN_STARTER) &&
			all_bnodes_are_closed) {
		/*
		 * We have all the links closed and we haven't sent a 
		 * qspn_open yet, time to become an opener
		 */
		qspn_open_start(real_from_rpos, pkt, rpkt.hdr.id, root_node_pos, 
				gid, level);
		root_node->flags|=QSPN_OPENER;
	} else if((root_node->flags & QSPN_STARTER) && !int_qspn_starter) {
		/* We send a normal tracer_pkt limited to the qspn_starter nodes */
		pkt.hdr.op=TRACER_PKT;
		pkt.hdr.id=++root_node->brdcast;
		debug(DBG_INSANE, "Fwd %s(0x%x) lvl %d to the qspn starters	<<<<", 
				rq_to_str(pkt.hdr.op),  pkt.hdr.id, level);
		
		flood_pkt_send(exclude_from_and_glevel, upper_level, -1,
				real_from_rpos, pkt);
	} else {
		/* 
		 * Forward the qspn_close to all our r_nodes which are not 
		 * closed!
		 */
		debug(DBG_INSANE, "Fwd %s(0x%x) lvl %d to broadcast", 
				rq_to_str(pkt.hdr.op), pkt.hdr.id, level);
		flood_pkt_send(exclude_from_and_glevel_and_closed,
				upper_level, -1, real_from_rpos, pkt);
	}
finish:
	if(old_bblock)
		xfree(old_bblock);
	return ret;
}

int qspn_open(PACKET rpkt)
{
	PACKET pkt;
	brdcast_hdr  *bcast_hdr;
	tracer_hdr   *trcr_hdr;
	tracer_chunk *tracer;
	bnode_hdr    *bhdr=0;
	struct qspn_buffer *qb=0;
	int not_opened=0, ret=0, reply, sub_id, ret_err;
	u_short hops;
	size_t bblock_sz=0, old_bblock_sz;
	u_short old_bchunks=0;
	const char *ntop;
	char *old_bblock=0;
	char do_real_qspn_action=0, just_forward_it=0, int_qspn_opener=0;
	char all_bnodes_are_opened=0;

	map_node *from, *root_node, *tracer_starter;
	quadro_group rip_quadg;
	int gid, root_node_pos, real_from_rpos;
	u_char level, upper_level, blevel;

	
	/*
	 * * Unpack the qspn pkt and split it * *
	 */
	ret_err=qspn_unpack_pkt(rpkt, &bcast_hdr, &trcr_hdr, &tracer, &bhdr,
			&bblock_sz, &rip_quadg, &real_from_rpos,
			&hops, &upper_level, &gid,
			&from, &root_node,
			&tracer_starter, &sub_id,
			&root_node_pos, &level, &blevel,
			&just_forward_it, &do_real_qspn_action);
	if(ret_err < 0) {
		ret = -1;
		goto finish;
	}

#ifdef DEBUG
	debug(DBG_INSANE, "QSPN_OPEN(0x%x, lvl %d): node[0]: %d, node[1]: %d, hops: %d", 
			rpkt.hdr.id, level, tracer[0].node, 
			trcr_hdr->hops > 1 ? tracer[1].node : -1 ,
			trcr_hdr->hops);
#endif


	/*
	 *   * * Verify the qspn_open pkt * *
	 */
	
	if( ( !level || (do_real_qspn_action && 
					(root_node->flags & QSPN_OPENER)) ) 
			&& sub_id == root_node_pos) {
		ntop=inet_to_str(rpkt.from);
		debug(DBG_NOISE, "qspn_open(0x%x): Dropped qspn_open from "
				"%s: we are the qspn_starter of that pkt!"
				" (hops: %d)", rpkt.hdr.id, ntop,
				trcr_hdr->hops);
		ret=-1;
		goto finish;
	}

	if(rpkt.hdr.id < me.cur_qspn_id[level]) {
		ntop=inet_to_str(rpkt.from);
		debug(DBG_NOISE, "qspn_open(): %s sent a qspn_open"
				" with a wrong qspn_id (0x%x), cur_id: 0x%x", 
				ntop, rpkt.hdr.id, me.cur_qspn_id[level]);
		ret=-1;
		goto finish;
	}

	
	/* Some bnode, which is in the same gnode where we are, sent a
	 * qspn_open, so we are a qspn_opener too */
	if(level && sub_id == root_node_pos && hops == 1 && 
			do_real_qspn_action) {
		root_node->flags|=QSPN_OPENER;

		/* This flag indicates that the new qspn_open we received was
		 * sent from our gnode, so it is an internal qspn opener.*/
		int_qspn_opener=1;
	}

	/* We have only to forward it */
	if(level && from == root_node)
		just_forward_it=1;

	
	/*Time to update our map*/
	tracer_store_pkt(rpkt.from, &rip_quadg, level, trcr_hdr, tracer, 
			(void *)bhdr, bblock_sz, &old_bchunks, &old_bblock,
			&old_bblock_sz);
	
	
	if(bcast_hdr->flags & QSPN_BNODE_OPENED) {
		if(from == root_node) {
			/* 
			 * This pkt passed through a bnode which has all its 
			 * links opened. Increment the counter.
			 */
			me.bmap_nodes_opened[blevel]++;
		} else 
			bcast_hdr->flags &= ~QSPN_BNODE_OPENED;
	}

	if(!level || me.bmap_nodes_opened[blevel] >= (me.bmap_nodes[blevel]-1))
		all_bnodes_are_opened=1;

	not_opened=0;
	if(do_real_qspn_action && !just_forward_it) {
		/* 
		 * We search in the qspn_buffer the reply which has current
		 * sub_id.  If we don't find it, we add it.
		 */
		qb=qspn_b[level];
		if(!qb) {
			debug(DBG_NOISE, "There isn't qspn_buffer information"
					" for the %d level", level);
			
			
			ret=-1;
			goto finish;
		}
		
		if((reply=qspn_b_find_reply(qb, sub_id)) == -1)
			list_for(qb)
				reply=qspn_b_add(qb, sub_id, 0);


		/* Time to open the links */
		qb=qspn_b[level];
		list_for(qb) {
			if(qb->rnode == from)
				qb->flags[reply]|=QSPN_OPENED;

			if(!(qb->flags[reply] & QSPN_OPENED))
				not_opened++;
		}

		/* 
		 * If we have the links opened and we are in level > 0, set
		 * the flags to let the other bnodes know.
		 */
		if(!not_opened && level && !(root_node->flags & QSPN_OPENED)){
			bcast_hdr->flags|=QSPN_BNODE_OPENED;
			root_node->flags|=QSPN_OPENED;
		}

		
		/*Fokke, we've all the links opened. let's take a rest.*/
		if(!not_opened && all_bnodes_are_opened) {
			debug(DBG_NOISE, "qspn_open(0x%x, sub_id: %d) lvl %d: "
					"The qspn_open phase is finished",
					rpkt.hdr.id, sub_id, level);
			if(level && !(me.bmap_nodes[blevel]-1)) {
				/* 
				 * If in this `level' we are the only bnode,
				 * we need to broadcast the qspn_open to the
				 * other nodes in this gnode, to let them
				 * store the qspn_open's entries. So don't
				 * go to finish;
				 */
				debug(DBG_INSANE, "Propagating the last qspn_open");
				do_nothing();
			} else
				goto finish;
		}

	   	/* The forge of the packet. "One pkt to rule them all". Dum dum */
		ret_err=tracer_pkt_build(QSPN_OPEN, rpkt.hdr.id, bcast_hdr->sub_id,/*IDs*/
			    gid, 	 level,
			    bcast_hdr,   trcr_hdr, tracer, 	      /*Received tracer_pkt*/
			    old_bchunks, old_bblock, old_bblock_sz,   /*bnode_block*/
			    &pkt);				      /*Where the pkt is built*/
		if(ret_err) {
			debug(DBG_NOISE, "Cannot forward the qspn_open(0x%x) "
					"lvl %d sub_id: %d: Pkt build failed.",
					 rpkt.hdr.id, level, sub_id);
			ret=-1; 
			goto finish;
		}
	} else {
		/* 
		 * Increment the rtt of the last gnode chunk, because we
		 * aren't adding any entry, but we are just forwarding it.
		 */
		debug(DBG_INSANE, "qspn_close: Incrementing the last hops rtt.");
		ret_err=tracer_add_rtt(real_from_rpos, tracer, hops-1);
		if(ret_err < 0)
			debug(DBG_NOISE, "tracer_add_rtt(0x%x) hop %d failed",
					rpkt.hdr.id, hops-1);
		
		/* the pkt we're sending is a copy of the received one */
		pkt_copy(&pkt, &rpkt);
		pkt_clear(&pkt);
	}


	/*
	 * * Forward the new pkt * *
	 */
	
	debug(DBG_INSANE, "%s(0x%x) lvl %d to broadcast",
			rq_to_str(pkt.hdr.op), pkt.hdr.id, level);

	if(do_real_qspn_action && !int_qspn_opener) {
		flood_pkt_send(exclude_from_and_opened_and_glevel,
				upper_level, sub_id, real_from_rpos, pkt);
	} else {
		/* Just forward it without caring of opened or not rnodes */
		flood_pkt_send(exclude_from_and_glevel, upper_level, 
				sub_id, real_from_rpos, pkt);
	}
	
finish:
	if(old_bblock)
		xfree(old_bblock);
	return ret;
}
