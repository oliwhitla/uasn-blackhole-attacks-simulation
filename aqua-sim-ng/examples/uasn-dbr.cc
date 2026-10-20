/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * CSC 466 project: baseline DBR scenario (no bad nodes yet).
 *
 * Layout (z = height above the seafloor, so bigger z = shallower):
 *   - one source on the seafloor  at (width/2, length/2, 0)
 *   - one sink   at the surface   at (width/2, length/2, depth)
 *   - `relays` relay nodes placed uniformly at random in the box
 *
 * The source sends `packets` packets, one every `interval` seconds, to the sink.
 * DBR forwards them upward. At the end we print:
 *   sent, delivered (unique packets at the sink), delivery ratio,
 *   average delay (first copy to arrive), transmissions, and
 *   transmission cost (transmissions / delivered).
 *
 * Example:
 *   ./ns3 run "UasnDbr --relays=100 --range=100 --packets=50 --RngRun=1"
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/energy-module.h"
#include "ns3/aqua-sim-ng-module.h"

#include <map>
#include <set>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("UasnDbr");

namespace
{
std::map<uint64_t, Time> g_sendTime; // packet UID -> time the source sent it
std::set<uint64_t> g_delivered;      // UIDs that reached the sink at least once
uint64_t g_duplicates = 0;           // extra copies that reached the sink
uint64_t g_unmatched = 0;            // sink packets we had no send time for
uint64_t g_txCount = 0;              // every routing-layer transmission, any node
Time g_totalDelay = Seconds(0);

void
SinkRx(Ptr<const Packet> p)
{
  uint64_t uid = p->GetUid();
  if (!g_delivered.insert(uid).second)
    {
      g_duplicates++;
      return;
    }
  auto it = g_sendTime.find(uid);
  if (it != g_sendTime.end())
    {
      g_totalDelay += Simulator::Now() - it->second;
    }
  else
    {
      g_unmatched++;
    }
}

void
RoutingTx(Ptr<const Packet> p)
{
  g_txCount++;
}

void
SendOne(Ptr<Socket> socket, uint32_t size, Time interval, uint32_t remaining)
{
  Ptr<Packet> p = Create<Packet>(size);
  g_sendTime[p->GetUid()] = Simulator::Now();
  if (socket->Send(p) < 0)
    {
      NS_LOG_WARN("Source failed to send packet " << p->GetUid());
    }
  if (remaining > 1)
    {
      Simulator::Schedule(interval, &SendOne, socket, size, interval, remaining - 1);
    }
}
} // namespace

int
main(int argc, char* argv[])
{
  uint32_t relays = 100;
  double width = 300;    // x extent (m)
  double length = 300;   // y extent (m)
  double depth = 500;    // z extent (m), seafloor to surface
  double range = 100;    // acoustic transmission range (m)
  uint32_t packets = 50; // packets the source sends
  double interval = 10;  // seconds between packets
  uint32_t packetSize = 32;
  double drain = 60;     // extra seconds after the last send

  CommandLine cmd;
  cmd.AddValue("relays", "Number of relay nodes", relays);
  cmd.AddValue("width", "Box width in x (m)", width);
  cmd.AddValue("length", "Box length in y (m)", length);
  cmd.AddValue("depth", "Box depth in z (m)", depth);
  cmd.AddValue("range", "Transmission range (m)", range);
  cmd.AddValue("packets", "Packets sent by the source", packets);
  cmd.AddValue("interval", "Seconds between packets", interval);
  cmd.AddValue("packetSize", "Packet size (bytes)", packetSize);
  cmd.AddValue("drain", "Seconds to keep running after the last send", drain);
  cmd.Parse(argc, argv);

  // Nodes: relays [0, relays), then the source, then the sink.
  NodeContainer relayNodes;
  relayNodes.Create(relays);
  Ptr<Node> source = CreateObject<Node>();
  Ptr<Node> sink = CreateObject<Node>();
  NodeContainer allNodes(relayNodes);
  allNodes.Add(source);
  allNodes.Add(sink);

  PacketSocketHelper socketHelper;
  socketHelper.Install(allNodes);

  // Positions
  Ptr<UniformRandomVariable> rand = CreateObject<UniformRandomVariable>();
  Ptr<ListPositionAllocator> positions = CreateObject<ListPositionAllocator>();
  for (uint32_t i = 0; i < relays; i++)
    {
      positions->Add(Vector(rand->GetValue(0, width),
                            rand->GetValue(0, length),
                            rand->GetValue(0, depth)));
    }
  positions->Add(Vector(width / 2, length / 2, 0));     // source on the seafloor
  positions->Add(Vector(width / 2, length / 2, depth)); // sink at the surface

  MobilityHelper mobility;
  mobility.SetPositionAllocator(positions);
  mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobility.Install(allNodes);

  // Connectivity check: if no relay is within range of the source or the sink,
  // nothing can be delivered no matter what the routing does.
  Ptr<MobilityModel> srcMob = source->GetObject<MobilityModel>();
  Ptr<MobilityModel> sinkMob = sink->GetObject<MobilityModel>();
  uint32_t srcNeighbours = 0;
  uint32_t sinkNeighbours = 0;
  for (uint32_t i = 0; i < relays; i++)
    {
      Ptr<MobilityModel> m = relayNodes.Get(i)->GetObject<MobilityModel>();
      srcNeighbours += (m->GetDistanceFrom(srcMob) <= range);
      sinkNeighbours += (m->GetDistanceFrom(sinkMob) <= range);
    }

  // Aqua-Sim stack: range-limited channel, broadcast MAC, DBR routing
  AquaSimChannelHelper channel = AquaSimChannelHelper::Default();
  channel.SetPropagation("ns3::AquaSimRangePropagation");
  AquaSimHelper asHelper = AquaSimHelper::Default();
  asHelper.SetChannel(channel.Create());
  asHelper.SetMac("ns3::AquaSimBroadcastMac");
  asHelper.SetRouting("ns3::AquaSimDBR");

  NetDeviceContainer devices;
  for (uint32_t i = 0; i < allNodes.GetN(); i++)
    {
      Ptr<AquaSimNetDevice> dev = CreateObject<AquaSimNetDevice>();
      devices.Add(asHelper.Create(allNodes.Get(i), dev));
      dev->GetPhy()->SetTransRange(range);
      dev->GetRouting()->TraceConnectWithoutContext("PacketTransmitting",
                                                    MakeCallback(&RoutingTx));
    }

  Ptr<AquaSimNetDevice> sinkDev = DynamicCast<AquaSimNetDevice>(devices.Get(relays + 1));
  sinkDev->GetRouting()->TraceConnectWithoutContext("PacketReceived", MakeCallback(&SinkRx));

  // Source socket aimed at the sink
  PacketSocketAddress sinkAddr;
  sinkAddr.SetAllDevices();
  sinkAddr.SetPhysicalAddress(sinkDev->GetAddress());
  sinkAddr.SetProtocol(0);

  Ptr<Socket> srcSocket =
    Socket::CreateSocket(source, TypeId::LookupByName("ns3::PacketSocketFactory"));
  srcSocket->Bind();
  srcSocket->Connect(sinkAddr);

  double start = 1.0;
  Simulator::Schedule(Seconds(start), &SendOne, srcSocket, packetSize, Seconds(interval), packets);

  double stop = start + packets * interval + drain;
  Simulator::Stop(Seconds(stop));
  Simulator::Run();

  // Results (read before Simulator::Destroy tears the nodes down)
  uint64_t sent = g_sendTime.size();
  uint64_t delivered = g_delivered.size();
  double pdr = sent ? double(delivered) / sent : 0.0;
  uint64_t timed = delivered - g_unmatched;
  double avgDelay = timed ? g_totalDelay.GetSeconds() / timed : 0.0;
  double txCost = delivered ? double(g_txCount) / delivered : 0.0;

  std::cout << "relays=" << relays << " range=" << range << " box=" << width << "x" << length
            << "x" << depth << " run=" << RngSeedManager::GetRun()
            << " relaysInRangeOfSource=" << srcNeighbours
            << " relaysInRangeOfSink=" << sinkNeighbours << "\n";
  if (srcNeighbours == 0 || sinkNeighbours == 0)
    {
      std::cout << "WARNING: source or sink has no relay in range; this layout "
                   "cannot deliver anything\n";
    }
  std::cout << "sent=" << sent << " delivered=" << delivered << " pdr=" << pdr
            << " avgDelay=" << avgDelay << "s tx=" << g_txCount << " txCost=" << txCost
            << " duplicates=" << g_duplicates << " unmatched=" << g_unmatched << "\n";
  // one machine-readable line for the metrics script
  std::cout << "CSV," << RngSeedManager::GetRun() << "," << relays << "," << range << "," << sent
            << "," << delivered << "," << pdr << "," << avgDelay << "," << g_txCount << ","
            << txCost << "\n";

  Simulator::Destroy();
  return 0;
}
