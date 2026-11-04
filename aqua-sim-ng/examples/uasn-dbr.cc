/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * CSC 466 project: DBR under blackhole / grayhole / depth-spoofing / sinkhole
 * attacks, with bad nodes placed at random or in one depth band.
 *
 * Layout (z = height above the seafloor, so bigger z = shallower):
 *   - one source on the seafloor  at (width/2, length/2, 0)
 *   - one sink   at the surface   at (width/2, length/2, depth)
 *   - `relays` relay nodes placed uniformly at random in the box
 *   - the box is split into `bands` equal depth bands; with 3 bands:
 *       deep    z in [0,       depth/3)
 *       middle  z in [depth/3, 2depth/3)
 *       shallow z in [2depth/3, depth]
 *
 * Bad nodes are chosen only from relays: round(fraction * relays) of them
 * (or --badNodes=N), either from all relays (placement=random) or only from
 * one band. If a band has fewer relays than needed, all of them are used and
 * the CSV records the real number in `bad`.
 *
 * Fair comparisons: node positions and the order bad nodes are picked in
 * come from their own random streams, so for one --RngRun the layout is the
 * same in every run, and the 10% bad set contains the 5% bad set.
 *
 * Metrics (printed, plus one CSV line):
 *   delivery ratio = unique packets at the sink / packets sent
 *   delay          = mean time for the first copy to reach the sink
 *   tx cost        = routing-layer transmissions (all nodes) / delivered
 *
 * Examples:
 *   ./ns3 run "UasnDbr --RngRun=1"                                   (no attack)
 *   ./ns3 run "UasnDbr --attack=blackhole --placement=shallow --fraction=0.1 --RngRun=1"
 *   ./ns3 run "UasnDbr --attack=grayhole --dropProb=0.5 --placement=deep --fraction=0.2"
 *   ./ns3 run "UasnDbr --attack=depthspoof --placement=middle --badNodes=1"
 *   ./ns3 run "UasnDbr --csvHeader"              (print the CSV column names and exit)
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/energy-module.h"
#include "ns3/aqua-sim-ng-module.h"
#include "ns3/aqua-sim-routing-dbr.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <vector>

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

const char* CSV_HEADER =
  "run,relays,range,width,length,depth,packets,attack,placement,fractionAsked,fraction,bad,"
  "dropProb,"
  "sent,delivered,pdr,avgDelay,tx,txCost,duplicates,attackerDrops,spoofTx,"
  "srcNeighbours,sinkNeighbours,reachable,badInRangeOfSink";

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

// Band index of a relay at height z: 0 = deepest band ... bands-1 = shallowest.
uint32_t
BandOf(double z, double depth, uint32_t bands)
{
  uint32_t b = static_cast<uint32_t>(z / depth * bands);
  return std::min(b, bands - 1);
}

// Could a packet get from the source to the sink with every node honest?
// A DBR hop needs the receiver in range and strictly shallower than the sender.
bool
DbrReachable(const std::vector<Vector>& pos, uint32_t src, uint32_t dst, double range)
{
  std::vector<bool> seen(pos.size(), false);
  std::queue<uint32_t> q;
  q.push(src);
  seen[src] = true;
  while (!q.empty())
    {
      uint32_t u = q.front();
      q.pop();
      if (u == dst)
        {
          return true;
        }
      for (uint32_t v = 0; v < pos.size(); v++)
        {
          if (!seen[v] && pos[v].z > pos[u].z && CalculateDistance(pos[u], pos[v]) <= range)
            {
              seen[v] = true;
              q.push(v);
            }
        }
    }
  return false;
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

  std::string attack = "none";       // none|blackhole|grayhole|depthspoof|sinkhole
  std::string placement = "random";  // random|shallow|middle|deep  (or band index 0..bands-1)
  double fraction = 0.0;             // fraction of relays that are bad
  int badNodes = -1;                 // exact number of bad nodes (overrides fraction)
  uint32_t bands = 3;
  double dropProb = 0.5;             // grayhole drop probability
  double spoofZ = -1;                // claimed height for spoofing; default depth + range
  bool csvHeader = false;
  bool verbose = true;

  CommandLine cmd;
  cmd.AddValue("relays", "Number of relay nodes (max 252)", relays);
  cmd.AddValue("width", "Box width in x (m)", width);
  cmd.AddValue("length", "Box length in y (m)", length);
  cmd.AddValue("depth", "Box depth in z (m)", depth);
  cmd.AddValue("range", "Transmission range (m)", range);
  cmd.AddValue("packets", "Packets sent by the source", packets);
  cmd.AddValue("interval", "Seconds between packets", interval);
  cmd.AddValue("packetSize", "Packet size (bytes)", packetSize);
  cmd.AddValue("drain", "Seconds to keep running after the last send", drain);
  cmd.AddValue("attack", "none|blackhole|grayhole|depthspoof|sinkhole", attack);
  cmd.AddValue("placement", "random|shallow|middle|deep, or a band index", placement);
  cmd.AddValue("fraction", "Fraction of relays that are bad (0-1)", fraction);
  cmd.AddValue("badNodes", "Exact number of bad nodes (overrides --fraction)", badNodes);
  cmd.AddValue("bands", "Number of depth bands", bands);
  cmd.AddValue("dropProb", "Grayhole drop probability (0-1)", dropProb);
  cmd.AddValue("spoofZ", "Height a spoofer/sinkhole claims (default depth+range)", spoofZ);
  cmd.AddValue("csvHeader", "Print the CSV column names and exit", csvHeader);
  cmd.AddValue("verbose", "Print the human-readable summary", verbose);
  cmd.Parse(argc, argv);

  if (csvHeader)
    {
      std::cout << "CSVHDR," << CSV_HEADER << "\n";
      return 0;
    }

  // AquaSim addresses are 1-byte style and 255 is broadcast: keep below it.
  NS_ABORT_MSG_IF(relays + 2 >= 255, "At most 252 relays are supported");
  NS_ABORT_MSG_IF(bands < 1, "--bands must be at least 1");
  if (spoofZ < 0)
    {
      spoofZ = depth + range;
    }

  std::map<std::string, AquaSimDBR::AttackMode> modes = {
    {"none", AquaSimDBR::ATTACK_NONE},
    {"blackhole", AquaSimDBR::ATTACK_BLACKHOLE},
    {"grayhole", AquaSimDBR::ATTACK_GRAYHOLE},
    {"depthspoof", AquaSimDBR::ATTACK_DEPTHSPOOF},
    {"sinkhole", AquaSimDBR::ATTACK_SINKHOLE},
  };
  NS_ABORT_MSG_IF(modes.find(attack) == modes.end(), "Unknown --attack=" << attack);
  AquaSimDBR::AttackMode mode = modes[attack];

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

  // Positions (own stream, so they never depend on the attack settings)
  Ptr<UniformRandomVariable> rand = CreateObject<UniformRandomVariable>();
  rand->SetStream(1);
  std::vector<Vector> pos;
  for (uint32_t i = 0; i < relays; i++)
    {
      double x = rand->GetValue(0, width);
      double y = rand->GetValue(0, length);
      double z = rand->GetValue(0, depth);
      pos.push_back(Vector(x, y, z));
    }
  pos.push_back(Vector(width / 2, length / 2, 0));     // source on the seafloor
  pos.push_back(Vector(width / 2, length / 2, depth)); // sink at the surface
  uint32_t srcIdx = relays;
  uint32_t sinkIdx = relays + 1;

  Ptr<ListPositionAllocator> positions = CreateObject<ListPositionAllocator>();
  for (const Vector& v : pos)
    {
      positions->Add(v);
    }
  MobilityHelper mobility;
  mobility.SetPositionAllocator(positions);
  mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobility.Install(allNodes);

  // Connectivity (computed with every node honest)
  uint32_t srcNeighbours = 0;
  uint32_t sinkNeighbours = 0;
  for (uint32_t i = 0; i < relays; i++)
    {
      srcNeighbours += (CalculateDistance(pos[i], pos[srcIdx]) <= range);
      sinkNeighbours += (CalculateDistance(pos[i], pos[sinkIdx]) <= range);
    }
  bool reachable = DbrReachable(pos, srcIdx, sinkIdx, range);

  // ---- choose the bad nodes ----
  std::vector<uint32_t> candidates;
  int band = -1;
  if (placement != "random")
    {
      if (placement == "deep")
        band = 0;
      else if (placement == "shallow")
        band = bands - 1;
      else if (placement == "middle")
        band = bands / 2;
      else
        {
          std::istringstream is(placement);
          NS_ABORT_MSG_IF(!(is >> band) || band < 0 || band >= int(bands),
                          "Unknown --placement=" << placement);
        }
    }
  for (uint32_t i = 0; i < relays; i++)
    {
      if (band < 0 || int(BandOf(pos[i].z, depth, bands)) == band)
        {
          candidates.push_back(i);
        }
    }
  // Shuffle with its own stream: same order for every fraction, so sets nest.
  Ptr<UniformRandomVariable> pick = CreateObject<UniformRandomVariable>();
  pick->SetStream(2 + (band < 0 ? 0 : band + 1));
  for (uint32_t i = candidates.size(); i > 1; i--)
    {
      uint32_t j = pick->GetInteger(0, i - 1);
      std::swap(candidates[i - 1], candidates[j]);
    }
  uint32_t wanted = badNodes >= 0 ? uint32_t(badNodes)
                                  : uint32_t(std::lround(fraction * relays));
  if (mode == AquaSimDBR::ATTACK_NONE)
    {
      wanted = 0;
    }
  uint32_t nBad = std::min<uint32_t>(wanted, candidates.size());
  if (nBad < wanted)
    {
      std::cerr << "WARNING: placement=" << placement << " has only " << candidates.size()
                << " relays; using " << nBad << " bad nodes instead of " << wanted << "\n";
    }
  std::set<uint32_t> bad(candidates.begin(), candidates.begin() + nBad);
  uint32_t badNearSink = 0;
  for (uint32_t i : bad)
    {
      badNearSink += (CalculateDistance(pos[i], pos[sinkIdx]) <= range);
    }

  // Aqua-Sim stack: range-limited channel, broadcast MAC, DBR routing
  AquaSimChannelHelper channel = AquaSimChannelHelper::Default();
  channel.SetPropagation("ns3::AquaSimRangePropagation");
  AquaSimHelper asHelper = AquaSimHelper::Default();
  asHelper.SetChannel(channel.Create());
  asHelper.SetMac("ns3::AquaSimBroadcastMac");
  asHelper.SetRouting("ns3::AquaSimDBR");

  NetDeviceContainer devices;
  std::vector<Ptr<AquaSimDBR>> badRouting;
  for (uint32_t i = 0; i < allNodes.GetN(); i++)
    {
      Ptr<AquaSimNetDevice> dev = CreateObject<AquaSimNetDevice>();
      devices.Add(asHelper.Create(allNodes.Get(i), dev));
      dev->GetPhy()->SetTransRange(range);
      dev->GetRouting()->TraceConnectWithoutContext("PacketTransmitting",
                                                    MakeCallback(&RoutingTx));
    }
  for (uint32_t i : bad)
    {
      Ptr<AquaSimNetDevice> dev = DynamicCast<AquaSimNetDevice>(devices.Get(i));
      Ptr<AquaSimDBR> dbr = DynamicCast<AquaSimDBR>(dev->GetRouting());
      NS_ABORT_MSG_IF(!dbr, "Routing is not AquaSimDBR");
      dbr->SetAttack(mode, dropProb, spoofZ);
      badRouting.push_back(dbr);
    }

  Ptr<AquaSimNetDevice> sinkDev = DynamicCast<AquaSimNetDevice>(devices.Get(sinkIdx));
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
  uint64_t attackerDrops = 0;
  uint64_t spoofTx = 0;
  for (auto& r : badRouting)
    {
      attackerDrops += r->GetAttackDrops();
      spoofTx += r->GetSpoofTx();
    }
  // a run with no bad nodes is the baseline, whatever --attack said
  std::string attackOut = (nBad == 0) ? "none" : attack;
  std::string placementOut = (nBad == 0) ? "none" : placement;
  double fractionOut = double(nBad) / relays;
  // what was asked for (rounded to whole nodes), even if a band ran short
  double fractionAsked = double(wanted) / relays;

  if (verbose)
    {
      std::cout << "relays=" << relays << " range=" << range << " box=" << width << "x"
                << length << "x" << depth << " run=" << RngSeedManager::GetRun()
                << " relaysInRangeOfSource=" << srcNeighbours
                << " relaysInRangeOfSink=" << sinkNeighbours
                << " reachable=" << (reachable ? "yes" : "no") << "\n";
      std::cout << "attack=" << attackOut << " placement=" << placementOut << " bad=" << nBad
                << " (" << 100.0 * fractionOut << "% of relays, " << badNearSink
                << " next to the sink)";
      if (mode == AquaSimDBR::ATTACK_GRAYHOLE)
        std::cout << " dropProb=" << dropProb;
      if (mode == AquaSimDBR::ATTACK_DEPTHSPOOF || mode == AquaSimDBR::ATTACK_SINKHOLE)
        std::cout << " spoofZ=" << spoofZ;
      std::cout << "\n";
      if (!reachable)
        {
          std::cout << "WARNING: even with no attackers, DBR cannot reach the sink in this "
                       "layout\n";
        }
      std::cout << "sent=" << sent << " delivered=" << delivered << " pdr=" << pdr
                << " avgDelay=" << avgDelay << "s tx=" << g_txCount << " txCost=" << txCost
                << " duplicates=" << g_duplicates << " attackerDrops=" << attackerDrops
                << " spoofTx=" << spoofTx << "\n";
    }

  // one machine-readable line for the run script (columns: see CSV_HEADER)
  std::cout << "CSV," << RngSeedManager::GetRun() << "," << relays << "," << range << ","
            << width << "," << length << "," << depth << "," << packets << "," << attackOut
            << "," << placementOut << "," << (nBad == 0 ? 0.0 : fractionAsked) << ","
            << fractionOut << "," << nBad << ","
            << (mode == AquaSimDBR::ATTACK_GRAYHOLE ? dropProb : 1.0) << "," << sent << ","
            << delivered << "," << pdr << "," << avgDelay << "," << g_txCount << "," << txCost
            << "," << g_duplicates << "," << attackerDrops << "," << spoofTx << ","
            << srcNeighbours << "," << sinkNeighbours << "," << (reachable ? 1 : 0) << ","
            << badNearSink << "\n";

  Simulator::Destroy();
  return 0;
}
