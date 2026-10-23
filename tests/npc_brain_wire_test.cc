// npc_brain_wire: STORY-MEMORY-SPEC sections 1, 2, 5 and save state.
#include "npc_brain.h"
#include "npc_brain_json.h"
#include <cassert>
#include <iostream>
using namespace fallout;
using namespace fallout::brainJson;
int main()
{
    NpcBrainRequest r;
    r.campaign = "Chosen One|123"; r.npcPid=16777278; r.npcName="Vic";
    r.story.quests.push_back({"Arroyo", "Find GECK", "active"});
    r.story.towns.push_back({"Klamath", "Accepted", 12});
    r.story.karmaTitle="Defender"; r.story.killsTotal=31;
    r.story.killsMen=4; r.story.killsChildren=2; r.story.party={"Sulik"};
    r.story.perksTitles={"Childkiller"}; r.story.knownLocations={"Arroyo"};
    r.story.gameTicks=302400000; r.story.npcTeam=4;
    r.story.discountPercent=10; WastelandQuest q;
    q.id="q-3"; q.giver="42"; q.giverName="Vic"; q.title="Radio";
    q.objective="Bring radio"; q.kind="fetch"; q.pid=19; q.quantity=2;
    q.rewardCaps=100; q.rewardXp=50; r.story.wastelandQuests={q};
    r.timeline={"b-root", 117, true, true};
    r.allowedEffects.maxDiscount=20; r.allowedEffects.gifts={{40,"Stimpak",2}};
    r.allowedEffects.sideQuest=true; r.allowedEffects.fetchItems={{19,"Rock",1}};
    r.allowedEffects.revealLocations={"Klamath"}; r.allowedEffects.visitLocations={"The Den"};
    r.allowedEffects.completeQuests={"q-3"}; r.allowedEffects.turnHostile=true;
    JsonValue j; auto json=npcBrainRequestToJson(r); assert(JsonParser(json).parse(&j));
    auto s=j.get("story"); assert(s && s->get("quests")->array[0].get("state")->string=="active");
    assert(s->get("towns")->array[0].get("reputation")->number==12);
    assert(s->get("karma_title")->string=="Defender");
    assert(s->get("kills")->get("children")->number==2);
    assert(s->get("game_ticks")->number==302400000 && s->get("npc_team")->number==4);
    assert(s->get("wasteland")->get("discount_percent")->number==10);
    assert(s->get("wasteland")->get("quests")->array[0].get("giver")->string=="Vic");
    assert(j.get("timeline")->get("fork")->boolean);
    assert(j.get("allowed_effects")->get("gift")->get("items")->array[0].get("max_qty")->number==2);
    assert(j.get("allowed_effects")->get("side_quest")->get("max_caps")->number==200);
    r.story.quests.resize(80,r.story.quests[0]); r.story.towns.resize(50,r.story.towns[0]);
    r.story.karmaTitle=std::string(500,'x'); assert(JsonParser(npcBrainRequestToJson(r)).parse(&j));
    assert(j.get("story")->get("quests")->array.size()==40);
    assert(j.get("story")->get("towns")->array.size()==20);
    assert(j.get("story")->get("karma_title")->string.size()<=300);
    NpcBrainResponse out;
    assert(npcBrainParseResponse(R"({"say":"Here.","timeline":{"branch":"b-a","head":118},"effects":[{"type":"discount","percent":10},{"type":"gift","pid":40,"qty":1}]})", {}, &out));
    assert(out.timeline.present && out.timeline.branch=="b-a" && out.timeline.head==118 && out.effects.size()==2);
    for (auto extra : {R"("timeline":[])",R"("timeline":{"branch":42,"head":1})",R"("timeline":{"branch":"b","head":-1})",R"("timeline":{"branch":"b","head":1e99})",R"("timeline":{"branch":"b","head":1.5})"}) {
        assert(npcBrainParseResponse(std::string("{\"say\":\"ok\",")+extra+"}",{},&out)); assert(!out.timeline.present);
    }
    assert(npcBrainParseResponse(R"({"say":"ok","effects":[null,[],{"type":"nuke"},{"type":"gift","pid":"40","qty":1},{"type":"discount","percent":1e100}]})",{},&out)); assert(out.effects.empty());
    assert(npcBrainParseResponse(R"({"say":"ok","effects":[{"type":"side_quest","kind":"visit","title":"Scout","objective":"Go","location":"Den","reward_caps":20,"reward_xp":5},{"type":"complete_quest","id":"q-3"},{"type":"turn_hostile"}]})",{},&out)); assert(out.effects.size()==2);
    assert(!npcBrainParseResponse("{",{},&out));
    assert(!npcBrainParseResponse(std::string(300000,'x'),{},&out));
    WastelandState state; state.discounts={{"42",10}};state.quests={q};state.questCounter=3;state.revealed={"Klamath"};
    WastelandState loaded; assert(wastelandStateFromJson(wastelandStateToJson(state),&loaded));
    assert(loaded.questCounter==3 && loaded.discounts[0].npc=="42" && loaded.quests[0].quantity==2 && loaded.quests[0].giverName=="Vic");
    assert(loaded.revealed==state.revealed);
    assert(!wastelandStateFromJson(R"({"quests":"bad"})",&loaded));
    assert(loaded.questCounter==3); // invalid state must not replace the valid state
    assert(!wastelandStateFromJson(R"({"quest_counter":-1,"discounts":[],"quests":[],"revealed":[]})",&loaded));
    NpcBrainTimeline tl; assert(npcBrainTimelineFromJson(npcBrainTimelineToJson(r.timeline),&tl));
    assert(tl.head==117 && tl.branch=="b-root");
    assert(!npcBrainTimelineFromJson(R"({"branch":"b","head":true})",&tl));
    std::cout << "npc_brain_wire: story, bounds, malformed responses, timeline and state round trips PASS\n";
}
