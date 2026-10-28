/// Regression test for lib/ts_stream.cpp: a TS video track with B-frames crossing the 33-bit
/// PTS/DTS boundary (every 2^33/90000 s = 26.5 h). PTS runs ahead of DTS, so it wraps a few
/// frames before DTS does. The demuxer must unwrap each clock on its own: the frames where the
/// raw PTS has wrapped but the raw DTS has not are valid, must not be reported as "DTS > PTS",
/// and must not advance the rollover counter early. The output times must stay continuous
/// (DTS + 0 extra rollover periods) and the composition offsets must survive.
#include <mist/ts_packet.h>
#include <mist/ts_stream.h>
#include <mist/dtsc.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

static const uint64_t ROLL = TS_PTS_ROLLOVER; // ms
static const uint16_t VIDEO_PID = 256;

static void encTimestamp(std::string &buf, char lead, uint64_t ticks){
  buf += (char)(lead | ((ticks & 0x1C0000000LL) >> 29) | 0x01);
  buf += (char)((ticks & 0x03FC00000LL) >> 22);
  buf += (char)(((ticks & 0x0003F8000LL) >> 14) | 0x01);
  buf += (char)((ticks & 0x000007F80LL) >> 7);
  buf += (char)(((ticks & 0x00000007FLL) << 1) | 0x01);
}

/// One TS packet holding one complete video PES with PTS+DTS, written as a muxer would: the
/// 33-bit fields carry the timestamps modulo 2^33.
static TS::Packet videoPes(uint64_t dtsMs, uint64_t ptsMs, uint8_t &cc){
  uint64_t d = (dtsMs * 90) & 0x1FFFFFFFFull;
  uint64_t p = (ptsMs * 90) & 0x1FFFFFFFFull;
  std::string payload("\000\000\000\001\101\232\003\004\005\006\007\010", 12); // non-IDR slice NAL
  std::string pes("\000\000\001\340", 4);
  uint16_t len = 3 + 10 + payload.size();
  pes += (char)(len >> 8);
  pes += (char)(len & 0xFF);
  pes += (char)0x84; // data_alignment_indicator
  pes += (char)0xC0; // PTS and DTS present
  pes += (char)10;   // PES_header_data_length
  encTimestamp(pes, 0x30, p);
  encTimestamp(pes, 0x10, d);
  pes += payload;
  TS::Packet pack;
  pack.clear();
  pack.setPID(VIDEO_PID);
  pack.setUnitStart(true);
  pack.setContinuityCounter(cc++ & 0x0F);
  pack.fillFree(pes.data(), pes.size());
  pack.addStuffing();
  return pack;
}

static TS::Packet pmt(){
  TS::ProgramMappingTable PMT;
  PMT.setPID(4096);
  PMT.setTableId(2);
  PMT.setSectionLength(0xB00D + 5);
  PMT.setProgramNumber(1);
  PMT.setVersionNumber(0);
  PMT.setCurrentNextIndicator(1);
  PMT.setSectionNumber(0);
  PMT.setLastSectionNumber(0);
  PMT.setContinuityCounter(0);
  PMT.setPCRPID(VIDEO_PID);
  PMT.setProgramInfo("");
  TS::ProgramMappingEntry entry = PMT.getEntry(0);
  entry.setElementaryPid(VIDEO_PID);
  entry.setStreamType(0x1B);
  entry.setESInfo("");
  entry.advance();
  PMT.calcCRC();
  return PMT;
}

int main(){
  // Decode-order frames of an IBBP-like stream at 30 fps straddling the boundary, as
  // (dts, pts) in unwrapped ms. cts cycles 100/0/33/67 like a PBB pattern.
  std::vector<std::pair<uint64_t, uint64_t> > frames;
  const uint64_t cts[4] = {100, 0, 33, 67};
  for (size_t i = 0; i < 24; ++i){
    uint64_t d = ROLL - 300 + i * 33;
    frames.push_back(std::make_pair(d, d + cts[i % 4]));
  }

  TS::Stream ts;
  TS::Packet pat;
  pat.FromPointer(TS::PAT);
  ts.parse(pat, 0);
  TS::Packet pmtPack = pmt();
  ts.parse(pmtPack, 188);

  uint8_t cc = 0;
  uint64_t bpos = 376;
  for (size_t i = 0; i < frames.size(); ++i){
    TS::Packet p = videoPes(frames[i].first, frames[i].second, cc);
    ts.parse(p, bpos);
    bpos += 188;
  }
  ts.finish();

  std::vector<std::pair<uint64_t, int64_t> > got;
  DTSC::Packet pack;
  while (ts.hasPacket(VIDEO_PID)){
    ts.getPacket(VIDEO_PID, pack);
    if (!pack){break;}
    got.push_back(std::make_pair(pack.getTime(), (int64_t)pack.getInt("offset")));
  }

  size_t testCount = 0;
  size_t failures = 0;
  std::cout << "TAP version 14" << std::endl << "1.." << (frames.size() + 1) << std::endl;
#define CHECK(expression, title)                                                                   \
  if (!(expression)){                                                                              \
    std::cout << "not ok " << ++testCount << " - " << title << std::endl;                          \
    ++failures;                                                                                    \
  }else{                                                                                           \
    std::cout << "ok " << ++testCount << " - " << title << std::endl;                              \
  }

  CHECK(got.size() == frames.size(), "all " << frames.size() << " frames came out (got " << got.size() << ")");
  for (size_t i = 0; i < frames.size(); ++i){
    uint64_t wantTime = frames[i].first;
    int64_t wantOffset = (int64_t)(frames[i].second - frames[i].first);
    bool ok = i < got.size() && got[i].first == wantTime && got[i].second == wantOffset;
    CHECK(ok, "frame " << i << " dts=" << (int64_t)(frames[i].first - ROLL) << " pts=" << (int64_t)(frames[i].second - ROLL)
                       << " relative to the wrap: time " << (i < got.size() ? (int64_t)(got[i].first - ROLL) : -1)
                       << " offset " << (i < got.size() ? got[i].second : -1) << " (want " << (int64_t)(wantTime - ROLL)
                       << " / " << wantOffset << ")");
  }
  return failures ? 1 : 0;
}
