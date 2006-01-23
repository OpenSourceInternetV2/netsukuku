	         /**************************************
	        *     AUTHOR: Federico Tomassini        *
	       *     Copyright (C) Federico Tomassini    *
	      *     Contact effetom@gmail.com             *
	     ***********************************************
	     *******                                ********
*************************************************************************
*                                              				* 
*  This program is free software; you can redistribute it and/or modify	*
*  it under the terms of the GNU General Public License as published by	*
*  the Free Software Foundation; either version 2 of the License, or	*
*  (at your option) any later version.					*
*									*
*  This program is distributed in the hope that it will be useful,	*
*  but WITHOUT ANY WARRANTY; without even the implied warranty of	*
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the	*
*  GNU General Public License for more details.				*
*									*
************************************************************************/


#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* Official includes*/
#include "andns.h"
#include "andns_mem.h"
#include "andns_pkt.h"
#include "xmalloc.h"
#include "log.h"

#include <string.h>

#include <resolv.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h> 

#define LOOPBACK(x)     (((x) & htonl(0xff000000)) == htonl(0x7f000000)) 

/* Debug includes: DO NOT REMOVE 
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "andns.h"
#include "andns_mem.h"
#include "andns_pkt.h"
#include "andna_fake.h"

#include <string.h>

#include <resolv.h>
#include <netinet/in.h>
#include <sys/socket.h>
#define LOOPBACK(x)     (((x) & htonl(0xff000000)) == htonl(0x7f000000))*/





/*
int andns_pkt_init(int restricted)
{
	int n,e,count=0;
	struct in_addr ai;
	struct sockaddr_in ns[MAXNS],*nsnow; // MAXNS is defined in resolv.h
	char myaddr[INET_ADDRSTRLEN], ns_str[MAXNS*(INET_ADDRSTRLEN+2)]="\0";

	_default_realm_=(restricted)?INET_REALM:NTK_REALM;

	for (n=0;n<MAXNS;n++)
		memset(ns+n,0,sizeof(struct sockaddr_in));
	
	if((n=res_init())) {
		error(ERROR_MSG "res_init call failed :(", ERROR_POS);
		return -1;
	}
	
	if (_res.nscount==0)
		goto no_nsservers;

	for (n=0;n<_res.nscount;n++)
	{
		nsnow=_res.nsaddr_list+n;
		ai=nsnow->sin_addr;
		if (!LOOPBACK(ai.s_addr))
		{
			memcpy(ns+count,nsnow,sizeof(struct sockaddr_in));
			count++;
		}
	}
	if (!count)
		goto no_nsservers;
	
	for (n=e=0;n<MAXNS;n++)
	{
		memcpy(_res.nsaddr_list+n,ns+n,sizeof(struct sockaddr_in));
		if ((ns+n)->sin_addr.s_addr!=0) 
		{
                        inet_ntop(AF_INET,(const void*)(&((ns+n)->sin_addr)),myaddr,INET_ADDRSTRLEN);
			if(e)
				strncat(ns_str, ", ", 2);
			strncat(ns_str, myaddr, INET_ADDRSTRLEN);
			e++;
		}
	}
	if(e)
		loginfo("DNS Query inet related will be forwarded to: %s", ns_str);
			
	_dns_forwarding_=1;
	return 0;

no_nsservers:
	
	debug(DBG_NORMAL, "andns_init: nameservers not found. dns forwarding disable.");
	_dns_forwarding_=0;
	return 0;
}*/

/*
 * Takes a label: is there a ptr?
 * Returns:
 *  -1 is a malformed label is finded
 *  0 if there's no pointer
 *  <offset from start_pkt> if a pointer is finded
 */
size_t getlblptr(char *buf)
{
        uint16_t dlbl;
	char c[2];

	memcpy(c,buf,2);

        if (!LBL_PTR(*c)) /* No ptr */ return 0;
        if (LBL_PTR(*c)!=LBL_PTR_MK)
        {
                error("In getlblptr: label sequence malformed");
                return -1;
        }
	(*c)&=0x3f;
        memcpy(&dlbl,c,2);
	dlbl=ntohs(dlbl);
        return dlbl; // offset
}

int read_label_octet(const char *src,char *dst,int read_yet,int limit_len)
{
	int how;

	how=*src;
	if ( how+read_yet> limit_len || how+read_yet > MAX_SQLBL_LEN ) {
		error("In read_label_octet: exceeding pkt. limit_len=%d,read_yet=%d,count=%d",limit_len,read_yet,how);
		return -1;
	}
	memcpy(dst,src+1,how);
	return how;
}

/*
 * The next function is a little complex.
 * It converts a hname from sequence_label format to str.
 * Return bytes read. -1 on error, 1 (=len('\0')) on NULL-name
 *
 * -limit_len- is the maximum allowed space for hname.
 * it depends on pkt_len and others. Anyway, it's a limit that
 * i have to know.
 *
 * -buf- is the buffer to be parsed
 *
 * -start_pkt is the begin of pkt; it's useful: label-pointers
 * store offset from this point.
 *
 * -dst- is the destiny ~.~
 *
 * -count- is a counter: it shows how many bytes are readed yet..
 * It's useful bcz this function could recall itself, and it has to
 * remember the number of bytes readed.
 *
 * Tis function could go into recursion: in such case, recursion takes trace
 * about the recursion deep: more recursione than MAX_RECURSION_PTR are forbidded.
 *
 * Anyway,
 *      ***you have to call it with start_pkt=begin_pkt and count=0***
 */
size_t lbltoname(char *buf,char *start_pkt,char *dst,int count,int limit_len,int recursion)
{
        size_t temp,offset;

	/* controls the pkt size */
	if (count>limit_len) {
		error("In lbltoname: exceeding pkt.");
		return -1;
	}
	/* maybe we are at the last label octet */	
	if (*buf==0) {
		*dst=0;
		return 1;
	}
	/* maybe ther's a ptr: in such case, recursion is setted and temp
	 * stores offset from start_pkt */
	if ((temp=getlblptr(buf))) {
		if (temp==-1 || recursion>MAX_RECURSION_PTR) {
			error("In lbltoname: malformed pkt");
                        return -1;
		}
		recursion++;
		buf=start_pkt+temp;
	}
	else {
		if ((temp=read_label_octet(buf,dst,count,limit_len))==-1) 
			return -1;
		count+=temp+1; /* read also "." */
		buf+=temp+1;
		dst+=temp;
		if (*buf) 
			*dst++='.';
		else {
			*dst++=0;
			count++;
		}
	}
	if ((offset=lbltoname(buf,start_pkt,dst,count,limit_len,recursion))==-1)
		return -1;
	if (recursion) 
		return (recursion==1)?2:0;
	return offset+temp+1;
}

/*


        while (*buf) // This controls buf-format
        {
                // This is the best part of the trip
                if ((temp=getlblptr(buf)))
                {
                        // Malformed ptr or re-recursion mode
                        if (temp==-1 || recursion>MAX_RECURSION_PTR) 
			{
				error("In lbltoname: malformed pkt");
                                return -1;
			}
                        // now, count is set to 0 or not.
                        // Doesn't matter! count updates itself.
                        // Start the recursion with recursion+1
                        if((offset=lbltoname(start_pkt+temp,start_pkt,dst,count,limit_len,recursion+1))==-1) {
				error("In lbltoname: error recursion.");
				printf("In lbltoname: error recursion.\n");
                                return -1;
			}
			printf("Recursion is %d. count is %d (+2)\n",recursion,count);
			if (recursion) return 0; // do not procede with pkt lecture.
			return count+2;
                }
		printf("AHIAHI %s\n",buf);
		if ((temp=read_label_octet(buf,dst,count,limit_len))==-1) 
			return -1;
		count+=temp+1; 
		buf+=temp+1;
		dst+=temp;
				  
                temp=(size_t)(*buf++);
                 Update count and control dom_name_len
                count+=temp+1; // +1 to include "." (i.e. *buf octet)
                if (count > MAX_HNAME_LEN || count > limit_len)
                {
                        debug(DBG_NOISE, "In name_from_label: dom name too long or exceding packet");
			printf("In name_from_label: dom name too long or exceding packet\n");
                        return -1;
                }
                // It's ok to cp
                memcpy(dst,buf,temp); 
                dst+=temp;
                buf+=temp; 
		if (*buf==0) {  Lecture finish 
			*dst=0;
			count++;
			return count;
		}  COntinue, but without recursion set
		if ((temp=lbltoname(buf,start_pkt,dst,count,limit_len,0))==-1) {
			error("In lbltoname: error recursion.");
			printf("In lbltoname: error recursion.\n");
                               return -1;
		}
		return count+temp;

                if (*buf) *dst++='.';
                // And now, to next label
        }
        // ADD "/0" to dom_name
        *dst=0;
        count++;
	printf("lbltoname: readed %d bytes, recursion=%d\n",count,recursion);
        return count; */
//}*/
/*
 * Returns the used protocol which packet belongs,
 * understanding it from NK bit in pkt headers.
 * -1 on error
 */
int andns_proto(char *buf)
{
        char c;
	
	c=*(buf+3);
	c=(c>>4)&0x03;
	return c;
/*        if (c==NK_NTK || c==NK_INET)
                return ANDNS_NTK_PROTO;
        if (c==NK_OLDSTYLE)
                return ANDNS_DNS_PROTO;
        error("In andns_proto: query protocol?!?!?");
        return -1;*/
}

/*
 * Returns the question relative realm.
 * INET_REALM if you are seraching something on internet.
 * NTK_REALM if you search something on ntk.
 * -1 on error
 *  _default_realm_ is initialized in andns_init: if 
 *  no realm is specified in the query, 
 *
 *  prefixed is set to 1 if a REALM_PREFIX is found.
 *
 *  Note: in the case of a ptr_query, the suffix-realm has
 *  to be specified if you want a different behavior fron 
 *  the default.
 *  Do you want to know who is 1.2.3.4 in INET_REALM?
 *  ASk for 1.2.3.4.INT
 */
int andns_realm(dns_pkt_qst *dpq,int *prefixed)
{
	int slen;
	char *qst;

	qst=dpq->qname;

	if (!qst) 
	{
		error("In andns_realm: what appens?");
		return -1;	
	}
	slen=strlen(qst);
	if (slen<5) return _default_realm_;

	if (dpq->qtype==T_PTR) {
		if (strcasestr(qst,PTR_INET_REALM_PREFIX)==qst) {
			if (prefixed) *prefixed=1;
			return INET_REALM;
		}
		if (strcasestr(qst,PTR_NTK_REALM_PREFIX)==qst) {
			if (prefixed) *prefixed=1;
			return NTK_REALM;
		}
		if (prefixed) *prefixed=0;
		return _default_realm_;
	}
		
	// if qst is tto short, it's impossible to 
	// consider a prefix.
	if (strcasestr(qst+slen-REALM_PREFIX_LEN,INET_REALM_PREFIX)) {
		if (prefixed) *prefixed=1;
		return INET_REALM;
	}
	if (strcasestr(qst+slen-REALM_PREFIX_LEN,NTK_REALM_PREFIX)) {
		if (prefixed) *prefixed=1;
		return NTK_REALM;
	}
	if (prefixed) *prefixed=0;
	return _default_realm_;
}
int is_prefixed(dns_pkt *dp)
{
	int prefix=0;

	andns_realm(dp->pkt_qst,&prefix);
	return prefix;
}
	
/*
 * Remove the suffix realm
 */
char* rm_realm_prefix(char *from,char *dst,int type)
{
        int slen;
        slen=strlen(from);

	if (slen<5) 
		strcpy(dst,from);
	else if (type==T_PTR) {
		if (strcasestr(from,PTR_INET_REALM_PREFIX)==from || 
		    strcasestr(from,PTR_NTK_REALM_PREFIX)==from) 
			strcpy(dst,from+REALM_PREFIX_LEN);
		else 
			strcpy(dst,from);
		
	}
	else if (strcasestr(from+REALM_PREFIX_LEN,INET_REALM_PREFIX) || 
		 strcasestr(from+REALM_PREFIX_LEN,NTK_REALM_PREFIX)) 
			strncpy(dst,from,slen-REALM_PREFIX_LEN);
	else
		strcpy(dst,from);
	return dst;
	/*	
			

        if ((crow=strcasestr(from,INET_REALM_PREFIX)) || \
                (crow=strcasestr(from,NTK_REALM_PREFIX)))
        {
                if (crow==from) 
                        strcpy(dst,from+REALM_PREFIX_LEN);
		else
                	strncpy(dst,from,slen-REALM_PREFIX_LEN);
                return dst;
        }
	strcpy(dst,from);
        return dst;*/

}
			
/*
 * Returns a ip-stringed from an inverse string:
 * 4.3.2.1-IN-ADDR.ARPA becomes 1.2.3.4
 * blA.blU.IP6.ARPA becomes blU.blA
 * family is ipv4 or ipv6
 * NULL on error.
 */
char* swapped_straddr(char *src)
{
        char *temp,*temp1,*dst;
	int family;

	if (!src)
	{
		error("In swapped_straddr: NULL argument!");
		return NULL;
	}
	// If I cannot find any inverse prefix
        if( ! \
		( (temp=(char*)strcasestr(src,DNS_INV_PREFIX))  ||\
		  (temp=(char*)strcasestr(src,DNS_INV_PREFIX6)) ||\
		  (temp=(char*)strcasestr(src,OLD_DNS_INV_PREFIX6))))
	{
		error("In swapped_straddr: ptr query without suffix");
		return NULL;
	}
	// IPV4 or IPV6?
	family=(strcasestr(temp,"6"))?AF_INET6:AF_INET;
	// If a prefix for inet realm is found, discard the prefix 
	// part
	if ( (temp1=(char*)strcasestr(src,INET_REALM_PREFIX)))
		src+=strlen(INET_REALM_PREFIX);

        dst=xmalloc((size_t)(temp-src));
        temp--;
        if (family==AF_INET)
        {
                int count=0;
                while (temp!=src && *temp--!='.' && ++count)
                        if (*temp=='.')
                        {
                                strncpy(dst,temp+1,count);
                                dst+=count;
                                *dst++='.';
                                count=0;
                                temp--;
                        }
                strncpy(dst,src,++count);
                dst+=count;
                *dst='\0';
        }
        else while ((*temp--=*src++));
        return dst;
}
/*
 * This function controls the integrity of a dns_pkt
 * in the case of a query ntk-related: in such case,
 * not alls dns-implementations are permitted.
 *
 * Returns 0 if OK. -1 if not.
 * If -1 is returned, the rcode has to be setted to 
 * E_NIMPL
 */

int dnslovesntk(dns_pkt *dp)
{
        dns_pkt_hdr *dph;

        dph=&(dp->pkt_hdr);
        if (dph->qr || dph->aa || dph->tc || dph->z)
        {
                error("In dnslovesntk: pkt_hdr with QR || AA || TC || Z");
                return -1; // We acept only queries, not answers o pkt trunc
        }                  // Z has to be 0 (see rfc)

        if (dph->opcode>=2)
        { // No Status server or unuesed values
                error("In dnslovesntk: opcode not supported in ntk realm");
                return -1;
        }
        if (DP_QDCOUNT(dp)!=1)
        {
                error("In dnslovesntk: i need one and only one query");
                return -1; // Only a query must exist.
        }
        if (dph->arcount || dph->ancount || dph->nscount)
        {
                error("In dnslovesntk: pkt has AN || NS || AR sections");
                return -1; // No Answ, Auth, Adds
        }
        /* Pkt_hdR controls ended */
	return 0;
}
/*
 * Converts a domain_name_string into a sequence label format,
 * dns compliant. Writes on dst.
 * -1 on error, number of bytes writed on succes
 */
size_t nametolbl(char *name,char *dst)
{
        char *crow;
        size_t offset=0,res;

        if (!name || !strcmp(name,"") || strlen(name)>MAX_HNAME_LEN)
        {
                error("In nametolbl: invalid name");
                return -1;
        }
        while ((crow=strstr(name+1,".")))
        {
                res=crow-name;
                if (res+offset>MAX_SQLBL_LEN)
                {
                        error("In nametolbl: sequence label too long");
                        return -1;
                }
                *dst=(char)res; // write the octet length
		dst++;
		offset++;
                memcpy(dst,name,(size_t)res); // write label
                name+=res+1;dst+=res;offset+=res; // shift ptrs
        }
	if (!name) return offset;
	if((res=(char)strlen(name))>MAX_SQLBL_LEN)
        {
                error("In nametolbl: sequence label too long");
                return -1;
        }
	*dst++=(char)res;
	strcpy(dst,name);
	offset+=res+2;
	/*
	dst++;
        while ( (*dst++=*name++)) offset++;
	*dst++=*name++;
	offset++;*/
        return offset;
}
/*
 * Disassembles DNS packet headers, writing a yet allocated
 * dns_pkt_hdr struct.
 * No controls on len, bcz <<--the min_pkt_len is controlled
 * by recv.-->>
 * Returns the number of bytes readed (always DNS_PKT_HDR_SZ).
 */
size_t dpkttohdr(char *buf,dns_pkt_hdr *dph)
{
        uint8_t c;
        uint16_t s;

                // ROW 1
        memcpy(&s,buf,sizeof(uint16_t));
        dph->id=ntohs(s);
                // ROW 2
        buf+=2;
        memcpy(&c,buf,sizeof(uint8_t));
        dph->qr= (c>>7)&0x01;
        dph->opcode=(c>>3)&0x0f;
        dph->aa=(c>>2)&0x01;
        dph->tc=(c>>1)&0x01;
        dph->rd=c&0x01;

        buf++;
        memcpy(&c,buf,sizeof(uint8_t));
	dph->ra=(c>>7)&0x01;
        dph->z=(c>>4)&0x07;
        dph->rcode=c&0x0f;

                // ROW 3
        buf++;
        memcpy(&s,buf,sizeof(uint16_t));
        dph->qdcount=ntohs(s);
                // ROW 4
        buf+=2;
        memcpy(&s,buf,sizeof(uint16_t));
        dph->ancount=ntohs(s);
                // ROW 5
        buf+=2;
        memcpy(&s,buf,sizeof(uint16_t));
        dph->nscount=ntohs(s);
                // ROW 6
        buf+=2;
        memcpy(&s,buf,sizeof(uint16_t));
        dph->arcount=ntohs(s);

        buf+=2;
        return DNS_HDR_SZ; // i.e. 12 :)
}
/*
 * This function alloc a new dns_pkt_qst to store a dns_question_section.
 * The new dns_pkt_qst is also added to the principal dp-struct
 * Returns bytes readed if OK. -1 otherwise.
 * 
 */
size_t dpkttoqst(char *start_buf,char *buf,dns_pkt *dp,int limit_len)
{
        size_t count;
        uint16_t s;
	dns_pkt_qst *dpq;

	dpq=dns_add_qst(dp);

        // get name
        if((count=lbltoname(buf,start_buf,dpq->qname,0,limit_len,0))==-1)
                return -1;
        buf+=count;
        // Now we have to write 2+2 bytes
        if (count+4>limit_len)
        {
                debug(DBG_NOISE, "In dpkttoqst: limit_len break!");
                return -1;
        }

        // shift to type and class
        memcpy(&s,buf,2);
        dpq->qtype=ntohs(s);
        count+=2;
        buf+=2;

        memcpy(&s,buf,2);
        dpq->qclass=ntohs(s);
        count+=2;

	rm_realm_prefix(dpq->qname,dpq->qname_nopref,dpq->qtype);
        return count;
}
/*
 * Disassembles a DNS qst_section_set.
 * Use the above function for each question section.
 * -1 on error. Number of bytes readed on success.
 *  If -1 is returned, rcode ha sto be set to E_INTRPRT
 */
size_t dpkttoqsts(char *start_buf,char *buf,dns_pkt *dp,int limit_len)
{
        size_t offset=0,res;
        int i,count;

        if (!(count=DP_QDCOUNT(dp)))
        	return 0; // No questions.

        for(i=0;i<count;i++)
        {
                if ( (res=dpkttoqst(start_buf,buf+offset,dp,limit_len-offset))==-1)
                        return -1;
                offset+=res;
        }
        return offset;
}

/*
 * The behavior of this function is in all similar to dpkttoqst.
 * Returns -1 on error. Bytes readed otherwise.
 */
size_t dpkttoa(char *start_buf,char *buf,dns_pkt_a **dpa_orig,int limit_len)
{
        size_t count,rdlen;
	dns_pkt_a *dpa;
        uint16_t s;
        uint32_t ui;

	dpa=dns_add_a(dpa_orig);

        // get name
        if((count=lbltoname(buf,start_buf,dpa->name,0,limit_len,0))==-1)
                return -1;
        buf+=count;
        // Now we have to write 2+2+4+2 bytes
        if (count+10>limit_len)
        {
                debug(DBG_NOISE, "In npkttoa: limit_len braek!");
                return -1;
        }

        memcpy(&s,buf,2);
        dpa->type=ntohs(s);
        count+=2;
        buf+=2;

        memcpy(&s,buf,2);
        dpa->class=ntohs(s);
        count+=2;
        buf+=2;

        memcpy(&ui,buf,4);
        dpa->ttl=ntohs(ui);
        count+=4;
        buf+=4;

        memcpy(&s,buf,2);
        dpa->rdlength=ntohs(s);
        count+=2;
        buf+=2;

        rdlen=dpa->rdlength;
        // Now we have to write dpa->rdlength bytes
        if (count+rdlen>limit_len || rdlen>MAX_HNAME_LEN)
        {
                debug(DBG_NOISE, "In npkttoa: limit_len break!");
                return -1;
        }
	if (dpa->type==T_A)
        	memcpy(dpa->rdata,buf,rdlen);
	else 
		if ((ui=lbltoname(buf,start_buf,dpa->rdata,0,MAX_HNAME_LEN,0))==-1) {
			error("In dpkttpa: can not write rdata field.");
			return -1;
		}
        count+=rdlen;
        return count;
}

/*
 * See dpkttoqsts.
 * -1 on error.  Bytes readed otherwise.
 */
size_t dpkttoas(char *start_buf,char *buf,dns_pkt_a **dpa,int limit_len,int count)
{
        size_t offset=0,res;
        int i;

        if (!count) return 0;
        for(i=0;i<count;i++)
        {
                if ((res=dpkttoa(start_buf,buf+offset,dpa,limit_len-offset))==-1)
                        return -1;
                offset+=res;
        }
        return offset;
}

/*
 * This is a main function: takes the pkt-buf and translate
 * it in structured data.
 * It cares about dns_pkt allocation.
 *
 * Returns:
 * -1 on E_INTRPRT
 *  0 if pkt must be discarded.
 *  Number of bytes readed otherwise
 */
size_t dpkt(char *buf,size_t pktlen,dns_pkt **dpp)
{
	dns_pkt *dp;
	size_t offset=0,res;
	char *crow;

	crow=buf;
	// Controls pkt consistency: we must at least read pkt headers
	if (pktlen<DNS_HDR_SZ)
		return 0; // pkt MUST be discarded!
	*dpp=dp=create_dns_pkt();

	// Writes headers
	offset+=dpkttohdr(buf,&(dp->pkt_hdr));
	if (pktlen > DNS_MAX_SZ) // If pkt is too long, the headers are written,
				// so we can reply with E_INTRPRT
		return -1;
	crow+=offset;
	// Writes qsts
	if ((res=dpkttoqsts(buf,crow,dp,pktlen-offset))==-1)
		return -1;
	offset+=res;
	crow+=res;
	if ((res=dpkttoas(buf,crow,&(dp->pkt_answ),pktlen-offset,DP_ANCOUNT(dp)))==-1)
		return -1;
	offset+=res;
	crow+=res;
	if ((res=dpkttoas(buf,crow,&(dp->pkt_auth),pktlen-offset,DP_NSCOUNT(dp)))==-1)
		return -1;
	offset+=res;
	crow+=res;
	if ((res=dpkttoas(buf,crow,&(dp->pkt_add),pktlen-offset,DP_ARCOUNT(dp)))==-1)
		return -1;
	return offset;
}
/*
 * This function is the dptktohdr inverse.
 * Takes a dns_pkt struct and builds the
 * header pkt-buffer
 * Returns the number of bytes writd.
 */
size_t hdrtodpkt(dns_pkt *dp,char *buf)
{
        char *crow=buf;
        uint16_t u;
        dns_pkt_hdr *dph;

        dph=&(dp->pkt_hdr);
        u=htons(dph->id);
        memcpy(buf,&u,2);
        buf+=2;

        if (dph->qr) *buf|=0x80;
        *buf|=dph->opcode<<3;
        *buf|=dph->aa<<2;
        *buf|=dph->tc<<1;
        *buf|=dph->rd;

        buf++;
        *buf|=dph->ra<<7;
        *buf|=dph->z<<4;
        *buf|=dph->rcode;

        buf++;

        u=htons(dph->qdcount);
        memcpy(buf,&u,2);
        buf+=2;
        u=htons(dph->ancount);
        memcpy(buf,&u,2);
        buf+=2;
        u=htons(dph->nscount);
        memcpy(buf,&u,2);
        buf+=2;
        u=htons(dph->arcount);
        memcpy(buf,&u,2);
        buf+=2;
        return (size_t)(buf-crow);
}
/*
 * Translate a struct dns_pkt_qst in the dns-buffer buf.
 * -1 On error. Bytes writed otherwise
 */
size_t qsttodpkt(dns_pkt_qst *dpq,char *buf, int limitlen,int nopref)
{
        size_t offset;
        uint16_t u;
	char *temp;

	temp=(nopref)?dpq->qname_nopref:dpq->qname;

        if((offset=nametolbl(temp,buf))==-1) {
		error("In qsttodpkt: error transalting name to sequence labels: name=%s",temp);
                return -1;
	}
        if (offset+4>limitlen)
        {
                error("In qsttodpkt: limitlen broken");
                return -1;
        }
        buf+=offset;
        u=htons(dpq->qtype);
        memcpy(buf,&u,2);
        buf+=2;offset+=2;
        u=htons(dpq->qclass);
        memcpy(buf,&u,2);
        buf+=2;offset+=2;
        return offset;
}
/*
 * Translates the question sections of a struct dns_pkt
 * into buf.
 * -1 on error.
 *  Number of bytes writed otherwise,
 */
size_t qststodpkt(dns_pkt *dp,char *buf,int limitlen,int nopref)
{
        size_t offset=0,res;
        int i;
	dns_pkt_qst *dpq;
	dpq=dp->pkt_qst;

        for (i=0;dpq && i<DP_QDCOUNT(dp);i++)
        {
                if ((res=qsttodpkt(dpq,buf+offset,limitlen-offset,nopref))==-1)
                        return -1;
                offset+=res;
		dpq=dpq->next;
        }
        return offset;
}
size_t atodpkt(dns_pkt_a *dpa,char *buf,int limitlen)
{
        size_t offset,rdlen;
        uint16_t u;
        int i;

        if((rdlen=nametolbl(dpa->name,buf))==-1)
                return -1;
	offset=rdlen;
	if (offset+10>limitlen)
        {
                error("In atodpkt: limitlen broken");
                return -1;
        }
        buf+=offset;
        u=htons(dpa->type);
        memcpy(buf,&u,2);
        buf+=2;offset+=2;
        u=htons(dpa->class);
        memcpy(buf,&u,2);
        buf+=2;offset+=2;
        i=htonl(dpa->ttl);
        memcpy(buf,&i,4);
        buf+=4;offset+=4;

	if (dpa->type==T_A) {
		if (offset+dpa->rdlength>limitlen) {
                	error("In atodpkt: limitlen broken");
	                return -1;
        	}
        	memcpy(buf+2,dpa->rdata,dpa->rdlength);
		offset+=dpa->rdlength;
	}
	else {
		if ((rdlen=nametolbl(dpa->rdata,buf+2))==-1) {
			error("In atodpkt: can not write rdata field.");
			return -1;
		}
		offset+=rdlen;
		if (offset>limitlen) {
                	error("In atodpkt: limitlen broken");
	                return -1;
        	}
		dpa->rdlength=rdlen;
	}
        u=htons(dpa->rdlength);
        memcpy(buf,&u,2);
        offset+=2;
        return offset;
}
size_t astodpkt(dns_pkt_a *dpa,char *buf,int limitlen,int count)
{
        size_t offset=0,res;
        int i;
        for (i=0;dpa && i<count;i++)
        {
                if ((res=atodpkt(dpa,buf+offset,limitlen-offset))==-1)
                        return -1;
                offset+=res;
		dpa=dpa->next;
        }
        return offset;
}
/*
 * Transform a dns_pkt structure in char stream.
 * On error, returns  -1
 * The stream has at least the header section writed.
 * `buf' must be at least of DNS_MAX_SZ bytes.
 *
 * nopref: do you want to pkt with name_realm_prefixed or not?
 * if nopref, name without prefix will be used
 *
 * DANGER: This function realeses *ALWAYS* the dns_pkt *dp!!!!
 * The stream buffer is allocated here.
 */
size_t dpktpack(dns_pkt *dp,char *buf,int nopref)
{
        size_t offset,res;

        memset(buf,0,DNS_MAX_SZ);

        offset=hdrtodpkt(dp,buf);
        buf+=offset;
        if((res=qststodpkt(dp,buf,DNS_MAX_SZ-offset,nopref))==-1)
                goto server_fail;
        offset+=res;
        buf+=res;
        if ( (res=astodpkt(dp->pkt_answ,buf,DNS_MAX_SZ-offset,DP_ANCOUNT(dp)))==-1)
		goto server_fail;
	offset+=res;
	/*buf+=res;
	if ( (res=astodpkt(dp->pkt_auth,buf,DNS_MAX_SZ-offset,DP_NSCOUNT(dp)))==-1) 
		goto server_fail;
	offset+=res;
	buf+=res;*/
        /*if ( (res=astodpkt(dp->pkt_add,buf,DNS_MAX_SZ-offset,DP_ARCOUNT(dp)))==-1)
		goto server_fail;
	offset+=res;*/
        destroy_dns_pkt(dp);
        return offset;
server_fail:
	destroy_dns_pkt(dp);
	return -1;
}
/*
 * Takes the buffer stream and translate headers to
 * andns_pkt struct. 
 * Returns ALWAYS 4. The pkt_len has to be controlled
 * elsewhere.
 */
size_t apkttohdr(char *buf,andns_pkt *ap)
{
        uint8_t c;
        uint16_t s;
        char *start_buf;

        start_buf=buf;

                // ROW 1
        memcpy(&s,buf,sizeof(uint16_t));
        ap->id=ntohs(s);
        buf+=2;

        memcpy(&c,buf,sizeof(uint8_t));
	ap->qr=(c>>7)&0x01;
	ap->qtype=(c>>3)&0x0f;
	ap->ancount=(c<<1)&0x0e;

        buf++;
	if (((*buf)|0x80)) ap->ancount++;

	ap->nk=(c>>4)&0x03;
	ap->rcode=c&0x0f;
        return ANDNS_HDR_SZ;
}
/*
 * Translate the andns_pkt question stream to andns_pkt struct.
 * -1 on error. Bytes readed otherwise.
 *  NOTE: The qst-data size is controlled: apkt won't be need
 *  this control.
 */
size_t apkttoqst(char *buf,andns_pkt *ap)
{
	uint16_t s;
	memcpy(&s,buf,sizeof(uint16_t));
	ap->qstlength=ntohs(s);
	if (ap->qstlength>=MAX_ANDNS_QST_LEN)
	{
		error("In apkttoqst: size exceeded");
		return -1;
	}
	buf+=2;
	memcpy(ap->qstdata,buf,ap->qstlength);
	rm_realm_prefix(ap->qstdata,ap->qstdata_nopref,ap->qtype);
	return ap->qstlength+2;
}

/*
 * This is a main function: takes the pkt-buf and translate
 * it in structured data.
 * It cares about andns_pkt allocation.
 * The apkt is allocate here.
 *
 * Returns:
 * -1 on E_INTRPRT
 *  0 if pkt must be discarded.
 *  Number of bytes readed otherwise
 */
size_t apkt(char *buf,size_t pktlen,andns_pkt **app)
{
	andns_pkt *ap;
	size_t offset,res;

	if (pktlen<ANDNS_HDR_SZ)
	{
		error("In apkt: pkt sz is less than pkt headers!");
		return 0;
	}
	*app=ap=create_andns_pkt();
	offset=apkttohdr(buf,ap);
	buf+=offset;
	if ((res=apkttoqst(buf,ap))==-1)
		return -1;
	return offset+res;
}

size_t hdrtoapkt(andns_pkt *ap,char *buf)
{
	uint16_t s;
	s=htons(ap->id);
	memcpy(buf,&s,sizeof(uint16_t));
	buf+=2;
	if (ap->qr) 
		(*buf)|=0x80;
	(*buf)|=( (ap->qtype)<<3);
	(*buf++)|=( (ap->ancount)>>1);
	(*buf)|=( (ap->ancount)<<7);
	(*buf)|=( (ap->nk)<<4);
	(*buf)|=(  ap->rcode);
	return ANDNS_HDR_SZ;
}
size_t qsttoapkt(andns_pkt *ap,char *buf,size_t limitlen)
{
	uint16_t s;

	if (ap->qstlength>MAX_ANDNS_QST_LEN || limitlen < ap->qstlength+2 )
	{
		error("In qsttooapkt: size exceeded");
		return -1;
	}
	s=htons(ap->qstlength);
	memcpy(buf,&s,sizeof(uint16_t));
	buf+=2;
	memcpy(buf,ap->qstdata,ap->qstlength);
	return ap->qstlength+2;
}
size_t answtoapkt(andns_pkt_data *apd,char *buf,size_t limitlen)
{
	uint16_t s;
	if (apd->rdlength>MAX_ANDNS_ANSW_LEN || limitlen< apd->rdlength+2)
	{
		error("In answtoapkt: size exceeded");
		return -1;
	}
	s=htons(apd->rdlength);
	memcpy(buf,&s,sizeof(uint16_t));
	buf+=2;
	memcpy(buf,apd->rdata,apd->rdlength);
	return apd->rdlength+2;
}
size_t answstoapkt(andns_pkt *ap,char *buf, size_t limitlen)
{
	andns_pkt_data *apd;
	int i;
	size_t offset=0,res;

	apd=ap->pkt_answ;
	for (i=0;i<AP_ANCOUNT(ap) && apd;i++)
	{
		if((res=answtoapkt(apd,buf+offset,limitlen-offset))==-1)
			return -1;
		offset+=res;
		apd=apd->next;
	}
	return offset;
}

/*
 * apktpack: `buf' must be at least of ANDNS_MAX_SZ bytes.
 */
size_t apktpack(andns_pkt *ap, char *buf)
{
	size_t offset,res;

	memset(buf,0,ANDNS_MAX_SZ);

	offset=hdrtoapkt(ap,buf);
	buf+=offset;
	if ((res=qsttoapkt(ap,buf,ANDNS_MAX_SZ-offset))==-1)
		goto server_fail;
	offset+=res;
	if ((res=answstoapkt(ap,buf,ANDNS_MAX_SZ-offset))==-1)
		goto server_fail;
	offset+=res;
	destroy_andns_pkt(ap);

	return offset;

server_fail:
	destroy_andns_pkt(ap);
	return -1;
	//AANSWFAIL(crow,offset);
	//return crow;
}
/*
char* apkttodstream(andns_pkt *ap)
{
	dns_pkt *dp;
	dns_pkt_qst *dpq;
	char *bufstream;

	dp=create_dns_pkt();
	memset(dp,0,DNS_PKT_SZ);

	//writes headers
	(dp->pkt_hdr).id=1;
	(dp->pkt_hdr).rd=1;
	(dp->pkt_hdr).qdcount=1;

	dpq=dns_add_qst(dp);
	dpq->qtype=ap->qtype;
	dpq->qclass=C_IN;
	strncpy(dpq->qname,ap->qstdata,MAX_HNAME_LEN);
	dpktpack(dp,bufstream);
	return bufstream;
}*/
	
/*
 * In the case of a query whith ntk-protocol, but inet-related,
 * the apkt must be translated o a dns_pkt and the dns_pkt has to be 
 * forwarded to some nameserver.
 * This function translate the answer fron ntk style to dns style.
 * Returns: -1 on error, 0 if OK
 */
int danswtoaansw(dns_pkt *dp,andns_pkt *ap,char *msg)
{
	int acount,i,type,res;
	andns_pkt_data *apd;
	dns_pkt_a *dpa;

	acount=DP_ANCOUNT(dp);

	dpa=dp->pkt_answ;
	for(i=0;i<acount;i++)
	{
		apd=andns_add_answ(ap);
		if (!dpa)
		{
			error("In danswtoaansw: ancount is %d, but answers are not.",acount); 
			return -1;
		}
		type=ap->qtype;
		switch(type)
		{
			case AT_A:
				apd->rdlength=4;
				memcpy(apd->rdata,dpa->rdata,4);
				break;
			case AT_PTR:
				if ((res=lbltoname(dpa->rdata,msg,apd->rdata,0,MAX_HNAME_LEN,0))==-1)
					return -1;
				apd->rdlength=strlen(apd->rdata);
				break;
			case AT_MX:
				if ((res=lbltoname(dpa->rdata,msg,apd->rdata,0,MAX_HNAME_LEN,0))==-1)
					return -1;
				apd->rdlength=strlen(apd->rdata);
				break;
			default:
				error("In danswtoaansw: qtype error");
				break;
		}
		dpa=dpa->next;
	}
	return 0;
}

				
				
		
		
	