//
// Copyright (C) 2008 Alfonso Ariza
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//

#define CHEAT_IEEE80211MESH
#include "Ieee80211Mesh.h"
#include "MeshControlInfo_m.h"
#include "lwmpls_data.h"
#include "ControlInfoBreakLink_m.h"
#include "ControlManetRouting_m.h"
#include "OLSRpkt_m.h"
#include "dymo_msg_struct.h"
#include "aodv_msg_struct.h"
#include "InterfaceTableAccess.h"
#include "IPDatagram.h"
#include "IPv6Datagram.h"
#include "LinkStatePacket_m.h"
#include "MPLSPacket.h"
#include "ARPPacket_m.h"
#include "OSPFPacket_m.h"
#include <string.h>


/* WMPLS */

#define WLAN_MPLS_TIME_DELETE  6

#define WLAN_MPLS_TIME_REFRESH 3

#define _WLAN_BAD_PKT_TIME_ 30
#define MAX_ADDR_SIZE 30


#if !defined (UINT32_MAX)
#   define UINT32_MAX  4294967295UL
#endif

#ifdef CHEAT_IEEE80211MESH
Ieee80211Mesh::GateWayDataMap * Ieee80211Mesh::gateWayDataMap;
#endif

Define_Module(Ieee80211Mesh);

static uint64_t MacToUint64(const MACAddress &add)
{
    uint64_t aux;
    uint64_t lo=0;
    for (int i=0; i<MAC_ADDRESS_BYTES; i++)
    {
        aux  = add.getAddressByte(MAC_ADDRESS_BYTES-i-1);
        aux <<= 8*i;
        lo  |= aux ;
    }
    return lo;
}

static MACAddress Uint64ToMac(uint64_t lo)
{
    MACAddress add;
    add.setAddressByte(0, (lo>>40)&0xff);
    add.setAddressByte(1, (lo>>32)&0xff);
    add.setAddressByte(2, (lo>>24)&0xff);
    add.setAddressByte(3, (lo>>16)&0xff);
    add.setAddressByte(4, (lo>>8)&0xff);
    add.setAddressByte(5, lo&0xff);
    return add;
}

Ieee80211Mesh::~Ieee80211Mesh()
{
    if (mplsData)
        delete mplsData;
    if (WMPLSCHECKMAC)
        cancelAndDelete(WMPLSCHECKMAC);
    if (gateWayTimeOut)
        cancelAndDelete(gateWayTimeOut);
    associatedAddress.clear();
    if (getGateWayDataMap())
    {
        delete gateWayDataMap;
        gateWayDataMap=NULL;
    }
}

Ieee80211Mesh::Ieee80211Mesh()
{
	// Mpls data
    mplsData = NULL;
    gateWayDataMap=NULL;
    // subprocess
    ETXProcess=NULL;
    routingModuleProactive = NULL;
    routingModuleReactive = NULL;
    // packet timers
    WMPLSCHECKMAC =NULL;
    gateWayTimeOut=NULL;
    //
    macBaseGateId = -1;
    gateWayIndex=-1;
    isGateWay=false;
}

void Ieee80211Mesh::initialize(int stage)
{
    EV << "Init mesh proccess \n";
    Ieee80211MgmtBase::initialize(stage);

    if (stage== 0)
    {
    	if (gateWayDataMap==NULL)
    		gateWayDataMap=new GateWayDataMap;

        limitDelay = par("maxDelay").doubleValue();
        useLwmpls = par("UseLwMpls");
        maxHopProactiveFeedback = par("maxHopProactiveFeedback");
        maxHopProactive = par("maxHopProactive");
        maxHopReactive = par("maxHopReactive");
        maxTTL=par("maxTTL");
    }
    else if (stage==1)
    {
        bool useReactive = par("useReactive");
        bool useProactive = par("useProactive");
        //if (useReactive)
        //    useProactive = false;

        if (useReactive && useProactive)
        {
            proactiveFeedback  = par("ProactiveFeedback");
        }
        else
            proactiveFeedback = false;
        mplsData = new LWMPLSDataStructure;
         //
        // cambio para evitar que puedan estar los dos protocolos simultaneamente
        // cuidado con esto
        //
        // Proactive protocol
        if (useReactive)
            startReactive();
        // Reactive protocol
        if (useProactive)
            startProactive();

        if (routingModuleProactive==NULL && routingModuleReactive ==NULL)
            error("Ieee80211Mesh doesn't have active routing protocol");

        mplsData->mplsMaxTime()=35;
        activeMacBreak=false;
        if (activeMacBreak)
            WMPLSCHECKMAC = new cMessage();

        ETXProcess = NULL;

        if (par("ETXEstimate"))
        	startEtx();
    }
    if (stage==4)
    {
        macBaseGateId = gateSize("macOut")==0 ? -1 : gate("macOut",0)->getId();
        EV << "macBaseGateId :" << macBaseGateId << "\n";
        ift = InterfaceTableAccess ().get();
        nb = NotificationBoardAccess().get();
        nb->subscribe(this, NF_LINK_BREAK);
        nb->subscribe(this,NF_LINK_REFRESH);
    }
    //Gateway and group address code
    if (stage==5)
    {
    	isGateWay=false;
        if (par("IsGateWay"))
        	startGateWay();
        //end Gateway and group address code
    }
}

void Ieee80211Mesh::startProactive()
{
    cModuleType *moduleType;
    cModule *module;
    //if (isEtx)
    //  moduleType = cModuleType::find("inet.networklayer.manetrouting.OLSR_ETX");
    //else
    moduleType = cModuleType::find("inet.networklayer.manetrouting.OLSR");
    module = moduleType->create("ManetRoutingProtocolProactive", this);
    routingModuleProactive = dynamic_cast <ManetRoutingBase*> (module);
    routingModuleProactive->gate("to_ip")->connectTo(gate("routingInProactive"));
    gate("routingOutProactive")->connectTo(routingModuleProactive->gate("from_ip"));
    routingModuleProactive->buildInside();
    routingModuleProactive->scheduleStart(simTime());
}


void Ieee80211Mesh::startReactive()
{
    cModuleType *moduleType;
    cModule *module;
    moduleType = cModuleType::find("inet.networklayer.manetrouting.DYMOUM");
    module = moduleType->create("ManetRoutingProtocolReactive", this);
    routingModuleReactive = dynamic_cast <ManetRoutingBase*> (module);
    routingModuleReactive->gate("to_ip")->connectTo(gate("routingInReactive"));
    gate("routingOutReactive")->connectTo(routingModuleReactive->gate("from_ip"));
    routingModuleReactive->buildInside();
    routingModuleReactive->scheduleStart(simTime());
}

void Ieee80211Mesh::startEtx()
{
    cModuleType *moduleType;
    cModule *module;
    moduleType = cModuleType::find("inet.experimental.linklayer.ieee80211.mgmt.Ieee80211Etx");
    module = moduleType->create("ETXproc", this);
    ETXProcess = dynamic_cast <Ieee80211Etx*> (module);
    ETXProcess->gate("toMac")->connectTo(gate("ETXProcIn"));
    gate("ETXProcOut")->connectTo(ETXProcess->gate("fromMac"));
    ETXProcess->buildInside();
    ETXProcess->scheduleStart(simTime());
    ETXProcess->setAddress(myAddress);
}

void Ieee80211Mesh::startGateWay()
{
// check if the ethernet exist
// check: datarate is forbidden with EtherMAC -- module's txrate must be used
    cGate *g = gate("toEthernet")->getPathEndGate();
    MACAddress ethAddr;
    if (strcmp(g->getOwnerModule()->getClassName(),"EtherEncapMesh")!=0)
        return;
    // find the interface
    char interfaceName[100];
    for (int i=0;i<ift->getNumInterfaces();i++)
    {
        InterfaceEntry * ie= ift->getInterface(i);
        if (ie->isLoopback())
            continue;
        if (ie->getMacAddress()==myAddress)
            continue;
        memset(interfaceName,0,sizeof(interfaceName));
        char *d=interfaceName;
        for (const char *s=g->getOwnerModule()->getParentModule()->getFullName(); *s; s++)
        	if (isalnum(*s))
        		*d++ = *s;
        *d = '\0';
        if (strcmp(interfaceName,ie->getName())==0)
        {
        	ethAddr=ie->getMacAddress();
        	break;
        }
    }
    if (ethAddr.isUnspecified())
        opp_error("Mesh gateway not initialized, Ethernet interface not found");
    isGateWay=true;
    GateWayData data;
    data.idAddress=myAddress;
    data.ethAddress=ethAddr;
    #ifdef CHEAT_IEEE80211MESH
    data.associatedAddress=&associatedAddress;
    data.proactive=routingModuleProactive;
    data.reactive=routingModuleReactive;
    #endif
    getGateWayDataMap()->insert(std::pair<Uint128,GateWayData>(myAddress,data));
    if(routingModuleProactive)
        routingModuleProactive->addInAddressGroup(myAddress);
    if (routingModuleReactive)
        routingModuleReactive->addInAddressGroup(myAddress);
    gateWayIndex = 0;
    for(GateWayDataMap::iterator it=getGateWayDataMap()->begin();it!=getGateWayDataMap()->end();it++)
    {
        if (it->first ==myAddress)
            break;
        gateWayIndex++;
    }
    gateWayTimeOut = new cMessage();
    double delay=gateWayIndex*par("GateWayAnnounceInterval").doubleValue ();
    scheduleAt(simTime()+delay+uniform(0,2),gateWayTimeOut);
}

void Ieee80211Mesh::handleMessage(cMessage *msg)
{

    if (msg->isSelfMessage())
    {
        // process timers
        EV << "Timer expired: " << msg << "\n";
        handleTimer(msg);
        return;
    }
    cGate * msggate = msg->getArrivalGate();
    char gateName [40];
    memset(gateName,0,40);
    strcpy(gateName,msggate->getBaseName());
    //if (msg->arrivedOn("macIn"))
    if (strstr(gateName,"macIn")!=NULL)
    {
        // process incoming frame
        EV << "Frame arrived from MAC: " << msg << "\n";
        Ieee80211DataFrame *frame = dynamic_cast<Ieee80211DataFrame *>(msg);
        Ieee80211MeshFrame *frame2  = dynamic_cast<Ieee80211MeshFrame *>(msg);
        if (frame2)
            frame2->setTTL(frame2->getTTL()-1);
        actualizeReactive(frame,false);
        processFrame(frame);
    }
    //else if (msg->arrivedOn("agentIn"))
    else if (strstr(gateName,"agentIn")!=NULL)
    {
        // process command from agent
        EV << "Command arrived from agent: " << msg << "\n";
        int msgkind = msg->getKind();
        cPolymorphic *ctrl = msg->removeControlInfo();
        delete msg;
        handleCommand(msgkind, ctrl);
    }
    //else if (msg->arrivedOn("routingIn"))
    else if (strstr(gateName,"routingIn")!=NULL)
    {
        handleRoutingMessage(PK(msg));
    }
    else if (strstr(gateName,"ETXProcIn")!=NULL)
    {
        handleEtxMessage(PK(msg));
    }
    //else if (strstr(gateName,"interGateWayConect")!=NULL)
    else if (strstr(gateName,"fromEthernet")!=NULL)
    {
        handleWateGayDataReceive(PK(msg));
    }
    else
    {
        cPacket *pk = PK(msg);
        // packet from upper layers, to be sent out
        EV << "Packet arrived from upper layers: " << pk << "\n";
        if (pk->getByteLength() > 2312)
            error("message from higher layer (%s)%s is too long for 802.11b, %d bytes (fragmentation is not supported yet)",
                  pk->getClassName(), pk->getName(), pk->getByteLength());
        handleUpperMessage(pk);
    }
}

void Ieee80211Mesh::handleTimer(cMessage *msg)
{
    //ASSERT(false);
    mplsData->lwmpls_interface_delete_old_path();
    if (WMPLSCHECKMAC==msg)
        mplsCheckRouteTime();
    else if (gateWayTimeOut==msg)
        publishGateWayIdentity();
    else if (dynamic_cast<Ieee80211DataFrame*>(msg))
        sendOrEnqueue(PK(msg));
    else
        opp_error("message timer error");
}


void Ieee80211Mesh::handleRoutingMessage(cPacket *msg)
{
    cObject *temp  = msg->removeControlInfo();
    Ieee802Ctrl * ctrl = dynamic_cast<Ieee802Ctrl*> (temp);
    if (!ctrl)
    {
        char name[50];
        strcpy(name,msg->getName());
        error ("Message error");
    }
    Ieee80211DataFrame * frame = encapsulate(msg,ctrl->getDest());
    frame->setKind(ctrl->getInputPort());
    delete ctrl;
    if (frame)
        sendOrEnqueue(frame);
}

void Ieee80211Mesh::handleUpperMessage(cPacket *msg)
{
    Ieee80211DataFrame *frame = encapsulate(msg);
    if (frame && !isGateWay)
        sendOrEnqueue(frame);
    else if (frame)
    {
    	MACAddress gw;
    	if (!frame->getAddress4().isBroadcast()&& !frame->getAddress4().isUnspecified())
    	{
    	    if (selectGateWay(frame->getAddress4(),gw))
    	    {
               if (gw!=myAddress)
               {
                   frame->setReceiverAddress(gw);
                   sendOrEnqueue(frame);
                   return;
               }
            }
        }
        handleReroutingGateway(frame);
    }
}

void Ieee80211Mesh::handleCommand(int msgkind, cPolymorphic *ctrl)
{
    error("handleCommand(): no commands supported");
}

Ieee80211DataFrame *Ieee80211Mesh::encapsulate(cPacket *msg)
{
    Ieee80211MeshFrame *frame = new Ieee80211MeshFrame(msg->getName());
    frame->setTTL(maxTTL);
    frame->setTimestamp(msg->getCreationTime());
    LWMPLSPacket *lwmplspk = NULL;
    LWmpls_Forwarding_Structure *forwarding_ptr=NULL;

    // copy receiver address from the control info (sender address will be set in MAC)
    Ieee802Ctrl *ctrl = check_and_cast<Ieee802Ctrl *>(msg->removeControlInfo());
    MACAddress dest = ctrl->getDest();
    MACAddress next = ctrl->getDest();
    delete ctrl;
    frame->setFinalAddress(dest);

    frame->setAddress4(dest);
    frame->setAddress3(myAddress);


    if (dest.isBroadcast())
    {
        frame->setReceiverAddress(dest);
        frame->setTTL(1);
        uint32_t cont;

        mplsData->getBroadCastCounter(cont);

        lwmplspk = new LWMPLSPacket(msg->getName());
        cont++;
        mplsData->setBroadCastCounter(cont);
        lwmplspk->setCounter(cont);
        lwmplspk->setSource(myAddress);
        lwmplspk->setDest(dest);
        lwmplspk->setType(WMPLS_BROADCAST);
        lwmplspk->encapsulate(msg);
        frame->encapsulate(lwmplspk);
        return frame;
    }
    //
    // Search in the data base
    //
    int label = mplsData->getRegisterRoute(MacToUint64(dest));

    if (label!=-1)
    {
        forwarding_ptr = mplsData->lwmpls_forwarding_data(label,-1,0);
        if (!forwarding_ptr)
            mplsData->deleteRegisterRoute(MacToUint64(dest));

    }
    bool toGateWay=false;
    if (routingModuleReactive)
    {
        if (routingModuleReactive->findInAddressGroup(dest))
            toGateWay = true;
    }
    else if (routingModuleProactive)
    {
        if (routingModuleProactive->findInAddressGroup(dest))
             toGateWay = true;
    }

    if (forwarding_ptr)
    {
        lwmplspk = new LWMPLSPacket(msg->getName());
        lwmplspk->setTTL(maxTTL);
        lwmplspk->setSource(myAddress);
        lwmplspk->setDest(dest);

        if (forwarding_ptr->order==LWMPLS_EXTRACT)
        {
// Source or destination?
            if (forwarding_ptr->output_label>0 || forwarding_ptr->return_label_output>0)
            {
                lwmplspk->setType(WMPLS_NORMAL);
                if (forwarding_ptr->return_label_input==label && forwarding_ptr->output_label>0)
                {
                    next = Uint64ToMac(forwarding_ptr->mac_address);
                    lwmplspk->setLabel(forwarding_ptr->output_label);
                }
                else if (forwarding_ptr->input_label==label && forwarding_ptr->return_label_output>0)
                {
                    next = Uint64ToMac(forwarding_ptr->input_mac_address);
                    lwmplspk->setLabel(forwarding_ptr->return_label_output);
                }
                else
                {
                    opp_error("lwmpls data base error");
                }
            }
            else
            {
                lwmplspk->setType(WMPLS_BEGIN_W_ROUTE);

                int dist = forwarding_ptr->path.size()-2;
                lwmplspk->setVectorAddressArraySize(dist);
                //lwmplspk->setDist(dist);
                next=Uint64ToMac(forwarding_ptr->path[1]);

                for (int i=0; i<dist; i++)
                    lwmplspk->setVectorAddress(i,Uint64ToMac(forwarding_ptr->path[i+1]));
                lwmplspk->setLabel (forwarding_ptr->return_label_input);
            }
        }
        else
        {
            lwmplspk->setType(WMPLS_NORMAL);
            if (forwarding_ptr->input_label==label && forwarding_ptr->output_label>0)
            {
                next = Uint64ToMac(forwarding_ptr->mac_address);
                lwmplspk->setLabel(forwarding_ptr->output_label);
            }
            else if (forwarding_ptr->return_label_input==label && forwarding_ptr->return_label_output>0)
            {
                next = Uint64ToMac(forwarding_ptr->input_mac_address);
                lwmplspk->setLabel(forwarding_ptr->return_label_output);
            }
            else
            {
                opp_error("lwmpls data base error");
            }
        }
        forwarding_ptr->last_use=simTime();
    }
    else
    {
        std::vector<Uint128> add;
        int dist = 0;
        bool noRoute;

        if (routingModuleProactive)
        {
            if (toGateWay)
            {
                bool isToGt;
                Uint128 gateWayAddress;
                dist = routingModuleProactive->getRouteGroup(dest,add,gateWayAddress,isToGt);
                noRoute = false;
            }
            else
            {

                dist = routingModuleProactive->getRoute(dest,add);
                noRoute = false;
            }
        }

        if (dist==0)
        {
            // Search in the reactive routing protocol
            // Destination unreachable
            if (routingModuleReactive)
            {
                add.resize(1);
                if (toGateWay)
                {
                    int iface;
                    noRoute = true;
                    Uint128 gateWayAddress;
                    bool isToGt;

                    if (!routingModuleReactive->getNextHopGroup(dest,add[0],iface,gateWayAddress,isToGt)) //send the packet to the routingModuleReactive
                    {
                        ControlManetRouting *ctrlmanet = new ControlManetRouting();
                        ctrlmanet->setOptionCode(MANET_ROUTE_NOROUTE);
                        ctrlmanet->setDestAddress(dest);
                        ctrlmanet->setSrcAddress(myAddress);
                        frame->encapsulate(msg);
                        ctrlmanet->encapsulate(frame);
                        send(ctrlmanet,"routingOutReactive");
                        return NULL;
                    }
                    else
                    {
                        if (gateWayAddress == dest)
                            dist=1;
                        else
                            dist = 2;
                    }
                }
                else
                {
                    int iface;
                    noRoute = true;
                    double cost;
                    if (!routingModuleReactive->getNextHop(dest,add[0],iface,cost)) //send the packet to the routingModuleReactive
                    {
                        ControlManetRouting *ctrlmanet = new ControlManetRouting();
                        ctrlmanet->setOptionCode(MANET_ROUTE_NOROUTE);
                        ctrlmanet->setDestAddress(dest);
                        ctrlmanet->setSrcAddress(myAddress);
                        frame->encapsulate(msg);
                        ctrlmanet->encapsulate(frame);
                        send(ctrlmanet,"routingOutReactive");
                        return NULL;
                    }
                    else
                    {
                        if (add[0].getMACAddress() == dest)
                            dist=1;
                        else
                            dist = 2;
                    }

                }
            }
            else
            {
                delete frame;
                delete msg;
                return NULL;
            }
        }
        next=add[0];
        if (dist >1 && useLwmpls)
        {
            lwmplspk = new LWMPLSPacket(msg->getName());
            lwmplspk->setTTL(maxTTL);
            if (!noRoute)
                lwmplspk->setType(WMPLS_BEGIN_W_ROUTE);
            else
                lwmplspk->setType(WMPLS_BEGIN);

            lwmplspk->setSource(myAddress);
            lwmplspk->setDest(dest);
            if (!noRoute)
            {
                next=add[0];
                lwmplspk->setVectorAddressArraySize(dist-1);
                //lwmplspk->setDist(dist-1);
                for (int i=0; i<dist-1; i++)
                    lwmplspk->setVectorAddress(i,add[i]);
                lwmplspk->setByteLength(lwmplspk->getByteLength()+((dist-1)*6));
            }

            int label_in =mplsData->getLWMPLSLabel();

            /* es necesario introducir el nuevo path en la lista de enlace */
            //lwmpls_initialize_interface(lwmpls_data_ptr,&interface_str_ptr,label_in,sta_addr, ip_address,LWMPLS_INPUT_LABEL);
            /* es necesario ahora introducir los datos en la tabla */
            forwarding_ptr = new LWmpls_Forwarding_Structure();
            forwarding_ptr->input_label=-1;
            forwarding_ptr->return_label_input=label_in;
            forwarding_ptr->return_label_output=-1;
            forwarding_ptr->order=LWMPLS_EXTRACT;
            forwarding_ptr->mac_address=MacToUint64(next);
            forwarding_ptr->label_life_limit=mplsData->mplsMaxTime();
            forwarding_ptr->last_use=simTime();

            forwarding_ptr->path.push_back(MacToUint64(myAddress));
            for (int i=0; i<dist-1; i++)
                forwarding_ptr->path.push_back(MacToUint64(add[i]));
            forwarding_ptr->path.push_back(MacToUint64(dest));

            mplsData->lwmpls_forwarding_input_data_add(label_in,forwarding_ptr);
            // lwmpls_forwarding_output_data_add(label_out,sta_addr,forwarding_ptr,true);
            /*lwmpls_label_fw_relations (lwmpls_data_ptr,label_in,forwarding_ptr);*/
            lwmplspk->setLabel (label_in);
            mplsData->registerRoute(MacToUint64(dest),label_in);
        }
    }

    frame->setReceiverAddress(next);
    if (lwmplspk)
    {
        lwmplspk->encapsulate(msg);
        frame->setTTL(lwmplspk->getTTL());
        frame->encapsulate(lwmplspk);
    }
    else
        frame->encapsulate(msg);

    if (frame->getReceiverAddress().isUnspecified())
        ASSERT(!frame->getReceiverAddress().isUnspecified());
    return frame;
}

Ieee80211DataFrame *Ieee80211Mesh::encapsulate(cPacket *msg,MACAddress dest)
{
	Ieee80211MeshFrame *frame = dynamic_cast<Ieee80211MeshFrame*>(msg);
	if (frame==NULL)
        frame = new Ieee80211MeshFrame(msg->getName());
    frame->setTimestamp(msg->getCreationTime());
    frame->setTTL(maxTTL);

    if (msg->getControlInfo())
        delete msg->removeControlInfo();
    LWMPLSPacket* msgAux = dynamic_cast<LWMPLSPacket*> (msg);
    if (msgAux)
    {
        frame->setTTL(msgAux->getTTL());
    }
    frame->setReceiverAddress(dest);
    if (msg!=frame)
        frame->encapsulate(msg);

    if (frame->getReceiverAddress().isUnspecified())
    {
        char name[50];
        strcpy(name,msg->getName());
        opp_error ("Ieee80211Mesh::encapsulate Bad Address");
    }
    if (frame->getReceiverAddress().isBroadcast())
        frame->setTTL(1);
    return frame;
}


void Ieee80211Mesh::receiveChangeNotification(int category, const cPolymorphic *details)
{
    Enter_Method_Silent();
    printNotificationBanner(category, details);

    if (details==NULL)
        return;

    if (category == NF_LINK_BREAK)
    {
        if (details==NULL)
            return;
        Ieee80211DataFrame *frame  = check_and_cast<Ieee80211DataFrame *>(details);
        MACAddress add = frame->getReceiverAddress();
        mplsBreakMacLink(add);
    }
    else if (category == NF_LINK_REFRESH)
    {
        Ieee80211TwoAddressFrame *frame  = check_and_cast<Ieee80211TwoAddressFrame *>(details);
        if (frame)
            mplsData->lwmpls_refresh_mac (MacToUint64(frame->getTransmitterAddress()),simTime());
    }
}

void Ieee80211Mesh::handleDataFrame(Ieee80211DataFrame *frame)
{
    // The message is forward
    if (forwardMessage (frame))
        return;

    MACAddress finalAddress;
    MACAddress source= frame->getTransmitterAddress();
    Ieee80211MeshFrame *frame2  = dynamic_cast<Ieee80211MeshFrame *>(frame);
    short ttl = maxTTL;
    if (frame2)
    {
        ttl = frame2->getTTL();
        finalAddress =frame2->getFinalAddress();
    }
    cPacket *msg = decapsulate(frame);
    ///
    /// If it's a ETX packet to send to the appropriate module
    ///
    if (dynamic_cast<ETXBasePacket*>(msg))
    {
        if (ETXProcess)
        {
            if (msg->getControlInfo())
                delete msg->removeControlInfo();
            send(msg,"ETXProcOut");
        }
        else
            delete msg;
        return;
    }

    LWMPLSPacket *lwmplspk = dynamic_cast<LWMPLSPacket*> (msg);
    mplsData->lwmpls_refresh_mac(MacToUint64(source),simTime());

    if(isGateWay && lwmplspk)
    {

        if (lwmplspk->getDest()!=myAddress)
        {
            GateWayDataMap::iterator it = getGateWayDataMap()->find((Uint128)lwmplspk->getDest());
            if (it!=getGateWayDataMap()->end() && frame2->getAddress4()!=myAddress)
                associatedAddress[lwmplspk->getSource()]=simTime();
        }
        else
            associatedAddress[lwmplspk->getSource()]=simTime();
    }

    if (!lwmplspk)
    {
        //cGate * msggate = msg->getArrivalGate();
        //int baseId = gateBaseId("macIn");
        //int index = baseId - msggate->getId();
        msg->setKind(0);
        if ((routingModuleProactive != NULL) && (routingModuleProactive->isOurType(msg)))
        {
            //sendDirect(msg,0, routingModule, "from_ip");
            send(msg,"routingOutProactive");
        }
        // else if (dynamic_cast<AODV_msg  *>(msg) || dynamic_cast<DYMO_element  *>(msg))
        else if ((routingModuleReactive != NULL) && routingModuleReactive->isOurType(msg))
        {
            Ieee802Ctrl *ctrl = check_and_cast<Ieee802Ctrl*>(msg->removeControlInfo());
            MeshControlInfo *controlInfo = new MeshControlInfo;
            Ieee802Ctrl *ctrlAux = controlInfo;
            *ctrlAux=*ctrl;
            delete ctrl;
            Uint128 dest;
            msg->setControlInfo(controlInfo);
            if (routingModuleReactive->getDestAddress(msg,dest))
            {
                std::vector<Uint128>add;
                Uint128 src = controlInfo->getSrc();
                int dist = 0;
                if (routingModuleProactive && proactiveFeedback)
                {
                    // int neig = routingModuleProactive))->getRoute(src,add);
                    controlInfo->setPreviousFix(true); // This node is fix
                    dist = routingModuleProactive->getRoute(dest,add);
                }
                else
                    controlInfo->setPreviousFix(false); // This node is not fix
                if (maxHopProactive>0 && dist>maxHopProactive)
                    dist = 0;
                if (dist!=0 && proactiveFeedback)
                {
                    controlInfo->setVectorAddressArraySize(dist);
                    for (int i=0; i<dist; i++)
                        controlInfo->setVectorAddress(i,add[i]);
                }
            }
            send(msg,"routingOutReactive");
        }
        else if (dynamic_cast<OLSR_pkt*>(msg) || dynamic_cast <DYMO_element *>(msg) || dynamic_cast <AODV_msg *>(msg))
        {
            delete msg;
        }
        else // Normal frame test if upper layer frame in other case delete
            sendUp(msg);
        return;
    }
    lwmplspk->setTTL(ttl);
    mplsDataProcess(lwmplspk,source);
}

void Ieee80211Mesh::handleAuthenticationFrame(Ieee80211AuthenticationFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211Mesh::handleDeauthenticationFrame(Ieee80211DeauthenticationFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211Mesh::handleAssociationRequestFrame(Ieee80211AssociationRequestFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211Mesh::handleAssociationResponseFrame(Ieee80211AssociationResponseFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211Mesh::handleReassociationRequestFrame(Ieee80211ReassociationRequestFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211Mesh::handleReassociationResponseFrame(Ieee80211ReassociationResponseFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211Mesh::handleDisassociationFrame(Ieee80211DisassociationFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211Mesh::handleBeaconFrame(Ieee80211BeaconFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211Mesh::handleProbeRequestFrame(Ieee80211ProbeRequestFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211Mesh::handleProbeResponseFrame(Ieee80211ProbeResponseFrame *frame)
{
    dropManagementFrame(frame);
}

cPacket *Ieee80211Mesh::decapsulateMpls(LWMPLSPacket *frame)
{
    cPacket *payload = frame->decapsulate();
    // ctrl->setSrc(frame->getAddress3());
    Ieee802Ctrl *ctrl =(Ieee802Ctrl*) frame->removeControlInfo();
    payload->setControlInfo(ctrl);
    delete frame;
    return payload;
}

void Ieee80211Mesh::mplsSendAck(int label)
{
    if (label >= LWMPLS_MAX_LABEL || label <= 0)
        opp_error("mplsSendAck error in label %i", label);
    LWMPLSPacket *mpls_pk_aux_ptr = new LWMPLSPacket();
    mpls_pk_aux_ptr->setLabelReturn (label);
    LWmpls_Forwarding_Structure * forwarding_ptr = mplsData->lwmpls_forwarding_data(label,0,0);

    MACAddress sta_addr;
    int return_label;
    if (forwarding_ptr->input_label==label)
    {
        sta_addr = Uint64ToMac(forwarding_ptr->input_mac_address);
        return_label = forwarding_ptr->return_label_output;
    }
    else if (forwarding_ptr->return_label_input==label)
    {
        sta_addr = Uint64ToMac(forwarding_ptr->mac_address);
        return_label = forwarding_ptr->output_label;
    }
    mpls_pk_aux_ptr->setType(WMPLS_ACK);
    mpls_pk_aux_ptr->setLabel(return_label);
    mpls_pk_aux_ptr->setDest(sta_addr);
    mpls_pk_aux_ptr->setSource(myAddress);
//  sendOrEnqueue(encapsulate (mpls_pk_aux_ptr, sta_addr));
    sendOrEnqueue(encapsulate (mpls_pk_aux_ptr, MACAddress::BROADCAST_ADDRESS));
    /* initialize the mac timer */
    mplsInitializeCheckMac ();
}

//
// Crea las estructuras para enviar los paquetes por mpls e inicializa los registros del mac
//
void Ieee80211Mesh::mplsCreateNewPath(int label,LWMPLSPacket *mpls_pk_ptr,MACAddress sta_addr)
{
    int label_out = label;
// Alwais send a ACK
    int label_in;

    LWmpls_Interface_Structure * interface=NULL;

    LWmpls_Forwarding_Structure * forwarding_ptr = mplsData->lwmpls_forwarding_data(0,label_out,MacToUint64(sta_addr));
    if (forwarding_ptr!=NULL)
    {
        mplsData->lwmpls_check_label (forwarding_ptr->input_label,"begin");
        mplsData->lwmpls_check_label (forwarding_ptr->return_label_input,"begin");
        forwarding_ptr->last_use=simTime();

        mplsData->lwmpls_init_interface(&interface,forwarding_ptr->input_label,MacToUint64(sta_addr),LWMPLS_INPUT_LABEL);
// Is the destination?
        if (mpls_pk_ptr->getDest()==myAddress)
        {
            sendUp(decapsulateMpls(mpls_pk_ptr));
            forwarding_ptr->order = LWMPLS_EXTRACT;
            forwarding_ptr->output_label=0;
            if (Uint64ToMac(forwarding_ptr->input_mac_address) == sta_addr)
                mplsSendAck(forwarding_ptr->input_label);
            else if (Uint64ToMac(forwarding_ptr->mac_address) == sta_addr)
                mplsSendAck(forwarding_ptr->return_label_input);
            return;
        }
        int usedOutLabel;
        int usedIntLabel;
        MACAddress nextMacAddress;
        if (sta_addr == Uint64ToMac(forwarding_ptr->input_mac_address)) // forward path
        {
            usedOutLabel = forwarding_ptr->output_label;
            usedIntLabel = forwarding_ptr->input_label;
            nextMacAddress = Uint64ToMac(forwarding_ptr->mac_address);
        }
        else if (sta_addr== Uint64ToMac(forwarding_ptr->mac_address)) // reverse path
        {
            usedOutLabel = forwarding_ptr->return_label_output;
            usedIntLabel = forwarding_ptr->return_label_input;
            nextMacAddress = Uint64ToMac(forwarding_ptr->input_mac_address);
        }
        else
            opp_error("mplsCreateNewPath mac address incorrect");
        label_in = usedIntLabel;

        if (usedOutLabel>0)
        {
            /* path already exist */
            /* change to normal */
            mpls_pk_ptr->setType(WMPLS_NORMAL);
            cPacket * pk = mpls_pk_ptr->decapsulate();
            mpls_pk_ptr->setVectorAddressArraySize(0);
            mpls_pk_ptr->setByteLength(4);
            if (pk)
                mpls_pk_ptr->encapsulate(pk);

            //int dist = mpls_pk_ptr->getDist();
            //mpls_pk_ptr->setDist(0);
            /*op_pk_nfd_set_int32 (mpls_pk_ptr, "label",forwarding_ptr->output_label);*/
            Ieee80211MeshFrame *frame = new Ieee80211MeshFrame(mpls_pk_ptr->getName());
            frame->setTTL(mpls_pk_ptr->getTTL());
            frame->setTimestamp(mpls_pk_ptr->getCreationTime());
            if (usedOutLabel<=0 || usedIntLabel<=0)
                opp_error("mplsCreateNewPath Error in label");

            mpls_pk_ptr->setLabel(usedOutLabel);
            frame->setReceiverAddress(nextMacAddress);
            frame->setAddress4(mpls_pk_ptr->getDest());
            frame->setAddress3(mpls_pk_ptr->getSource());

            label_in = usedIntLabel;

            if (mpls_pk_ptr->getControlInfo())
                delete mpls_pk_ptr->removeControlInfo();

            frame->encapsulate(mpls_pk_ptr);
            if (frame->getReceiverAddress().isUnspecified())
                ASSERT(!frame->getReceiverAddress().isUnspecified());
            sendOrEnqueue(frame);
        }
        else
        {

            if (Uint64ToMac(forwarding_ptr->mac_address).isUnspecified())
            {
                forwarding_ptr->output_label=0;
                if (mpls_pk_ptr->getType()==WMPLS_BEGIN ||
                        mpls_pk_ptr->getVectorAddressArraySize()==0 )
                    //mpls_pk_ptr->getDist()==0 )
                {
                    std::vector<Uint128> add;
                    add.resize(1);
                    int dist = 0;
                    bool toGateWay=false;
                    if (routingModuleReactive)
                    {
                        if (routingModuleReactive->findInAddressGroup(mpls_pk_ptr->getDest()))
                            toGateWay = true;
                    }
                    else if (routingModuleProactive)
                    {
                        if (routingModuleProactive->findInAddressGroup(mpls_pk_ptr->getDest()))
                             toGateWay = true;
                    }
                    Uint128 gateWayAddress;
                    if (routingModuleProactive)
                    {
                        if (toGateWay)
                        {
                            bool isToGw;
                            dist = routingModuleProactive->getRouteGroup(mpls_pk_ptr->getDest(),add,gateWayAddress,isToGw);
                        }
                        else
                        {
                            dist = routingModuleProactive->getRoute(mpls_pk_ptr->getDest(),add);
                        }
                    }
                    if (dist==0 && routingModuleReactive)
                    {
                        int iface;
                        add.resize(1);
                        double cost;
                        if (toGateWay)
                        {
                            bool isToGw;
                            if (routingModuleReactive->getNextHopGroup(mpls_pk_ptr->getDest(),add[0],iface,gateWayAddress,isToGw))
                               dist = 1;
                        }
                        else
                            if (routingModuleReactive->getNextHop(mpls_pk_ptr->getDest(),add[0],iface,cost))
                               dist = 1;
                    }

                    if (dist==0)
                    {
                        // Destination unreachable
                        if (routingModuleReactive)
                        {
                            ControlManetRouting *ctrlmanet = new ControlManetRouting();
                            ctrlmanet->setOptionCode(MANET_ROUTE_NOROUTE);
                            ctrlmanet->setDestAddress(mpls_pk_ptr->getDest());
                            ctrlmanet->setSrcAddress(mpls_pk_ptr->getSource());
                            send(ctrlmanet,"routingOutReactive");
                        }
                        mplsData->deleteForwarding(forwarding_ptr);
                        delete mpls_pk_ptr;
                        return;
                    }
                    forwarding_ptr->mac_address=MacToUint64(add[0]);
                }
                else
                {
                    int position = -1;
                    int arraySize = mpls_pk_ptr->getVectorAddressArraySize();
                    //int arraySize = mpls_pk_ptr->getDist();
                    for (int i=0; i<arraySize; i++)
                        if (mpls_pk_ptr->getVectorAddress(i)==myAddress)
                            position = i;
                    if (position==(arraySize-1))
                        forwarding_ptr->mac_address= MacToUint64 (mpls_pk_ptr->getDest());
                    else if (position>=0)
                    {
// Check if neigbourd?
                        forwarding_ptr->mac_address=MacToUint64(mpls_pk_ptr->getVectorAddress(position+1));
                    }
                    else
                    {
// Local route
                        std::vector<Uint128> add;
                        int dist = 0;
                        bool toGateWay=false;
                        Uint128 gateWayAddress;
                        if (routingModuleReactive)
                        {
                            if (routingModuleReactive->findInAddressGroup(mpls_pk_ptr->getDest()))
                                toGateWay = true;
                        }
                        else if (routingModuleProactive)
                        {
                            if (routingModuleProactive->findInAddressGroup(mpls_pk_ptr->getDest()))
                                 toGateWay = true;
                        }
                        if (toGateWay)
                        {
                            bool isToGw;
                            dist = routingModuleProactive->getRouteGroup(mpls_pk_ptr->getDest(),add,gateWayAddress,isToGw);
                        }
                        else
                        {
                            dist = routingModuleProactive->getRoute(mpls_pk_ptr->getDest(),add);
                        }
                        if (dist==0 && routingModuleReactive)
                        {
                            int iface;
                            add.resize(1);
                            double cost;
                            if (toGateWay)
                            {

                                bool isToGw;
                                if (routingModuleReactive->getNextHopGroup(mpls_pk_ptr->getDest(),add[0],iface,gateWayAddress,isToGw))
                                     dist = 1;
                            }
                            else
                                if (routingModuleReactive->getNextHop(mpls_pk_ptr->getDest(),add[0],iface,cost))
                                      dist = 1;
                        }
                        if (dist==0)
                        {
                            // Destination unreachable
                            if (routingModuleReactive)
                            {
                                ControlManetRouting *ctrlmanet = new ControlManetRouting();
                                ctrlmanet->setOptionCode(MANET_ROUTE_NOROUTE);
                                ctrlmanet->setDestAddress(mpls_pk_ptr->getDest());
                                ctrlmanet->setSrcAddress(mpls_pk_ptr->getSource());
                                send(ctrlmanet,"routingOutReactive");
                            }
                            mplsData->deleteForwarding(forwarding_ptr);
                            delete mpls_pk_ptr;
                            return;
                        }
                        forwarding_ptr->mac_address=MacToUint64(add[0]);
                        mpls_pk_ptr->setVectorAddressArraySize(0);
                        //mpls_pk_ptr->setDist(0);
                    }
                }
            }

            if (forwarding_ptr->return_label_input<=0)
                opp_error("Error in label");

            mpls_pk_ptr->setLabel(forwarding_ptr->return_label_input);
            mpls_pk_ptr->setLabelReturn(0);
            Ieee80211MeshFrame *frame = new Ieee80211MeshFrame(mpls_pk_ptr->getName());
            frame->setTTL(mpls_pk_ptr->getTTL());
            frame->setTimestamp(mpls_pk_ptr->getCreationTime());
            frame->setReceiverAddress(Uint64ToMac(forwarding_ptr->mac_address));
            frame->setAddress4(mpls_pk_ptr->getDest());
            frame->setAddress3(mpls_pk_ptr->getSource());

            if (mpls_pk_ptr->getControlInfo())
                delete mpls_pk_ptr->removeControlInfo();


            frame->encapsulate(mpls_pk_ptr);
            if (frame->getReceiverAddress().isUnspecified())
                ASSERT(!frame->getReceiverAddress().isUnspecified());
            sendOrEnqueue(frame);
        }
    }
    else
    {
// New structure
        /* Obtain a label */
        label_in =mplsData->getLWMPLSLabel();
        mplsData->lwmpls_init_interface(&interface,label_in,MacToUint64 (sta_addr),LWMPLS_INPUT_LABEL);
        /* es necesario introducir el nuevo path en la lista de enlace */
        //lwmpls_initialize_interface(lwmpls_data_ptr,&interface_str_ptr,label_in,sta_addr, ip_address,LWMPLS_INPUT_LABEL);
        /* es necesario ahora introducir los datos en la tabla */
        forwarding_ptr = new LWmpls_Forwarding_Structure();
        forwarding_ptr->output_label=0;
        forwarding_ptr->input_label=label_in;
        forwarding_ptr->return_label_input=0;
        forwarding_ptr->return_label_output=label_out;
        forwarding_ptr->order=LWMPLS_EXTRACT;
        forwarding_ptr->input_mac_address=MacToUint64(sta_addr);
        forwarding_ptr->label_life_limit =mplsData->mplsMaxTime();
        forwarding_ptr->last_use=simTime();

        forwarding_ptr->path.push_back((Uint128)mpls_pk_ptr->getSource());
        for (unsigned int i=0 ; i<mpls_pk_ptr->getVectorAddressArraySize(); i++)
            //for (int i=0 ;i<mpls_pk_ptr->getDist();i++)
            forwarding_ptr->path.push_back((Uint128)mpls_pk_ptr->getVectorAddress(i));
        forwarding_ptr->path.push_back((Uint128)mpls_pk_ptr->getDest());

        // Add structure
        mplsData->lwmpls_forwarding_input_data_add(label_in,forwarding_ptr);
        if (!mplsData->lwmpls_forwarding_output_data_add(label_out,MacToUint64(sta_addr),forwarding_ptr,true))
        {
            mplsBasicSend (mpls_pk_ptr,sta_addr);
            return;
        }

        if (mpls_pk_ptr->getDest()==myAddress)
        {
            mplsSendAck(label_in);
            mplsData->registerRoute(MacToUint64(mpls_pk_ptr->getSource()),label_in);
            sendUp(decapsulateMpls(mpls_pk_ptr));
            // Register route
            return;
        }

        if (mpls_pk_ptr->getType()==WMPLS_BEGIN ||
                mpls_pk_ptr->getVectorAddressArraySize()==0 )
            //mpls_pk_ptr->getDist()==0 )
        {
            std::vector<Uint128> add;
            int dist = 0;
            if (routingModuleProactive)
            {
                dist = routingModuleProactive->getRoute(mpls_pk_ptr->getDest(),add);
            }
            if (dist==0 && routingModuleReactive)
            {
                int iface;
                add.resize(1);
                double cost;
                if (routingModuleReactive->getNextHop(mpls_pk_ptr->getDest(),add[0],iface,cost))
                    dist = 1;
            }

            if (dist==0)
            {
                // Destination unreachable
                if (routingModuleReactive)
                {
                    ControlManetRouting *ctrlmanet = new ControlManetRouting();
                    ctrlmanet->setOptionCode(MANET_ROUTE_NOROUTE);
                    ctrlmanet->setDestAddress(mpls_pk_ptr->getDest());
                    ctrlmanet->setSrcAddress(mpls_pk_ptr->getSource());
                    send(ctrlmanet,"routingOutReactive");
                }

                mplsData->deleteForwarding(forwarding_ptr);
                delete mpls_pk_ptr;
                return;
            }
            forwarding_ptr->mac_address=MacToUint64(add[0]);
        }
        else
        {
            int position = -1;
            int arraySize = mpls_pk_ptr->getVectorAddressArraySize();
            //int arraySize = mpls_pk_ptr->getDist();
            for (int i=0; i<arraySize; i++)
                if (mpls_pk_ptr->getVectorAddress(i)==myAddress)
                {
                    position = i;
                    break;
                }
            if (position==(arraySize-1) && (position>=0))
                forwarding_ptr->mac_address=MacToUint64(mpls_pk_ptr->getDest());
            else if (position>=0)
            {
// Check if neigbourd?
                forwarding_ptr->mac_address=MacToUint64(mpls_pk_ptr->getVectorAddress(position+1));
            }
            else
            {
// Local route o discard?
                // delete mpls_pk_ptr
                // return;
                std::vector<Uint128> add;
                int dist = 0;
                if (routingModuleProactive)
                {
                    dist = routingModuleProactive->getRoute(mpls_pk_ptr->getDest(),add);
                }
                if (dist==0 && routingModuleReactive)
                {
                    int iface;
                    add.resize(1);
                    double cost;

                    if (routingModuleReactive->getNextHop(mpls_pk_ptr->getDest(),add[0],iface,cost))
                        dist = 1;
                }
                if (dist==0)
                {
                    // Destination unreachable
                    if (routingModuleReactive)
                    {
                        ControlManetRouting *ctrlmanet = new ControlManetRouting();
                        ctrlmanet->setOptionCode(MANET_ROUTE_NOROUTE);
                        ctrlmanet->setDestAddress(mpls_pk_ptr->getDest());
                        ctrlmanet->setSrcAddress(mpls_pk_ptr->getSource());
                        send(ctrlmanet,"routingOutReactive");
                    }
                    mplsData->deleteForwarding(forwarding_ptr);
                    delete mpls_pk_ptr;
                    return;
                }
                forwarding_ptr->mac_address=MacToUint64 (add[0]);
                mpls_pk_ptr->setVectorAddressArraySize(0);
                //mpls_pk_ptr->setDist(0);
            }
        }

// Send to next node
        Ieee80211MeshFrame *frame = new Ieee80211MeshFrame(mpls_pk_ptr->getName());
        frame->setTTL(mpls_pk_ptr->getTTL());
        frame->setTimestamp(mpls_pk_ptr->getCreationTime());
        frame->setReceiverAddress(Uint64ToMac(forwarding_ptr->mac_address));
        frame->setAddress4(mpls_pk_ptr->getDest());
        frame->setAddress3(mpls_pk_ptr->getSource());

// The reverse path label
        forwarding_ptr->return_label_input = mplsData->getLWMPLSLabel();
// Initialize the next interface
        interface = NULL;
        mplsData->lwmpls_init_interface(&interface,forwarding_ptr->return_label_input,forwarding_ptr->mac_address,LWMPLS_INPUT_LABEL_RETURN);
// Store the reverse path label
        mplsData->lwmpls_forwarding_input_data_add(forwarding_ptr->return_label_input,forwarding_ptr);

        mpls_pk_ptr->setLabel(forwarding_ptr->return_label_input);
        mpls_pk_ptr->setLabelReturn(0);

        if (mpls_pk_ptr->getControlInfo())
            delete mpls_pk_ptr->removeControlInfo();

        frame->encapsulate(mpls_pk_ptr);
        if (frame->getReceiverAddress().isUnspecified())
            ASSERT(!frame->getReceiverAddress().isUnspecified());
        sendOrEnqueue(frame);
    }
    mplsSendAck(label_in);
}

void Ieee80211Mesh::mplsBasicSend (LWMPLSPacket *mpls_pk_ptr,MACAddress sta_addr)
{
    if (mpls_pk_ptr->getDest()==myAddress)
    {
        sendUp(decapsulateMpls(mpls_pk_ptr));
    }
    else
    {
        std::vector<Uint128> add;
        int dist=0;

        if (routingModuleProactive)
        {
            dist = routingModuleProactive->getRoute(mpls_pk_ptr->getDest(),add);
        }
        if (dist==0 && routingModuleReactive)
        {
            int iface;
            add.resize(1);
            double cost;
            if (routingModuleReactive->getNextHop(mpls_pk_ptr->getDest(),add[0],iface,cost))
                dist = 1;
        }


        if (dist==0)
        {
            // Destination unreachable
            if (routingModuleReactive)
            {
                ControlManetRouting *ctrlmanet = new ControlManetRouting();
                ctrlmanet->setOptionCode(MANET_ROUTE_NOROUTE);
                ctrlmanet->setDestAddress(mpls_pk_ptr->getDest());
                ctrlmanet->setSrcAddress(mpls_pk_ptr->getSource());
                send(ctrlmanet,"routingOutReactive");
            }
            delete mpls_pk_ptr;
            return;
        }

        mpls_pk_ptr->setType(WMPLS_SEND);
        cPacket * pk = mpls_pk_ptr->decapsulate();
        mpls_pk_ptr->setVectorAddressArraySize(0);
        //mpls_pk_ptr->setByteLength(0);
        if (pk)
            mpls_pk_ptr->encapsulate(pk);
        Ieee80211MeshFrame *frame = new Ieee80211MeshFrame(mpls_pk_ptr->getName());
        frame->setTimestamp(mpls_pk_ptr->getCreationTime());
        frame->setAddress4(mpls_pk_ptr->getDest());
        frame->setAddress3(mpls_pk_ptr->getSource());
        frame->setTTL(mpls_pk_ptr->getTTL());


        if (dist>1)
            frame->setReceiverAddress(add[0]);
        else
            frame->setReceiverAddress(mpls_pk_ptr->getDest());

        if (mpls_pk_ptr->getControlInfo())
            delete mpls_pk_ptr->removeControlInfo();
        frame->encapsulate(mpls_pk_ptr);
        if (frame->getReceiverAddress().isUnspecified())
            ASSERT(!frame->getReceiverAddress().isUnspecified());
        sendOrEnqueue(frame);
    }
}

void Ieee80211Mesh::mplsBreakPath(int label,LWMPLSPacket *mpls_pk_ptr,MACAddress sta_addr)
{
    // printf("break %f\n",time);
    // printf("code %i my_address %d org %d lin %d \n",code,my_address,sta_addr,label);
    // liberar todos los path, dos pasos quien detecta la ruptura y quien la propaga.
    // Es mecesario tambien liberar los caminos de retorno.
    /*  forwarding_ptr= lwmpls_forwarding_data(lwmpls_data_ptr,0,label,sta_addr);*/
    MACAddress send_mac_addr;
    LWmpls_Forwarding_Structure * forwarding_ptr=mplsData->lwmpls_interface_delete_label(label);
    if (forwarding_ptr == NULL)
    {
        delete mpls_pk_ptr;
        return;
    }

    if (label == forwarding_ptr->input_label)
    {
        mpls_pk_ptr->setLabel(forwarding_ptr->output_label);
        send_mac_addr = Uint64ToMac(forwarding_ptr->mac_address);
    }
    else
    {
        mpls_pk_ptr->setLabel(forwarding_ptr->return_label_output);
        send_mac_addr = Uint64ToMac(forwarding_ptr->input_mac_address);
    }

    mplsPurge (forwarding_ptr,true);
    // Must clean the routing tables?

    if ((forwarding_ptr->order==LWMPLS_CHANGE) && (!send_mac_addr.isUnspecified()))
    {
        Ieee80211MeshFrame *frame = new Ieee80211MeshFrame(mpls_pk_ptr->getName());
        frame->setTimestamp(mpls_pk_ptr->getCreationTime());
        frame->setReceiverAddress(send_mac_addr);
        frame->setAddress4(mpls_pk_ptr->getDest());
        frame->setAddress3(mpls_pk_ptr->getSource());
        frame->setTTL(mpls_pk_ptr->getTTL());
        if (mpls_pk_ptr->getControlInfo())
            delete mpls_pk_ptr->removeControlInfo();
        frame->encapsulate(mpls_pk_ptr);
        if (frame->getReceiverAddress().isUnspecified())
            ASSERT(!frame->getReceiverAddress().isUnspecified());
        sendOrEnqueue(frame);
    }
    else
    {
        // Firts or last node
        delete mpls_pk_ptr;
        //mplsData->deleteRegisterLabel(forwarding_ptr->input_label);
        //mplsData->deleteRegisterLabel(forwarding_ptr->return_label_input);
    }

    mplsData->deleteForwarding(forwarding_ptr);
}


void Ieee80211Mesh::mplsNotFoundPath(int label,LWMPLSPacket *mpls_pk_ptr,MACAddress sta_addr)
{
    LWmpls_Forwarding_Structure * forwarding_ptr= mplsData->lwmpls_forwarding_data(0,label,MacToUint64 (sta_addr));
    MACAddress send_mac_addr;
    if (forwarding_ptr == NULL)
        delete mpls_pk_ptr;
    else
    {
        mplsData->lwmpls_interface_delete_label(forwarding_ptr->input_label);
        mplsData->lwmpls_interface_delete_label(forwarding_ptr->return_label_input);
        if (label == forwarding_ptr->output_label)
        {
            mpls_pk_ptr->setLabel (forwarding_ptr->input_label);
            send_mac_addr = Uint64ToMac (forwarding_ptr->input_mac_address);
        }
        else
        {
            mpls_pk_ptr->setLabel(forwarding_ptr->return_label_input);
            send_mac_addr = Uint64ToMac (forwarding_ptr->mac_address);
        }
        mplsPurge (forwarding_ptr,false);

        if ((forwarding_ptr->order==LWMPLS_CHANGE)&&(!send_mac_addr.isUnspecified()))
        {
            Ieee80211MeshFrame *frame = new Ieee80211MeshFrame(mpls_pk_ptr->getName());
            frame->setTimestamp(mpls_pk_ptr->getCreationTime());
            frame->setReceiverAddress(send_mac_addr);
            frame->setAddress4(mpls_pk_ptr->getDest());
            frame->setAddress3(mpls_pk_ptr->getSource());
            frame->setTTL(mpls_pk_ptr->getTTL());
            if (mpls_pk_ptr->getControlInfo())
                delete mpls_pk_ptr->removeControlInfo();
            frame->encapsulate(mpls_pk_ptr);
            if (frame->getReceiverAddress().isUnspecified())
                ASSERT(!frame->getReceiverAddress().isUnspecified());
            sendOrEnqueue(frame);
        }
        else
        {
            delete mpls_pk_ptr;
            //deleteRegisterLabel(forwarding_ptr->input_label);
            //deleteRegisterLabel(forwarding_ptr->return_label_input);
        }
        /* */
        mplsData->deleteForwarding(forwarding_ptr);
    }
}

void Ieee80211Mesh::mplsForwardData(int label,LWMPLSPacket *mpls_pk_ptr,MACAddress sta_addr,LWmpls_Forwarding_Structure *forwarding_data)
{
    /* Extraer la etiqueta y la direcci�n de enlace del siguiente salto */
    LWmpls_Forwarding_Structure * forwarding_ptr = forwarding_data;
    if (forwarding_ptr==NULL)
        forwarding_ptr =  mplsData->lwmpls_forwarding_data(label,0,0);
    forwarding_ptr->last_use=simTime();
    bool is_source=false;
    int output_label,input_label_aux;
    MACAddress send_mac_addr;

    if (forwarding_ptr->order==LWMPLS_CHANGE || is_source)
    {
        if ((label == forwarding_ptr->input_label) || is_source)
        {
            output_label=forwarding_ptr->output_label;
            input_label_aux=forwarding_ptr->return_label_input;
            send_mac_addr = Uint64ToMac (forwarding_ptr->mac_address);
        }
        else
        {
            output_label=forwarding_ptr->return_label_output;
            input_label_aux=forwarding_ptr->input_label;
            send_mac_addr = Uint64ToMac(forwarding_ptr->input_mac_address);
        }
        if (output_label > 0)
        {
            mpls_pk_ptr->setLabel(output_label);
        }
        else
        {
            mpls_pk_ptr->setType (WMPLS_BEGIN);
            mpls_pk_ptr->setLabel(input_label_aux);
        }
        // Enviar al mac
        // polling = wlan_poll_list_member_find (send_mac_addr);
        // wlan_hlpk_enqueue (mpls_pk_ptr, send_mac_addr, polling,false);

        sendOrEnqueue(encapsulate(mpls_pk_ptr,send_mac_addr));
        return;
    }
    else if (forwarding_ptr->order==LWMPLS_EXTRACT)
    {

        if (mpls_pk_ptr->getDest()==myAddress)
        {
            sendUp(decapsulateMpls(mpls_pk_ptr));
            return;
        }
        mplsBasicSend(mpls_pk_ptr,sta_addr);
        return;

#if OMNETPP_VERSION > 0x0400
        if (!(dynamic_cast<LWMPLSPacket*> (mpls_pk_ptr->getEncapsulatedPacket())))
#else
        if (!(dynamic_cast<LWMPLSPacket*> (mpls_pk_ptr->getEncapsulatedMsg())))
#endif
        {
// Source or destination?

            if (sta_addr!= Uint64ToMac (forwarding_ptr->input_mac_address) || forwarding_ptr->mac_address ==0)
            {
                mplsBasicSend(mpls_pk_ptr,sta_addr);
                return;
            }

            output_label = forwarding_ptr->output_label;
            send_mac_addr = Uint64ToMac(forwarding_ptr->mac_address);

            if (output_label>0)
            {
                forwarding_ptr->order=LWMPLS_CHANGE;
                mpls_pk_ptr->setLabel(output_label);
                sendOrEnqueue(encapsulate(mpls_pk_ptr,send_mac_addr));
            }
            else
            {
                mpls_pk_ptr->setLabel (forwarding_ptr->return_label_input);
                if (forwarding_ptr->path.size()>0)
                {
                    mpls_pk_ptr->setType(WMPLS_BEGIN_W_ROUTE);
                    int dist = forwarding_ptr->path.size()-2;
                    mpls_pk_ptr->setVectorAddressArraySize(dist);
                    for (int i =0; i<dist; i++)
                        mpls_pk_ptr->setVectorAddress(i,Uint64ToMac(forwarding_ptr->path[i+1]));
                }
                else
                    mpls_pk_ptr->setType(WMPLS_BEGIN);

                sendOrEnqueue(encapsulate(mpls_pk_ptr,send_mac_addr));
            }
        }
        else
        {
#if OMNETPP_VERSION > 0x0400
            if (dynamic_cast<LWMPLSPacket*>(mpls_pk_ptr->getEncapsulatedPacket ()))
#else
            if (dynamic_cast<LWMPLSPacket*>(mpls_pk_ptr->getEncapsulatedMsg ()))
#endif
            {
                LWMPLSPacket *seg_pkptr =  dynamic_cast<LWMPLSPacket*>(mpls_pk_ptr->decapsulate());
                seg_pkptr->setTTL(mpls_pk_ptr->getTTL());
                delete mpls_pk_ptr;
                mplsDataProcess((LWMPLSPacket*)seg_pkptr,sta_addr);
            }
            else
                delete mpls_pk_ptr;
        }
        // printf("To application %d normal %f \n",time);
    }
}

void Ieee80211Mesh::mplsAckPath(int label,LWMPLSPacket *mpls_pk_ptr,MACAddress sta_addr)
{
    //   printf("ack %f\n",time);
    int label_out = mpls_pk_ptr->getLabelReturn ();

    /* es necesario ahora introducir los datos en la tabla */
    LWmpls_Forwarding_Structure * forwarding_ptr = mplsData->lwmpls_forwarding_data(label,0,0);

// Intermediate node


    int *labelOutPtr;
    int *labelInPtr;


    if (Uint64ToMac(forwarding_ptr->mac_address)==sta_addr)
    {
        labelOutPtr = &forwarding_ptr->output_label;
        labelInPtr = &forwarding_ptr->return_label_input;
    }
    else if (Uint64ToMac(forwarding_ptr->input_mac_address)==sta_addr)
    {
        labelOutPtr = &forwarding_ptr->return_label_output;
        labelInPtr = &forwarding_ptr->input_label;
    }
    else
    {
        delete mpls_pk_ptr;
        return;
    }

    if (*labelOutPtr==0)
    {
        *labelOutPtr=label_out;
        mplsData->lwmpls_forwarding_output_data_add(label_out,MacToUint64(sta_addr),forwarding_ptr,false);
    }
    else
    {
        if (*labelOutPtr!=label_out)
        {
            /* change of label */
            // prg_string_hash_table_item_remove (lwmpls_data_ptr->forwarding_table_output,forwarding_ptr->key_output);
            *labelOutPtr=label_out;
            mplsData->lwmpls_forwarding_output_data_add(label_out,MacToUint64(sta_addr),forwarding_ptr,false);
        }
    }

    forwarding_ptr->last_use=simTime();
    /* initialize the mac timer */
// init the
    LWmpls_Interface_Structure *interface=NULL;
    mplsData->lwmpls_init_interface(&interface,*labelInPtr,MacToUint64(sta_addr),LWMPLS_INPUT_LABEL_RETURN);
    mplsInitializeCheckMac ();

    if (forwarding_ptr->return_label_output>0 && forwarding_ptr->output_label>0)
        forwarding_ptr->order=LWMPLS_CHANGE;

    delete mpls_pk_ptr;
}

void Ieee80211Mesh::mplsDataProcess(LWMPLSPacket * mpls_pk_ptr,MACAddress sta_addr)
{
    int label;
    LWmpls_Forwarding_Structure *forwarding_ptr=NULL;
    bool         label_found;
    int code;
    simtime_t     time;

    /* First check for the case where the received segment contains the */
    /* entire data packet, i.e. the data is transmitted as a single     */
    /* fragment.*/
    time = simTime();
    code = mpls_pk_ptr->getType();
    label = mpls_pk_ptr->getLabel();
    bool is_source = false;
    label_found = true;

    if (code==WMPLS_ACK && mpls_pk_ptr->getDest()!=myAddress)
    {
        delete mpls_pk_ptr;
        return;
    }
    // printf("code %i my_address %d org %d lin %d %f \n",code,my_address,sta_addr,label,op_sim_time());
    bool testMplsData = (code!=WMPLS_BEGIN) && (code!=WMPLS_NOTFOUND) &&
                        (code!= WMPLS_BEGIN_W_ROUTE) && (code!=WMPLS_SEND) &&
                        (code!=WMPLS_BROADCAST) && (code!=WMPLS_ANNOUNCE_GATEWAY) && (code!=WMPLS_REQUEST_GATEWAY); // broadcast code

    if (testMplsData)
    {
        if ((code ==WMPLS_REFRES) && (label==0))
        {
            /* In this case the refresh message is used for refresh the mac connections */
            delete mpls_pk_ptr;
            // printf("refresh %f\n",time);
            // printf("fin 1 %i \n",code);
            return;
        }
        if (label>0)
        {
            if ((forwarding_ptr = mplsData->lwmpls_forwarding_data(label,0,0))!=NULL)
            {
                if  (code == WMPLS_NORMAL)
                {
                    if (!is_source)
                    {
                        if (forwarding_ptr->input_label ==label && forwarding_ptr->input_mac_address!=sta_addr)
                            forwarding_ptr = NULL;
                        else if (forwarding_ptr->return_label_input ==label && forwarding_ptr->mac_address!=sta_addr)
                            forwarding_ptr = NULL;
                    }
                }
            }
            //printf (" %p \n",forwarding_ptr);
            if (forwarding_ptr == NULL)
                label_found = false;
        }

        if (!label_found)
        {
            if ((code ==WMPLS_NORMAL))
                mplsBasicSend ((LWMPLSPacket*)mpls_pk_ptr->dup(),sta_addr);
            if (code !=WMPLS_ACK)
                delete mpls_pk_ptr->decapsulate();

            // � es necesario destruir label_msg_ptr? mirar la memoria
            //op_pk_nfd_set_ptr (seg_pkptr, "pointer", label_msg_ptr);
            mpls_pk_ptr->setType(WMPLS_NOTFOUND);
            // Enviar el mensaje al mac
            //polling = wlan_poll_list_member_find (sta_addr);
            // wlan_hlpk_enqueue (mpls_pk_ptr, sta_addr, polling,true);
            sendOrEnqueue(encapsulate(mpls_pk_ptr,sta_addr));
            return;
        }
    }

    switch (code)
    {

    case WMPLS_NORMAL:
        mplsForwardData(label,mpls_pk_ptr,sta_addr,forwarding_ptr);
        break;

    case WMPLS_BEGIN:
    case WMPLS_BEGIN_W_ROUTE:
        mplsCreateNewPath(label,mpls_pk_ptr,sta_addr);
        break;

    case WMPLS_REFRES:
        // printf("refresh %f\n",time);
        forwarding_ptr->last_use=simTime();
        if (forwarding_ptr->order==LWMPLS_CHANGE)
        {
            if (!(Uint64ToMac(forwarding_ptr->mac_address).isUnspecified()))
            {
                mpls_pk_ptr->setLabel(forwarding_ptr->output_label);
                sendOrEnqueue(encapsulate(mpls_pk_ptr,Uint64ToMac(forwarding_ptr->mac_address)));
            }
            else
                delete mpls_pk_ptr;
        }
        else if (forwarding_ptr->order==LWMPLS_EXTRACT)
        {
            delete mpls_pk_ptr;
        }
        break;

    case WMPLS_END:
        break;

    case WMPLS_BREAK:
        mplsBreakPath (label,mpls_pk_ptr,sta_addr);
        break;

    case WMPLS_NOTFOUND:
        mplsNotFoundPath(label,mpls_pk_ptr,sta_addr);
        break;

    case WMPLS_ACK:
        mplsAckPath(label,mpls_pk_ptr,sta_addr);
        break;
    case WMPLS_SEND:
        mplsBasicSend (mpls_pk_ptr,sta_addr);
        break;
    case WMPLS_ADITIONAL:
        break;
    case WMPLS_BROADCAST:
    case WMPLS_ANNOUNCE_GATEWAY:
    case WMPLS_REQUEST_GATEWAY:
        uint32_t cont;
        uint32_t newCounter = mpls_pk_ptr->getCounter();
        if (mpls_pk_ptr->getSource()==myAddress)
        {
            // los paquetes propios deben ser borrados
            delete mpls_pk_ptr;
            return;
        }

        if (mplsData->getBroadCastCounter(MacToUint64 (mpls_pk_ptr->getSource()),cont))
        {
            if (newCounter==cont)
            {
                delete mpls_pk_ptr;
                return;
            }
            else if (newCounter < cont) //
            {
                if (!(cont > UINT32_MAX-100 && newCounter<100)) // Dado la vuelta
                {
                    delete mpls_pk_ptr;
                    return;
                }
            }
        }
        mplsData->setBroadCastCounter(MacToUint64(mpls_pk_ptr->getSource()),newCounter);
        // send up and Resend
        if (code==WMPLS_BROADCAST)
        {
#if OMNETPP_VERSION > 0x0400
             sendUp(mpls_pk_ptr->getEncapsulatedPacket()->dup());
#else
             sendUp(mpls_pk_ptr->getEncapsulatedMsg()->dup());
#endif
        }
        else
            processControlPacket (dynamic_cast<LWMPLSControl*>(mpls_pk_ptr));
//        sendOrEnqueue(encapsulate(mpls_pk_ptr,MACAddress::BROADCAST_ADDRESS));
//       small random delay. Avoid the collision
        Ieee80211DataFrame *meshFrame = encapsulate(mpls_pk_ptr,MACAddress::BROADCAST_ADDRESS);
        scheduleAt(simTime()+par("MacBroadcastDelay"),meshFrame);
        break;
    }
}


/* clean the path and create the message WMPLS_BREAK and send */
void Ieee80211Mesh::mplsBreakMacLink (MACAddress macAddress)
{
    LWmpls_Forwarding_Structure *forwarding_ptr;

    uint64_t des_add;
    int out_label;
    uint64_t mac_id;
    mac_id = MacToUint64(macAddress);


    LWmpls_Interface_Structure * mac_ptr = mplsData->lwmpls_interface_structure(mac_id);
    if (!mac_ptr)
        return;

// Test para evitar falsos positivos por colisiones
    if ((simTime()-mac_ptr->lastUse())<mplsData->mplsMacLimit())
        return;

    int numRtr= mac_ptr->numRtr();

    if (numRtr<mplsData->mplsMaxMacRetry ())
    {
        mac_ptr->numRtr()=numRtr+1;
        return;
    }

    LWmplsInterfaceMap::iterator it = mplsData->interfaceMap->find(mac_id);
    if (it!= mplsData->interfaceMap->end())
        if (!it->second->numLabels())
        {
            delete it->second;
            mplsData->interfaceMap->erase(it);
        }

    for (unsigned int i = 1; i< mplsData->label_list.size(); i++)
    {
        forwarding_ptr = mplsData->lwmpls_forwarding_data (i,0,0);
        if (forwarding_ptr!=NULL)
        {
            if ((forwarding_ptr->mac_address == mac_id) || (forwarding_ptr->input_mac_address == mac_id))
            {
                mplsPurge (forwarding_ptr,true);
                /* prepare and send break message */
                if (forwarding_ptr->input_mac_address == mac_id)
                {
                    des_add = forwarding_ptr->mac_address;
                    out_label = forwarding_ptr->output_label;
                }
                else
                {
                    des_add = forwarding_ptr->input_mac_address;
                    out_label = forwarding_ptr->return_label_output;
                }
                if (des_add!=0)
                {
                    LWMPLSPacket *lwmplspk = new LWMPLSPacket;
                    lwmplspk->setType(WMPLS_BREAK);
                    lwmplspk->setLabel(out_label);
                    sendOrEnqueue(encapsulate(lwmplspk,Uint64ToMac(des_add)));
                }
                mplsData->deleteForwarding(forwarding_ptr);
                forwarding_ptr=NULL;
            }
        }
    }
}


void Ieee80211Mesh::mplsCheckRouteTime ()
{
    simtime_t actual_time;
    bool active = false;
    LWmpls_Forwarding_Structure *forwarding_ptr;
    int out_label;
    uint64_t mac_id;
    uint64_t des_add;

    actual_time = simTime();

    LWmplsInterfaceMap::iterator it;

    for ( it=mplsData->interfaceMap->begin() ; it != mplsData->interfaceMap->end();)
    {
        if ((actual_time - it->second->lastUse()) < (multipler_active_break*timer_active_refresh))
        {
            it++;
            continue;
        }

        mac_id = it->second->macAddress();
        delete it->second;
        mplsData->interfaceMap->erase(it);
        it = mplsData->interfaceMap->begin();
        if (mac_id==0)
            continue;

        for (unsigned int i = 1; i< mplsData->label_list.size(); i++)
        {
            forwarding_ptr = mplsData->lwmpls_forwarding_data (i,0,0);
            if (forwarding_ptr && (mac_id == forwarding_ptr->mac_address || mac_id == forwarding_ptr->input_mac_address))
            {
                mplsPurge (forwarding_ptr,true);
                /* prepare and send break message */
                if (forwarding_ptr->input_mac_address == mac_id)
                {
                    des_add = forwarding_ptr->mac_address;
                    out_label = forwarding_ptr->output_label;
                }
                else
                {
                    des_add = forwarding_ptr->input_mac_address;
                    out_label = forwarding_ptr->return_label_output;
                }
                if (des_add!=0)
                {
                    LWMPLSPacket *lwmplspk = new LWMPLSPacket;
                    lwmplspk->setType(WMPLS_BREAK);
                    lwmplspk->setLabel(out_label);
                    sendOrEnqueue(encapsulate(lwmplspk,Uint64ToMac(des_add)));
                }
                mplsData->deleteForwarding (forwarding_ptr);
            }
        }


    }

    if (mplsData->lwmpls_nun_labels_in_use ()>0)
        active=true;

    if (activeMacBreak &&  active && WMPLSCHECKMAC)
    {
        if (!WMPLSCHECKMAC->isScheduled())
            scheduleAt (actual_time+(multipler_active_break*timer_active_refresh),WMPLSCHECKMAC);
    }
}


void Ieee80211Mesh::mplsInitializeCheckMac ()
{
    int list_size;
    bool active = false;

    if (WMPLSCHECKMAC==NULL)
       return;
    if (activeMacBreak == false)
        return ;

    list_size = mplsData->lwmpls_nun_labels_in_use ();

    if (list_size>0)
        active=true;

    if (active ==true)
    {
        if (!WMPLSCHECKMAC->isScheduled())
            scheduleAt (simTime()+(multipler_active_break*timer_active_refresh),WMPLSCHECKMAC);
    }
    return;
}


void Ieee80211Mesh::mplsPurge (LWmpls_Forwarding_Structure *forwarding_ptr,bool purge_break)
{
// �Como? las colas estan en otra parte.
    bool purge;

    if (forwarding_ptr==NULL)
        return;

    for ( cQueue::Iterator iter(dataQueue,1); !iter.end();)
    {
        cMessage *msg = (cMessage *) iter();
        purge=false;
        Ieee80211DataFrame *frame =  dynamic_cast<Ieee80211DataFrame*> (msg);
        if (frame==NULL)
        {
            iter++;
            continue;
        }
#if OMNETPP_VERSION > 0x0400
        LWMPLSPacket* mplsmsg = dynamic_cast<LWMPLSPacket*>(frame->getEncapsulatedPacket());
#else
        LWMPLSPacket* mplsmsg = dynamic_cast<LWMPLSPacket*>(frame->getEncapsulatedMsg());
#endif

        if (mplsmsg!=NULL)
        {
            int label = mplsmsg->getLabel();
            int code = mplsmsg->getType();
            if (label ==0)
            {
                iter++;
                continue;
            }
            if (code==WMPLS_NORMAL)
            {
                if ((forwarding_ptr->output_label==label &&  frame->getReceiverAddress() ==
                        Uint64ToMac(forwarding_ptr->mac_address)) ||
                        (forwarding_ptr->return_label_output==label && frame->getReceiverAddress() ==
                         Uint64ToMac(forwarding_ptr->input_mac_address)))
                    purge = true;
            }
            else if ((code==WMPLS_BEGIN) &&(purge_break==true))
            {
                if (forwarding_ptr->return_label_input==label &&  frame->getReceiverAddress() ==
                        Uint64ToMac(forwarding_ptr->mac_address))
                    purge = true;
            }
            else if ((code==WMPLS_BEGIN) &&(purge_break==false))
            {
                if (forwarding_ptr->output_label>0)
                    if (forwarding_ptr->return_label_input==label &&  frame->getReceiverAddress() ==
                            Uint64ToMac(forwarding_ptr->mac_address))
                        purge = true;
            }
            if (purge == true)
            {
                dataQueue.remove(msg);
                mplsmsg = dynamic_cast<LWMPLSPacket*>(decapsulate(frame));
                delete msg;
                if (mplsmsg)
                {
                    MACAddress prev;
                    mplsBasicSend(mplsmsg,prev);
                }
                else
                    delete mplsmsg;
                iter.init(dataQueue,1);
                continue;
            }
            else
                iter++;
        }
        else
            iter++;
    }
}


// Cada ver que se envia un mensaje sirve para generar mensajes de permanencia. usa los propios hellos para garantizar que se env�an mensajes

void Ieee80211Mesh::sendOut(cMessage *msg)
{
    //InterfaceEntry *ie = ift->getInterfaceById(msg->getKind());
    msg->setKind(0);
    //send(msg, macBaseGateId + ie->getNetworkLayerGateIndex());
    send(msg, "macOut",0);
}


//
// mac label address method
// Equivalent to the 802.11s forwarding mechanism
//

bool Ieee80211Mesh::forwardMessage (Ieee80211DataFrame *frame)
{
#if OMNETPP_VERSION > 0x0400
    cPacket *msg = frame->getEncapsulatedPacket();
#else
    cPacket *msg = frame->getEncapsulatedMsg();
#endif
    LWMPLSPacket *lwmplspk = dynamic_cast<LWMPLSPacket*> (msg);

    if (lwmplspk)
        return false;
    if ((routingModuleProactive != NULL) && (routingModuleProactive->isOurType(msg)))
        return false;
    else if ((routingModuleReactive != NULL) && routingModuleReactive->isOurType(msg))
        return false;
    else // Normal frame test if use the mac label address method
        return macLabelBasedSend(frame);

}

bool Ieee80211Mesh::macLabelBasedSend (Ieee80211DataFrame *frame)
{

    if (!frame)
        return false;

    if (isGateWay)
    {
        // if this is gateway and the frame is for other gateway send it
        Ieee80211MeshFrame * frame2 = dynamic_cast<Ieee80211MeshFrame*>(frame);
        if (frame2 && !frame2->getFinalAddress().isUnspecified() && frame2->getFinalAddress()!=frame->getAddress4())
            frame2->setAddress4(frame2->getFinalAddress());

        bool toGateWay=false;
        if (routingModuleReactive)
        {
            if (routingModuleReactive->findInAddressGroup(frame->getAddress4()))
                toGateWay = true;
        }
        else if (routingModuleProactive)
        {
            if (routingModuleProactive->findInAddressGroup(frame->getAddress4()))
                 toGateWay = true;
        }
        if (toGateWay)
            associatedAddress[frame2->getAddress3()]=simTime();
        if (toGateWay && frame->getAddress4()!=myAddress)
        {
            frame2->setTransmitterAddress(myAddress);
            if (!frame2->getReceiverAddress().isBroadcast())
                frame2->setReceiverAddress(frame->getAddress4());
            sendOrEnqueue(frame2);
            return true;
        }
    }

    if (frame->getAddress4()==myAddress || frame->getAddress4().isUnspecified())
        return false;

    uint64_t dest = MacToUint64(frame->getAddress4());
    uint64_t src = MacToUint64(frame->getAddress3());
    uint64_t prev = MacToUint64(frame->getTransmitterAddress());
    uint64_t next = mplsData->getForwardingMacKey(src,dest,prev);
    Ieee80211MeshFrame *frame2  = dynamic_cast<Ieee80211MeshFrame *>(frame);

    double  delay = SIMTIME_DBL(simTime() - frame->getTimestamp());
    if ((frame2 && frame2->getTTL()<=0) || (delay > limitDelay))
    {
        delete frame;
        return true;
    }

    if (next)
    {
        frame->setReceiverAddress(Uint64ToMac(next));
    }
    else
    {
        std::vector<Uint128> add;
        int dist=0;
        int iface;

        bool toGateWay=false;
        if (routingModuleReactive)
        {
            if (routingModuleReactive->findInAddressGroup(dest))
                toGateWay = true;
        }
        else if (routingModuleProactive)
        {
            if (routingModuleProactive->findInAddressGroup(dest))
                 toGateWay = true;
        }
        Uint128 gateWayAddress;
        if (routingModuleProactive)
        {
            add.resize(1);
            if (toGateWay)
            {
                bool isToGw;
                if (routingModuleProactive->getNextHopGroup(dest,add[0],iface,gateWayAddress,isToGw))
                   dist = 1;
            }
            else
            {
                double cost;
                if (routingModuleProactive->getNextHop(dest,add[0],iface,cost))
                    dist = 1;
            }
        }

        if (dist==0 && routingModuleReactive)
        {
            add.resize(1);
            if (toGateWay)
            {
                bool isToGw;
                if (routingModuleReactive->getNextHopGroup(dest,add[0],iface,gateWayAddress,isToGw))
                   dist = 1;
            }
            else
            {
                double cost;
                if (routingModuleReactive->getNextHop(dest,add[0],iface,cost))
                    dist = 1;
            }
        }

        if (dist==0)
        {
// Destination unreachable
            if (routingModuleReactive)
            {
                ControlManetRouting *ctrlmanet = new ControlManetRouting();
                ctrlmanet->setOptionCode(MANET_ROUTE_NOROUTE);
                ctrlmanet->setDestAddress(dest);
                //  ctrlmanet->setSrcAddress(myAddress);
                ctrlmanet->setSrcAddress(src);
                ctrlmanet->encapsulate(frame);
                frame = NULL;
                send(ctrlmanet,"routingOutReactive");
            }
            else
            {
                delete frame;
                frame=NULL;
            }
        }
        else
        {
            frame->setReceiverAddress(add[0].getMACAddress());
        }

    }
    //send(msg, macBaseGateId + ie->getNetworkLayerGateIndex());
    if (frame)
        sendOrEnqueue(frame);
    return true;
}

void Ieee80211Mesh::sendUp(cMessage *msg)
{
    if (isUpperLayer(msg))
        send(msg, "uppergateOut");
    else
        delete msg;
}

bool Ieee80211Mesh::isUpperLayer(cMessage *msg)
{
    if (dynamic_cast<IPDatagram*>(msg))
        return true;
    else if (dynamic_cast<IPv6Datagram*>(msg))
        return true;
    else if (dynamic_cast<OSPFPacket*>(msg))
        return true;
    else if (dynamic_cast<MPLSPacket*>(msg))
        return true;
    else if (dynamic_cast<LinkStateMsg*>(msg))
        return true;
    else if (dynamic_cast<ARPPacket*>(msg))
        return true;
    return false;
}

cPacket *Ieee80211Mesh::decapsulate(Ieee80211DataFrame *frame)
{
    cPacket *payload = frame->decapsulate();
    Ieee802Ctrl *ctrl = new Ieee802Ctrl();
    ctrl->setSrc(frame->getTransmitterAddress());
    ctrl->setDest(frame->getReceiverAddress());
    payload->setControlInfo(ctrl);
    delete frame;
    return payload;
}

void Ieee80211Mesh::actualizeReactive(cPacket *pkt,bool out)
{
    Uint128 dest,prev,next,src;
    if (!routingModuleReactive)
        return;

    Ieee80211DataFrame * frame = dynamic_cast<Ieee80211DataFrame*>(pkt);

    if (!frame )
        return;
/*
    if (!out)
        return;
*/
    /*
        if (frame->getAddress4().isUnspecified() || frame->getAddress4().isBroadcast())
            return;

        ControlManetRouting *ctrlmanet = new ControlManetRouting();
        dest=frame->getAddress4();
        src=frame->getAddress3();
        ctrlmanet->setOptionCode(MANET_ROUTE_UPDATE);
        ctrlmanet->setDestAddress(dest);
        ctrlmanet->setSrcAddress(src);
        send(ctrlmanet,"routingOutReactive");
        return;
    */

    if (out)
    {
        if (!frame->getAddress4().isUnspecified() && !frame->getAddress4().isBroadcast())
            dest=frame->getAddress4();
        else
            return;
        if (!frame->getReceiverAddress().isUnspecified() && !frame->getReceiverAddress().isBroadcast())
            next=frame->getReceiverAddress();
        else
            return;

    }
    else
    {
        if (!frame->getAddress3().isUnspecified() && !frame->getAddress3().isBroadcast() )
            dest=frame->getAddress3();
        else
            return;
        if (!frame->getTransmitterAddress().isUnspecified() && !frame->getTransmitterAddress().isBroadcast())
            prev=frame->getTransmitterAddress();
        else
            return;

    }
    routingModuleReactive->setRefreshRoute(src,dest,next,prev);
}


void Ieee80211Mesh::sendOrEnqueue(cPacket *frame)
{
    Ieee80211MeshFrame * frameAux = dynamic_cast<Ieee80211MeshFrame*>(frame);
    if (frameAux && frameAux->getTTL()<=0)
    {
        delete frame;
        return;
    }
    // Check if the destination is other gateway if true send to it
    if (isGateWay)
    {
        if (frameAux &&  (frame->getControlInfo()==NULL || !dynamic_cast<MeshControlInfo*>(frame->getControlInfo())))
        {
            GateWayDataMap::iterator it;
            frameAux->setTransmitterAddress(myAddress);
            if (frameAux->getReceiverAddress().isBroadcast())
            {
                MACAddress origin;
                if (dynamic_cast<LWMPLSPacket*> (frameAux->getEncapsulatedPacket()))
                {
                    int code = dynamic_cast<LWMPLSPacket*> (frameAux->getEncapsulatedPacket())->getType();
                    if (code==WMPLS_BROADCAST || code == WMPLS_ANNOUNCE_GATEWAY || code== WMPLS_REQUEST_GATEWAY)
                        origin=dynamic_cast<LWMPLSPacket*> (frameAux->getEncapsulatedPacket())->getSource();
                }
                for (it=getGateWayDataMap()->begin();it!=getGateWayDataMap()->end();it++)
                {
                    if (it->second.idAddress==myAddress || it->second.idAddress==origin)
                        continue;
                    MeshControlInfo *ctrl = new MeshControlInfo();
                    ctrl->setSrc(MACAddress::UNSPECIFIED_ADDRESS); // the Ethernet will fill the field
                    //ctrl->setDest(frameAux->getReceiverAddress());
                    ctrl->setDest(it->second.ethAddress);
                    cPacket *pktAux=frameAux->dup();
                    pktAux->setControlInfo(ctrl);
                    //sendDirect(pktAux,it->second.gate);
                    send(pktAux,"toEthernet");
                }
            }
            else
            {
                it = getGateWayDataMap()->find((Uint128)frameAux->getAddress4());
                if (it!=getGateWayDataMap()->end())
                {
                    MeshControlInfo *ctrl = new MeshControlInfo();
                    ctrl->setSrc(MACAddress::UNSPECIFIED_ADDRESS);  // the Ethernet will fill the field
                    //ctrl->setDest(frameAux->getReceiverAddress());
                    ctrl->setDest(it->second.ethAddress);
                    frameAux->setControlInfo(ctrl);
                    actualizeReactive(frameAux,true);
// TODO: fragment packets before send to ethernet
                    //sendDirect(frameAux,5e-6,frameAux->getBitLength()/1e9,it->second.gate);
                    send(frameAux,"toEthernet");
                    return;
                }
                it = getGateWayDataMap()->find((Uint128)frameAux->getReceiverAddress());
            	if (it!=getGateWayDataMap()->end())
            	{
            	    MeshControlInfo *ctrl = new MeshControlInfo();
            	    //ctrl->setSrc(myAddress);
            	    ctrl->setSrc(MACAddress::UNSPECIFIED_ADDRESS);  // the Ethernet will fill the field
            	    //ctrl->setDest(frameAux->getReceiverAddress());
            	    ctrl->setDest(it->second.ethAddress);
            	    frameAux->setControlInfo(ctrl);
            	    actualizeReactive(frameAux,true);
            	    // sendDirect(frameAux,5e-6,frameAux->getBitLength()/1e9,it->second.gate);
            	    send(frameAux,"toEthernet");
            	    return;
            	}
            }
        }
    }
    actualizeReactive(frame,true);
    PassiveQueueBase::handleMessage(frame);
}

#if 0
void Ieee80211Mesh::sendOrEnqueue(cPacket *frame)
{
    Ieee80211MeshFrame * frameAux = dynamic_cast<Ieee80211MeshFrame*>(frame);
    if (frameAux && frameAux->getTTL()<=0)
    {
        delete frame;
        return;
    }
    // Check if the destination is other gateway if true send to it
    if (isGateWay)
    {
        if (frameAux &&  (frame->getControlInfo()==NULL || !dynamic_cast<MeshControlInfo*>(frame->getControlInfo())))
        {
            GateWayDataMap::iterator it;
            if (frameAux->getReceiverAddress().isBroadcast())
            {
                MACAddress origin;
                if (dynamic_cast<LWMPLSPacket*> (frameAux->getEncapsulatedPacket()))
                {
                    int code = dynamic_cast<LWMPLSPacket*> (frameAux->getEncapsulatedPacket())->getType();
                    if (code==WMPLS_BROADCAST || code == WMPLS_ANNOUNCE_GATEWAY || code== WMPLS_REQUEST_GATEWAY)
                        origin=dynamic_cast<LWMPLSPacket*> (frameAux->getEncapsulatedPacket())->getSource();
                }
                for (it=getGateWayDataMap()->begin();it!=getGateWayDataMap()->end();it++)
                {
                    if (it->second.idAddress==myAddress || it->second.idAddress==origin)
                        continue;
                    MeshControlInfo *ctrl = new MeshControlInfo();
                    ctrl->setSrc(myAddress);
                    ctrl->setDest(frameAux->getReceiverAddress());
                    cPacket *pktAux=frameAux->dup();
                    pktAux->setControlInfo(ctrl);
                    //sendDirect(pktAux,it->second.gate);
                    sendDirect(pktAux,5e-6,pktAux->getBitLength()/1e9,it->second.gate);
                }
            }
            else
            {
                it = getGateWayDataMap()->find((Uint128)frameAux->getAddress4());
                if (it!=getGateWayDataMap()->end())
                {
                    MeshControlInfo *ctrl = new MeshControlInfo();
                    ctrl->setSrc(myAddress);
                    ctrl->setDest(frameAux->getReceiverAddress());
                    frameAux->setControlInfo(ctrl);
                    actualizeReactive(frameAux,true);
                    //sendDirect(frameAux,it->second.gate);
                    sendDirect(frameAux,5e-6,frameAux->getBitLength()/1e9,it->second.gate);
                    return;
                }
                it = getGateWayDataMap()->find((Uint128)frameAux->getReceiverAddress());
                if (it!=getGateWayDataMap()->end())
            	{
            	    MeshControlInfo *ctrl = new MeshControlInfo();
            	    ctrl->setSrc(myAddress);
            	    ctrl->setDest(frameAux->getReceiverAddress());
            	    frameAux->setControlInfo(ctrl);
            	    actualizeReactive(frameAux,true);
            	    //sendDirect(frameAux,it->second.gate);
            	    sendDirect(frameAux,5e-6,frameAux->getBitLength()/1e9,it->second.gate);
            	    return;
            	}
            }
        }
    }
    actualizeReactive(frame,true);
    PassiveQueueBase::handleMessage(frame);
}
#endif

void Ieee80211Mesh::handleEtxMessage(cPacket *pk)
{
    ETXBasePacket * etxMsg = dynamic_cast<ETXBasePacket*>(pk);
    if (etxMsg)
    {
        Ieee80211DataFrame * frame = encapsulate(etxMsg,etxMsg->getDest());
        if (frame)
            sendOrEnqueue(frame);
    }
    else
        delete pk;
}

void Ieee80211Mesh::publishGateWayIdentity()
{
    LWMPLSControl * pkt = new LWMPLSControl();
#ifndef CHEAT_IEEE80211MESH
    cGate * gt=gate("interGateWayConect");
    unsigned char *ptr;
    pkt->setGateAddressPtrArraySize(sizeof(ptr));
    pkt->setAssocAddressPtrArraySize(sizeof(ptr));
    ptr = (unsigned char*)gt;
    for (unsigned int i=0;i<sizeof(ptr);i++)
        pkt->setGateAddressPtr(i,ptr[i]);
    ptr=(unsigned char*) &associatedAddress;
    for (unsigned int i=0;i<sizeof(ptr);i++)
        pkt->setGateAddressPtr(i,ptr[i]);
#endif
    // copy receiver address from the control info (sender address will be set in MAC)
    pkt->setType(WMPLS_ANNOUNCE_GATEWAY);
    Ieee80211MeshFrame *frame = new Ieee80211MeshFrame();
    frame->setReceiverAddress(MACAddress::BROADCAST_ADDRESS);
    frame->setTTL(1);
    uint32_t cont;
    mplsData->getBroadCastCounter(cont);
    cont++;
    mplsData->setBroadCastCounter(cont);
    pkt->setCounter(cont);
    pkt->setSource(myAddress);
    pkt->setDest(MACAddress::BROADCAST_ADDRESS);
    pkt->setType(WMPLS_ANNOUNCE_GATEWAY);
    if (this->getGateWayDataMap() && this->getGateWayDataMap()->size()>0)
    {
        int size = this->getGateWayDataMap()->size();
        pkt->setVectorAddressArraySize(size);
        cont = 0;
        for (GateWayDataMap::iterator it=this->getGateWayDataMap()->begin();it!=this->getGateWayDataMap()->end();it++)
        {
            pkt->setVectorAddress(cont,it->second.idAddress);
            cont++;
            pkt->setByteLength(pkt->getByteLength()+6); // 40 bits address
        }
    }
    frame->encapsulate(pkt);
    double delay=this->getGateWayDataMap()->size()*par("GateWayAnnounceInterval").doubleValue ();
    scheduleAt(simTime()+delay+uniform(0,2),gateWayTimeOut);
    if (frame)
        sendOrEnqueue(frame);
}


void Ieee80211Mesh::processControlPacket (LWMPLSControl *pkt)
{
#ifndef CHEAT_IEEE80211MESH
    if (getGateWayData())
    {
        GateWayData data;
        unsigned char *ptr;
        for (unsigned int i=0;i<sizeof(ptr);i++)
            pkt->getGateAddressPtr(i,ptr[i]);
        data.gate=(cGate*)ptr;
        for (unsigned int i=0;i<sizeof(ptr);i++)
            pkt->getGateAddressPtr(i,ptr[i]);
        data.associatedAddress= (AssociatedAddress *)ptr;
        getGateWayData()->insert(std::pair<Uint128,GateWayData>(pkt->getSource(),data));
    }
#endif
    for (unsigned int i=0;i<pkt->getVectorAddressArraySize();i++)
    {
        if(routingModuleProactive)
            routingModuleProactive->addInAddressGroup(pkt->getVectorAddress(i));
        if (routingModuleReactive)
            routingModuleReactive->addInAddressGroup(pkt->getVectorAddress(i));
    }
}

bool Ieee80211Mesh::selectGateWay(const Uint128 &dest,MACAddress &gateway)
{
    if (!isGateWay)
        return false;
#ifdef CHEAT_IEEE80211MESH
    GateWayDataMap::iterator best=this->getGateWayDataMap()->end();
    double bestCost;
    double myCost=10E10;
    simtime_t timeAsoc=0;
    for (GateWayDataMap::iterator it=this->getGateWayDataMap()->begin();it!=this->getGateWayDataMap()->end();it++)
    {
    	int iface;
    	Uint128 next;
        if (best==this->getGateWayDataMap()->end())
        {
        	bool destinationFind=false;
            if(it->second.proactive && it->second.proactive->getNextHop(dest,next,iface,bestCost))
            	destinationFind=true;
            else if(it->second.reactive && it->second.reactive->getNextHop(dest,next,iface,bestCost))
            	destinationFind=true;
            if (destinationFind)
            {
                if (it->second.idAddress==myAddress)
                    myCost=bestCost;
                best=it;
                AssociatedAddress::iterator itAssoc;
                itAssoc = best->second.associatedAddress->find(dest);
                if (itAssoc!=best->second.associatedAddress->end())
                    timeAsoc=itAssoc->second;
            }
        }
        else
        {
        	int iface;
        	Uint128 next;
        	double cost;
        	bool destinationFind=false;

            if(it->second.proactive && it->second.proactive->getNextHop(dest,next,iface,cost))
            	destinationFind=true;
            else if(it->second.reactive && it->second.reactive->getNextHop(dest,next,iface,cost))
            	destinationFind=true;
            if (destinationFind)
            {
                if (it->second.idAddress==myAddress)
                    myCost=cost;
                if (cost<bestCost)
                {
                    best=it;
                    bestCost=cost;
                    best=it;
                    AssociatedAddress::iterator itAssoc;
                    itAssoc = best->second.associatedAddress->find(dest);
                    if (itAssoc!=best->second.associatedAddress->end())
                        timeAsoc=itAssoc->second;
                    else
                        timeAsoc=0;
                }
                else if (cost==bestCost)
                {
                    AssociatedAddress::iterator itAssoc;
                    itAssoc = best->second.associatedAddress->find(dest);
                    if (itAssoc!=best->second.associatedAddress->end())
                    {
                        if (timeAsoc==0 || timeAsoc>itAssoc->second)
                        {
                   	        best=it;
                   	        timeAsoc=itAssoc->second;
                        }
                    }
                }
            }
        }
    }
    if (best!=this->getGateWayDataMap()->end())
    {
    	// check my address
    	if (myCost<=bestCost && (timeAsoc==0 || timeAsoc>10))
            gateway=myAddress;
    	else
            gateway=best->second.idAddress;
        return true;
    }
    else
        return false;
#endif
}
//
// TODO : Hacer que los gateway se comporten como un gran nodo �nico. Si llega un rreq este lo retransmite no solo �l sino tambien los otros, f�cil en reactivo
// necesario pensar en proactivo.
//
void Ieee80211Mesh::handleWateGayDataReceive(cPacket *pkt)
{
    Ieee80211MeshFrame *frame2  = dynamic_cast<Ieee80211MeshFrame *>(pkt);
    cPacket *encapPkt=NULL;
    encapPkt = pkt->getEncapsulatedPacket();
    if ((routingModuleProactive != NULL) && (routingModuleProactive->isOurType(encapPkt)))
    {
        //sendDirect(msg,0, routingModule, "from_ip");
        Ieee802Ctrl *ctrl = check_and_cast<Ieee802Ctrl*>(pkt->removeControlInfo());
        encapPkt = pkt->decapsulate();
        MeshControlInfo *controlInfo = new MeshControlInfo;
        Ieee802Ctrl *ctrlAux = controlInfo;
        *ctrlAux=*ctrl;
        if (frame2->getReceiverAddress().isUnspecified())
        	opp_error("transmitter address is unspecified");
        else if (frame2->getReceiverAddress() != myAddress && !frame2->getReceiverAddress().isBroadcast())
        	opp_error("bad address");
        else
        	controlInfo->setDest(frame2->getReceiverAddress());

        for (GateWayDataMap::iterator it=gateWayDataMap->begin();it!=gateWayDataMap->end();it++)
        {
            if (ctrl->getSrc()==it->second.ethAddress)
                controlInfo->setSrc(it->first);
        }
        delete ctrl;
        encapPkt->setControlInfo(controlInfo);
        send(encapPkt,"routingOutProactive");
        delete pkt;
        return;
    }
    else if ((routingModuleReactive != NULL) && routingModuleReactive->isOurType(encapPkt))
    {
        Ieee802Ctrl *ctrl = check_and_cast<Ieee802Ctrl*>(pkt->removeControlInfo());
        encapPkt = pkt->decapsulate();
        MeshControlInfo *controlInfo = new MeshControlInfo;
        Ieee802Ctrl *ctrlAux = controlInfo;
        *ctrlAux=*ctrl;
        if (frame2->getReceiverAddress().isUnspecified())
        	opp_error("transmitter address is unspecified");
        else if (frame2->getReceiverAddress() != myAddress && !frame2->getReceiverAddress().isBroadcast())
        	opp_error("bad address");
        else
        	controlInfo->setDest(frame2->getReceiverAddress());

        for (GateWayDataMap::iterator it=gateWayDataMap->begin();it!=gateWayDataMap->end();it++)
        {
            if (ctrl->getSrc()==it->second.ethAddress)
                controlInfo->setSrc(it->first);
        }
        delete ctrl;
        Uint128 dest;
        encapPkt->setControlInfo(controlInfo);
        if (routingModuleReactive->getDestAddress(encapPkt,dest))
        {
            std::vector<Uint128>add;
            Uint128 src = controlInfo->getSrc();
            int dist = 0;
            if (routingModuleProactive && proactiveFeedback)
            {
                // int neig = routingModuleProactive))->getRoute(src,add);
                controlInfo->setPreviousFix(true); // This node is fix
                dist = routingModuleProactive->getRoute(dest,add);
            }
            else
                controlInfo->setPreviousFix(false); // This node is not fix
            if (maxHopProactive>0 && dist>maxHopProactive)
                dist = 0;
            if (dist!=0 && proactiveFeedback)
            {
                controlInfo->setVectorAddressArraySize(dist);
                for (int i=0; i<dist; i++)
                    controlInfo->setVectorAddress(i,add[i]);
            }
        }
        send(encapPkt,"routingOutReactive");
        delete pkt;
        return;
    }

    if(isGateWay && frame2)
    {
        if (frame2->getFinalAddress()==myAddress)
        {
            cPacket *msg = decapsulate(frame2);
            if (dynamic_cast<ETXBasePacket*>(msg))
            {
                if (ETXProcess)
                {
                    if (msg->getControlInfo())
                        delete msg->removeControlInfo();
                    send(msg,"ETXProcOut");
                }
                else
                    delete msg;
                return;
            }
            else if (dynamic_cast<LWMPLSPacket*> (msg))
            {
                LWMPLSPacket *lwmplspk = dynamic_cast<LWMPLSPacket*> (msg);
                encapPkt = decapsulateMpls(lwmplspk);
            }
            else
                encapPkt=msg;
            if (encapPkt)
                sendUp(encapPkt);
        }
        else if (!frame2->getFinalAddress().isUnspecified())
        {
            handleReroutingGateway(frame2);
        }
        else
        {
            frame2->setTTL(frame2->getTTL()-1);
            actualizeReactive(frame2,false);
            processFrame(frame2);
        }
    }
    else
        delete pkt;
}

void Ieee80211Mesh::handleReroutingGateway(Ieee80211DataFrame *pkt)
{
    handleDataFrame(pkt);
}
