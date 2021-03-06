##
# This file is part of Netsukuku
# (c) Copyright 2007 Andrea Lo Pumo aka AlpT <alpt@freaknet.org>
#
# This source code is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as published
# by the Free Software Foundation; either version 2 of the License,
# or (at your option) any later version.
#
# This source code is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# Please refer to the GNU Public License for more details.
#
# You should have received a copy of the GNU Public License along with
# this source code; if not, write to:
# Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
##
#
# Implementation of the P2P Over Ntk RFC. See {-P2PNtk-}
#

from ntk.core.map import Map
from ntk.lib.event import Event
from ntk.lib.micro import microfunc
from ntk.lib.rencode import serializable
from ntk.lib.rpc import FakeRmt, RPCDispatcher, CallerInfo

class P2PError(Exception):
    '''Generic P2P Error'''

class ParticipantNode(object):
    def __init__(self,
                 lvl=None, id=None,  # these are mandatory for Map.__init__()
                 participant=False):

        self.participant = participant

    def _pack(self):
        return (0, 0, self.participant)

serializable.register(ParticipantNode)

class MapP2P(Map):
    """Map of the participant nodes"""

    def __init__(self, levels, gsize, me, pid):
        """levels, gsize, me: the same of Map

        pid: P2P id of the service associated to this map
        """

        Map.__init__(self, levels, gsize, ParticipantNode, me)

        self.pid = pid

    def participate(self):
        """Set self.me to be a participant node."""

        for l in xrange(self.levels):
            self.node_get(l, self.me[l]).participant = True

    @microfunc()
    def me_changed(self, old_me, new_me):
        '''Changes self.me

        :param old_me: my old nip (not used in MapP2P)
        :param new_me: new nip
        '''
        Map.me_change(self, new_me)

    @microfunc(True)
    def node_del(self, lvl, id):
        Map.node_del(self, lvl, id)

class P2P(RPCDispatcher):
    """This is the class that must be inherited to create a P2P module.
    """

    def __init__(self, radar, maproute, pid):
        """radar, maproute: the instances of the relative modules

           pid: P2P id of the service associated to this map
        """

        self.radar = radar
        self.neigh = radar.neigh
        self.maproute = maproute

        self.mapp2p = MapP2P(self.maproute.levels,
                             self.maproute.gsize,
                             self.maproute.me,
                             pid)

        self.maproute.events.listen('ME_CHANGED', self.mapp2p.me_changed)
        self.maproute.events.listen('NODE_DELETED', self.mapp2p.node_del)

        # are we a participant?
        self.participant = False

        self.remotable_funcs = [self.participant_add, self.msg_send]
        RPCDispatcher.__init__(self, root_instance=self)

    def h(self, key):
        """This is the function h:KEY-->IP.

        You should override it with your own mapping function.
        """
        return key

    def H(self, IP):
        """This is the function that maps each IP to an existent hash node IP

        If there are no participants, None is returned
        """

        mp = self.mapp2p
        hIP = [None] * mp.levels
        for l in reversed(xrange(mp.levels)):
            for id in xrange(mp.gsize):
                for sign in [-1, 1]:
                    hid = (IP[l] + id * sign) % mp.gsize
                    if mp.node_get(l, hid).participant:
                        hIP[l] = hid
                        break
                if hIP[l]:
                    break
            if hIP[l] is None:
                return None

            if hIP[l] != mp.me[l]:
                # we can stop here
                break

        return hIP

    def neigh_get(self, hip):
        """Returns the Neigh instance of the neighbour we must use to reach
           the hash node.

        `hip' is the IP of the hash node.
        If nothing is found, None is returned
        """

        lvl = self.mapp2p.nip_cmp(hip, self.maproute.me)
        br = self.maproute.node_get(lvl, hip[lvl]).best_route()
        if not br:
            return None
        return self.neigh.id_to_neigh(br.gw)

    def participate(self):
        """Let's become a participant node"""

        self.mapp2p.participate()

        for nr in self.neigh.neigh_list():
            nr.ntkd.p2p.participant_add(self.maproute.pid, self.maproute.me)

    def participant_add(self, pIP):
        '''Add a participant node to the P2P service

        :param pIP: participant node's Netsukuku IP (nip)
        '''
        continue_to_forward = False

        mp  = self.mapp2p
        lvl = self.maproute.nip_cmp(pIP, mp.me)
        for l in xrange(lvl, mp.levels):
            if not mp.node_get(l, pIP[l]).participant:
                mp.node_get(l, pIP[l]).participant = True
                mp.node_add(l, pIP[l])
                continue_to_forward = True

        if not continue_to_forward:
            return

        # continue to advertise the new participant
        for nr in self.neigh.neigh_list():
            nr.ntkd.p2p.participant_add(self.pid, pIP) # ntkd.p2p is a P2PAll
                                                       # instance so we must
                                                       # pass also the P2P pid

    def msg_send(self, sender_nip, hip, msg):
        """Routes a packet to `hip'. Do not use this function directly, use
        self.peer() instead

        msg: it is a (func_name, args) pair."""

        hip = self.H(hip)
        if hip == self.mapp2p.me:
            # the msg has arrived
            return self.msg_exec(sender_nip, msg)

        # forward the message until it arrives at destination
        n = self.neigh_get(hip)
        if n:
            exec("return n.ntkd.p2p.PID_" + str(self.mapp2p.pid) +
                 ".msg_send(sender_nip, hip, msg)")
        else:
            return None

    def msg_exec(self, sender_nip, msg):
        return self.dispatch(CallerInfo(), *msg)

    class RmtPeer(FakeRmt):
        def __init__(self, p2p, hIP=None, key=None):
            self.p2p = p2p
            self.key = key
            self.hIP = hIP
            FakeRmt.__init__(self)

        def rmt(self, func_name, *params):
            """Overrides FakeRmt.rmt()"""
            if self.hIP == None:
                self.hIP = self.p2p.H(self.p2p.h(self.key))
            return self.p2p.msg_send(self.p2p.maproute.me,
                                     self.hIP,
                                     (func_name, params))

    def peer(self, hIP=None, key=None):
        if hIP is None and key is None:
            raise P2PError("hIP and key are both None. Specify at least one")
        return self.RmtPeer(self, hIP=hIP, key=key)

class P2PAll(object):
    """Class of all the registered P2P services"""

    __slots__ = ['radar',
                 'neigh',
                 'maproute',
                 'service',
                 'remotable_funcs',
                 'events']

    def __init__(self, radar, maproute):
        self.radar = radar
        self.neigh = radar.neigh
        self.maproute = maproute

        self.service = {}

        self.remotable_funcs = [self.pid_getall]
        self.events = Event(['P2P_HOOKED'])

    def listen_hook_ev(self, hook):
        hook.events.listen('HOOKED', self.p2p_hook)

    def pid_add(self, pid):
        self.service[pid] = P2P(self.radar, self.maproute, pid)
        return self.service[pid]

    def pid_del(self, pid):
        if pid in self.service:
            del self.service[pid]

    def pid_get(self, pid):
        if pid not in self.service:
            return self.pid_add(pid)
        else:
            return self.service[pid]

    def pid_getall(self):
        return [(s, self.service[s].mapp2p.map_data_pack())
                    for s in self.service]

    def p2p_register(self, p2p):
        """Used to add for the first time a P2P instance of a module in the
           P2PAll dictionary."""

        # It's possible that the stub P2P instance `self.pid_get(p2p.pid)'
        # created by pid_add() has an update map of participants, which has
        # been accumulated during the time. Copy this map in the `p2p'
        # instance to be sure.
        map_pack = self.pid_get(p2p.pid).mapp2p.map_data_pack()
        p2p.mapp2p.map_data_merge(map_pack)
        self.service[p2p.pid] = p2p

    def participant_add(self, pid, pIP):
        self.pid_get(pid).participant_add(pIP)

    @microfunc()
    def p2p_hook(self, *args):
        """P2P hooking procedure

        It gets the P2P maps from our nearest neighbour"""

        ## Find our nearest neighbour
        minlvl = self.maproute.levels
        minnr = None
        for nr in self.neigh.neigh_list():
            lvl = self.maproute.nip_cmp(self.maproute.me,
                                        self.maproute.ip_to_nip(nr.ip))
            if lvl < minlvl:
                minlvl = lvl
                minnr  = nr
        ##

        if minnr == None:
            # nothing to do
            return

        nrmaps_pack = minnr.ntkd.p2p.pid_getall()
        for (pid, map_pack) in nrmaps_pack:
            self.pid_get(pid).mapp2p.map_data_merge(map_pack)

        for s in self.service:
            if self.service[s].participant:
                self.service[s].participate()

        self.events.send('P2P_HOOKED', ())

    def __getattr__(self, str):

        if str[:4] == "PID_":
            return self.pid_get(int(str[4:]))
        raise AttributeError
