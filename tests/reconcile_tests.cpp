#include "reconcile.h"
#include <iostream>
#include <tuple>
#include <source_location>
using namespace media;
void check(bool condition, std::source_location at = std::source_location::current()) {
  if (!condition) throw std::runtime_error("reconciliation assertion failed at line " + std::to_string(at.line()));
}
pb::SpeechHypothesis window(uint64_t number, uint64_t first, uint64_t fresh,
    std::initializer_list<std::tuple<std::string,int64_t,int64_t>> words) {
  pb::SpeechHypothesis h; h.set_window_number(number); h.set_first_sample(first);
  h.set_first_new_sample(fresh); h.set_sample_count(64000);
  auto* s=h.add_segments(); s->set_segment_id(uuid()); s->set_source_language("en");
  for (const auto& [text,start,end] : words) {
    *s->mutable_text() += text; auto* t=s->add_tokens(); t->set_text(text);
    t->set_kind(pb::SPEECH_TOKEN_KIND_TEXT); t->set_start_us(start); t->set_end_us(end);
  }
  return h;
}
int main() {
 try {
  std::vector<pb::TranscriptSegment> out;
  Reconciler r([&](auto s){out.push_back(std::move(s));});
  r.accept(window(1,0,0,{{" hello",100000,1000000},{" world",3200000,3700000}}),16000);
  check(out.size()==2 && out[0].final() && !out[1].final());
  const auto pending=out[1].segment_id();
  r.accept(window(2,48000,64000,{{" World",3300000,3750000},{" again",4000000,4500000}}),0);
  check(out.size()==3 && out[2].final() && out[2].segment_id()==pending && out[2].revision()==2);
  check(out[2].text()==" World again" && out[2].reconciliation()==pb::RECONCILIATION_STATUS_ALIGNED);
  r.finish(); check(out.size()==3);
  bool failed=false; try {r.finish();} catch(const std::logic_error&) {failed=true;} check(failed);
  out.clear(); Reconciler repeats([&](auto s){out.push_back(std::move(s));});
  repeats.accept(window(1,0,0,{{" go",100000,500000},{" go",3300000,3600000}}),16000);
  repeats.accept(window(2,48000,64000,{{" go",3300000,3600000},{" go",5000000,5500000}}),0);
  repeats.finish(); std::string text; for(const auto& s:out) if(s.final()) text+=s.text();
  check(text==" go go go");
  out.clear(); Reconciler uncertain([&](auto s){out.push_back(std::move(s));});
  uncertain.accept(window(1,0,0,{{" left",3300000,3600000}}),16000);
  uncertain.accept(window(2,48000,64000,{{" right",3300000,3600000}}),0);
  uncertain.finish(); text.clear(); for(const auto& s:out) if(s.final()) {text+=s.text(); check(s.reconciliation()==pb::RECONCILIATION_STATUS_UNRESOLVED);}
  check(text==" left right");
  out.clear(); Reconciler partial([&](auto s){out.push_back(std::move(s));});
  partial.accept(window(1,0,0,{{" an",3100000,3400000},{" inter",3700000,3990000}}),16000);
  partial.accept(window(2,48000,64000,{{" an",3100000,3400000},{" international",3700000,4500000},{" meeting",4600000,5000000}}),0);
  partial.finish(); text.clear(); for(const auto& s:out) if(s.final()) text+=s.text();
  check(text==" an international meeting");
  out.clear(); Reconciler identity([&](auto s){out.push_back(std::move(s));});
  identity.accept(window(1,0,0,{{" track",3300000,3600000}}),16000);
  auto wrong=window(2,48000,64000,{{" track",3300000,3600000}});
  wrong.set_track_id(1); wrong.mutable_segments(0)->set_track_id(1);
  failed=false; try {identity.accept(wrong,0);} catch(const Failure&) {failed=true;} check(failed);
  check(out.size()==1); // No output attributed to the wrong track.
  Reconciler broken([](auto){throw std::runtime_error("subscriber gone");});
  failed=false; try {broken.accept(window(1,0,0,{{" stop",100000,200000}}),0);} catch(const std::runtime_error&) {failed=true;} check(failed);
  failed=false; try {broken.finish();} catch(const std::logic_error&) {failed=true;} check(failed);
  out.clear(); Reconciler unicode([&](auto s){out.push_back(std::move(s));});
  unicode.accept(window(1,0,0,{{" \xc3",2000000,2500000},{"\xa9",3100000,3400000}}),16000);
  check(out.size()==1 && !out[0].final());
  unicode.finish(); check(out.size()==2 && out[1].final() && out[1].text()==" \xc3\xa9");
  out.clear(); Reconciler ambiguous([&](auto s){out.push_back(std::move(s));});
  ambiguous.accept(window(1,0,0,{{" go",3300000,3600000}}),16000);
  ambiguous.accept(window(2,48000,64000,{{" go",3300000,3600000},{" go",3300000,3600000}}),0);
  ambiguous.finish(); text.clear(); for(const auto& s:out) if(s.final()) {text+=s.text(); check(s.reconciliation()==pb::RECONCILIATION_STATUS_UNRESOLVED);}
  check(text==" go go go");
  out.clear(); Reconciler timing([&](auto s){out.push_back(std::move(s));});
  timing.accept(window(1,0,0,{{" old",3300000,4100000}}),16000); // Out-of-window native timing.
  timing.accept(window(2,48000,64000,{{" old",3300000,3900000},{" tail",6300000,6700000}}),16000);
  timing.finish();
  check(out.back().final() && out.back().text()==" tail" && out.back().reconciliation()==pb::RECONCILIATION_STATUS_UNRESOLVED);
  std::cout<<"reconciliation cases passed\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
