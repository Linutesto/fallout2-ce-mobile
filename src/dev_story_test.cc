#include "dev_story_test.h"
#include <cstdlib>
#include <ctime>
#include "dev_autotest.h"
#include "critter.h"
#include "display_monitor.h"
#include "inventory.h"
#include "item.h"
#include "map.h"
#include "npc_story.h"
#include "object.h"
#include "party_member.h"
#include "proto.h"
#include "scripts.h"
#include "stat.h"
#include "tile.h"
#include "worldmap.h"
namespace fallout {
namespace {
    int npcId = -1, caps = 0, xp = 0, originalPrice = 0, giftBefore = 0;
    long long savedHead = 0;
    std::string savedBranch, campaign;
    std::shared_ptr<NpcBrainCall> call;
    void check(bool value, const char* what)
    {
        devAutotestNote("  story %s: %s\n", what, value ? "PASS" : "FAIL");
        if (!value) exit(EXIT_FAILURE);
    }
    Object* item(Object* owner, int pid)
    {
        for (int i=0; i<owner->data.inventory.length; i++) if (owner->data.inventory.items[i].item->pid==pid) return owner->data.inventory.items[i].item;
        return nullptr;
    }
    int quantity(Object* owner, int pid)
    {
        auto obj = item(owner, pid);
        return obj ? itemGetQuantity(owner, obj) : 0;
    }
    void give(Object* owner, int pid, int qty)
    {
        Object* obj = nullptr;
        check(objectCreateWithProtoId(&obj, ProtoId(pid)) == 0 && itemAdd(owner,obj,qty)==0, "fixture item");
        _obj_disconnect(obj, nullptr);
    }
}
bool devStoryTestTick(int stage)
{
    if (stage == 0) {
        npcStoryReset();
        Object* npc = nullptr;
        check(objectCreateWithProtoId(&npc, ProtoId(16777278)) == 0, "fixture NPC");
        Rect rect;
        objectSetLocation(npc, tileGetTileInDirection(gDude->tile, ROTATION_SE, 2), gDude->elevation, &rect);
        check(partyMemberAdd(npc) == 0, "fixture party save");
        npcId = scriptsSetUniqueObjectId(npc);
        give(npc, 40, 2);
        pcSetStat(PC_STAT_LEVEL, 3);
        originalPrice = barterQuoteNpcItem(gDude, npc, item(npc,40), 1);
        giftBefore = quantity(gDude,40);
        campaign = "story-autotest-" + std::to_string(std::time(nullptr));
        npcStoryBeginConversation();
        return true;
    }
    if (stage == 6) {
        check(npcStoryState().quests.size()==2 && npcStoryState().quests.back().state=="abandoned", "journal abandon persisted in live state");
        return true;
    }
    Object* npc = objectFindById(npcId);
    if (!call) {
        check(npc != nullptr, "stable NPC identity");
        NpcBrainRequest request;
        request.campaign=campaign; request.npcPid=npc->pid; request.npcName=critterGetName(npc);
        request.npcInstance=std::to_string(npcId); request.map=gMapHeader.name;
        request.partyMember=true; request.canBarter=true;
        request.playerName=critterGetName(gDude); request.playerLevel=pcGetStat(PC_STAT_LEVEL);
        request.allowedIntents={"none"};
        if (stage==3) {
            give(gDude,19,1); caps=itemGetTotalCaps(gDude); xp=pcGetStat(PC_STAT_EXPERIENCE);
        }
        if (stage==4) { savedHead=npcStoryTimeline().head; savedBranch=npcStoryTimeline().branch; }
        if (stage==5) {
            check(npcStoryTimeline().fork && npcStoryTimeline().head==savedHead && npcStoryTimeline().branch==savedBranch, "save load requests timeline fork");
            check(npcStoryState().quests.size()==1 && npcStoryState().quests[0].state=="done" && npcStoryDiscount(npc)==10, "save restores quests and discount");
        }
        npcStoryFillRequest(npc,&request);
        request.say="story-stage-"+std::to_string(stage);
        call=npcBrainTalk(request);
        return false;
    }
    if (call->status()==NpcBrainStatus::Pending) return false;
    check(call->status()==NpcBrainStatus::Done, "HTTP brain reply");
    auto response=call->response(); call.reset();
    for (const auto& msg : npcStoryApplyResponse(npc,response)) {
        devAutotestNote("  story result: %s\n",msg.c_str());
        displayMonitorAddMessage(msg.c_str());
    }
    if (stage==1) {
        check(npcStoryDiscount(npc)==10, "NPC discount applied");
        int price=barterQuoteNpcItem(gDude,npc,item(npc,40),1);
        devAutotestNote("  story barter before %d after %d\n",originalPrice,price);
        check(price<originalPrice && price>=originalPrice*9/10-1 && price<=originalPrice*9/10+1, "barter quote reduced ten percent");
        check(quantity(gDude,40)==giftBefore+1 && quantity(npc,40)==1, "gift moves actual item");
    } else if (stage==2) {
        check(npcStoryState().quests.size()==1, "fetch quest given");
        check(npcStoryState().revealed.size()==1, "reveal recorded");
        bool known=false;
        for (int i=0; i<wmAreaMaxCount();i++) {
            char name[41]={}; wmGetAreaIdxName(static_cast<City>(i),name);
            if (npcStoryState().revealed[0]==name) known=wmAreaIsKnown(static_cast<City>(i));
        }
        check(known,"reveal marks world area known");
    } else if (stage==3) {
        check(npcStoryState().quests[0].state=="done", "quest requirement completed");
        check(itemGetTotalCaps(gDude)==caps+100 && pcGetStat(PC_STAT_EXPERIENCE)>=xp+50, "caps and XP paid");
        check(quantity(gDude,19)==0 && quantity(npc,19)>=1, "fetch removed from player");
    } else if (stage==4) {
        check(npcStoryTimeline().head>savedHead,"post save memory advances");
    } else if (stage==5) {
        check(npcStoryTimeline().branch!=savedBranch && !npcStoryTimeline().fork,"response adopts child branch");
        check(npcStoryState().quests.size()==2,"new branch quest for journal");
    }
    return true;
}
}
