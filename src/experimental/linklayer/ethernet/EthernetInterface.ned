//
// Copyright (C) 2004 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//



package inet.experimental.linklayer.ethernet;

import inet.linklayer.INetworkInterface;
import inet.networklayer.queue.OutputQueue;
import inet.linklayer.ethernet.EtherMAC;

//
// Ethernet network interface. Complements EtherMAC and EtherEncap
// with an output queue for QoS and RED support.
//
// @see EthernetInterface2, EthernetInterfaceNoQueue
//
module EthernetInterface like INetworkInterface
{
    parameters:
        string queueType = default("DropTailQueue");
        @display("i=block/ifcard");
    gates:
        input netwIn;
        output netwOut;
        inout phys @labels(EtherFrame);
        input wifiMeshIn @labels(Ieee80211Frame);   // to Ethernet MAC
        output wifiMeshOut @labels(Ieee80211Frame); // to Ethernet MAC
    submodules:
        queue: <queueType> like OutputQueue {
            parameters:
                @display("p=46,143;q=l2queue");
        }
        mac: EtherMAC {
            parameters:
                queueModule = "queue";
                txQueueLimit = 1; // queue sends one packet at a time
                @display("p=116,203");
        }
        encap: EtherEncapMesh {
            parameters:
                @display("p=116,94");
        }
    connections:
        // netwIn --> queue.in;
        netwIn --> encap.upperLayerIn;
        encap.lowerLayerOut --> queue.in;
        // queue.out --> encap.upperLayerIn;
        netwOut <-- { @display("m=n"); } <-- encap.upperLayerOut;
        //        encap.lowerLayerOut --> mac.upperLayerIn;
        queue.out --> mac.upperLayerIn;
        encap.lowerLayerIn <-- mac.upperLayerOut;
        mac.phys <--> phys;
        wifiMeshIn --> encap.wifiMeshIn;   // from wifi mesh
        wifiMeshOut <-- encap.wifiMeshOut; // to wifi mesh
}

