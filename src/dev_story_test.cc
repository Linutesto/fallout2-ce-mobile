#include "dev_story_test.h"
#include "combat.h"
#include "config.h"
#include "critter.h"
#include "dev_autotest.h"
#include "display_monitor.h"
#include "game_config.h"
#include "game_dialog.h"
#include "inventory.h"
#include "item.h"
#include "map.h"
#include "npc_story.h"
#include "object.h"
#include "party_member.h"
#include "proto.h"
#include "scripts.h"
#include "sfall_global_vars.h"
#include "stat.h"
#include "tile.h"
#include "worldmap.h"
#include <cstdlib>
#include <ctime>
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
        for (int i = 0; i < owner->data.inventory.length; i++)
            if (owner->data.inventory.items[i].item->pid == pid) return owner->data.inventory.items[i].item;
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
        check(objectCreateWithProtoId(&obj, ProtoId(pid)) == 0 && itemAdd(owner, obj, qty) == 0, "fixture item");
        _obj_disconnect(obj, nullptr);
    }
} // namespace
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
        pcSetStat(PC_STAT_LEVEL, 5);
        originalPrice = barterQuoteNpcItem(gDude, npc, item(npc, 40), 1);
        giftBefore = quantity(gDude, 40);
        campaign = "story-autotest-" + std::to_string(std::time(nullptr));
        npcStoryBeginConversation();
        return true;
    }
    if (stage == 6) {
        check(npcStoryState().quests.size() == 2 && npcStoryState().quests.back().state == "abandoned", "journal abandon persisted in live state");
        return true;
    }
    Object* npc = objectFindById(npcId);
    if (!call) {
        check(npc != nullptr, "stable NPC identity");
        NpcBrainRequest request;
        request.campaign = campaign;
        request.npcPid = npc->pid;
        request.npcName = critterGetName(npc);
        request.npcInstance = std::to_string(npcId);
        request.map = gMapHeader.name;
        request.partyMember = true;
        request.canBarter = true;
        request.playerName = critterGetName(gDude);
        request.playerLevel = pcGetStat(PC_STAT_LEVEL);
        request.allowedIntents = { "none" };
        if (stage == 3) {
            give(gDude, 19, 1);
            caps = itemGetTotalCaps(gDude);
            xp = pcGetStat(PC_STAT_EXPERIENCE);
        }
        if (stage == 4) {
            savedHead = npcStoryTimeline().head;
            savedBranch = npcStoryTimeline().branch;
        }
        if (stage == 5) {
            check(npcStoryTimeline().fork && npcStoryTimeline().head == savedHead && npcStoryTimeline().branch == savedBranch, "save load requests timeline fork");
            check(npcStoryState().quests.size() == 1 && npcStoryState().quests[0].state == "done" && npcStoryDiscount(npc) == 10, "save restores quests and discount");
        }
        npcStoryFillRequest(npc, &request);
        request.say = "story-stage-" + std::to_string(stage);
        call = npcBrainTalk(request);
        return false;
    }
    if (call->status() == NpcBrainStatus::Pending) return false;
    check(call->status() == NpcBrainStatus::Done, "HTTP brain reply");
    auto response = call->response();
    call.reset();
    for (const auto& msg : npcStoryApplyResponse(npc, response)) {
        devAutotestNote("  story result: %s\n", msg.c_str());
        displayMonitorAddMessage(msg.c_str());
    }
    if (stage == 1) {
        check(npcStoryDiscount(npc) == 10, "NPC discount applied");
        int price = barterQuoteNpcItem(gDude, npc, item(npc, 40), 1);
        devAutotestNote("  story barter before %d after %d\n", originalPrice, price);
        check(price < originalPrice && price >= originalPrice * 9 / 10 - 1 && price <= originalPrice * 9 / 10 + 1, "barter quote reduced ten percent");
        check(quantity(gDude, 40) == giftBefore + 1 && quantity(npc, 40) == 1, "gift moves actual item");
    } else if (stage == 2) {
        check(npcStoryState().quests.size() == 1, "fetch quest given");
        check(npcStoryState().revealed.size() == 1, "reveal recorded");
        bool known = false;
        for (int i = 0; i < wmAreaMaxCount(); i++) {
            char name[41] = {};
            wmGetAreaIdxName(static_cast<City>(i), name);
            if (npcStoryState().revealed[0] == name) known = wmAreaIsKnown(static_cast<City>(i));
        }
        check(known, "reveal marks world area known");
    } else if (stage == 3) {
        check(npcStoryState().quests[0].state == "done", "quest requirement completed");
        check(itemGetTotalCaps(gDude) == caps + 100 && pcGetStat(PC_STAT_EXPERIENCE) >= xp + 50, "caps and XP paid");
        check(quantity(gDude, 19) == 0 && quantity(npc, 19) >= 1, "fetch removed from player");
    } else if (stage == 4) {
        check(npcStoryTimeline().head > savedHead, "post save memory advances");
    } else if (stage == 5) {
        check(npcStoryTimeline().branch != savedBranch && !npcStoryTimeline().fork, "response adopts child branch");
        check(npcStoryState().quests.size() == 2, "new branch quest for journal");
    }
    return true;
}

bool devStoryRealTestTick(int stage)
{
    if (stage == 0) {
        dudeSetName("Story QA");
        return devStoryTestTick(0);
    }
    Object* npc = objectFindById(npcId);
    if (!call) {
        check(npc != nullptr, "real NPC identity");
        int campaignId = 0;
        sfall_gl_vars_fetch("WRCAMPID", campaignId);
        campaign = std::string(critterGetName(gDude)) + "|" + std::to_string(campaignId);
        if (stage == 1 && (!npcStoryState().quests.empty() || npcStoryDiscount(npc) > 0)) {
            devAutotestNote("  real campaign %s branch %s head %lld\n", campaign.c_str(), npcStoryTimeline().branch.c_str(), npcStoryTimeline().head);
            check(true, "real Qwen earned a quest or discount in dialogue");
            return true;
        }
        NpcBrainRequest request;
        request.campaign = campaign;
        request.npcPid = npc->pid;
        request.npcName = critterGetName(npc);
        request.npcInstance = std::to_string(npcId);
        request.map = gMapHeader.name;
        request.partyMember = true;
        request.canBarter = true;
        request.playerName = critterGetName(gDude);
        request.playerLevel = pcGetStat(PC_STAT_LEVEL);
        request.allowedIntents = { "none" };
        npcStoryFillRequest(npc, &request);
        if (stage == 1) request.say = "Can you give me a simple job to fetch you one rock for 10 caps and 5 XP? I will bring it back to you.";
        if (stage == 2) {
            savedHead = npcStoryTimeline().head;
            savedBranch = npcStoryTimeline().branch;
            request.say = "Remember this confidential secret phrase: cobalt-potato-4913. Keep it between us.";
        }
        if (stage == 3) {
            check(npcStoryTimeline().fork && npcStoryTimeline().head == savedHead, "real saved timeline restored for fork");
            request.say = "Have I ever told you a secret phrase? If you have no memory of one, say you do not remember any phrase.";
        }
        call = npcBrainTalk(request);
        return false;
    }
    if (call->status() == NpcBrainStatus::Pending) return false;
    check(call->status() == NpcBrainStatus::Done, "real HTTP talk");
    auto response = call->response();
    call.reset();
    check(response.source == "llm", "real Qwen source");
    devAutotestNote("  real Qwen stage %d reply %s\n", stage, response.say.c_str());
    for (const auto& msg : npcStoryApplyResponse(npc, response)) {
        devAutotestNote("  real effect %s\n", msg.c_str());
        displayMonitorAddMessage(msg.c_str());
    }
    if (stage == 1) {
        static int retries = 0;
        if (npcStoryState().quests.empty() && npcStoryDiscount(npc) == 0 && ++retries < 3) return false;
        check(!npcStoryState().quests.empty() || npcStoryDiscount(npc) > 0, "real Qwen earned consequence");
    } else if (stage == 2)
        check(npcStoryTimeline().head > savedHead, "real post-save memory written");
    else if (stage == 3) {
        check(npcStoryTimeline().branch != savedBranch, "real reload child branch");
        check(response.say.find("cobalt") == std::string::npos && response.say.find("4913") == std::string::npos, "real NPC cannot recall discarded secret");
        devAutotestNote("  real final campaign %s branch %s head %lld\n", campaign.c_str(), npcStoryTimeline().branch.c_str(), npcStoryTimeline().head);
    }
    return true;
}

bool devStoryGuardTestTick(int stage)
{
    if (stage == 0) return devStoryTestTick(0);
    Object* npc = objectFindById(npcId);
    check(npc != nullptr, "guard NPC identity");
    if (stage == 2) {
        check(isInCombat(), "two consecutive hostile replies start native combat");
        return true;
    }
    auto apply = [&](std::vector<NpcBrainEffect> effects, const char* mood = "friendly", Object* giver = nullptr) {
        NpcBrainResponse response;
        response.effects = effects;
        response.mood = mood;
        return npcStoryApplyResponse(giver ? giver : npc, response);
    };
    NpcBrainEffect invalid;
    invalid.type = "discount";
    invalid.percent = 99;
    int initialCaps = itemGetTotalCaps(gDude);
    apply({ invalid });
    check(npcStoryDiscount(npc) == 0, "oversize discount rejected");
    NpcBrainEffect discount;
    discount.type = "discount";
    discount.percent = 10;
    apply({ discount });
    int discountedPrice = barterQuoteNpcItem(gDude, npc, item(npc, 40), 1);
    configSetBool(&gGameConfig, "ai", "enabled", false);
    check(barterQuoteNpcItem(gDude, npc, item(npc, 40), 1) == originalPrice, "AI off restores original barter price");
    configSetBool(&gGameConfig, "ai", "enabled", true);
    check(barterQuoteNpcItem(gDude, npc, item(npc, 40), 1) == discountedPrice, "AI on restores saved discount");
    invalid.type = "reveal_location";
    invalid.name = "Navarro";
    apply({ invalid });
    check(npcStoryState().revealed.empty(), "forbidden location rejected");
    invalid.type = "gift";
    invalid.pid = 232;
    invalid.quantity = 1;
    apply({ invalid });
    check(quantity(gDude, 232) == 0, "unoffered quest item rejected");
    invalid.pid = 40;
    invalid.quantity = 999;
    apply({ invalid });
    check(quantity(npc, 40) == 2, "oversize gift rejected");
    NpcBrainEffect gift;
    gift.type = "gift";
    gift.pid = 40;
    gift.quantity = 1;
    int before = quantity(gDude, 40);
    apply({ gift, gift });
    check(quantity(gDude, 40) == before + 1 && quantity(npc, 40) == 1, "combined gifts cannot exceed level budget");

    NpcBrainEffect fetch;
    fetch.type = "side_quest";
    fetch.kind = "fetch";
    fetch.pid = 19;
    fetch.quantity = 1;
    fetch.title = "Guard fetch";
    fetch.objective = "Bring one rock";
    fetch.rewardCaps = 7;
    fetch.rewardXp = 3;
    apply({ fetch, fetch });
    check(npcStoryState().quests.size() == 1, "one active quest per NPC per turn");
    NpcBrainEffect complete;
    complete.type = "complete_quest";
    complete.id = npcStoryState().quests.back().id;
    apply({ complete });
    check(npcStoryState().quests.back().state == "active" && itemGetTotalCaps(gDude) == initialCaps, "fetch completion requires actual inventory");
    Object* other = nullptr;
    check(objectCreateWithProtoId(&other, ProtoId(16777279)) == 0, "other giver fixture");
    apply({ complete }, "friendly", other);
    check(npcStoryState().quests.back().state == "active", "wrong giver cannot complete quest");
    check(npcStoryAbandon(complete.id), "active quest abandonment");
    apply({ complete });
    check(npcStoryState().quests.back().state == "abandoned" && itemGetTotalCaps(gDude) == initialCaps, "abandoned quest cannot pay out");

    int area = -1;
    for (int i = 0; i < wmAreaMaxCount(); i++) {
        char name[41] = {};
        wmGetAreaIdxName(static_cast<City>(i), name);
        if (std::string(name) == "Klamath") area = i;
    }
    check(area >= 0, "visit fixture area");
    wmAreaSetVisibleState(static_cast<City>(area), CITY_STATE_KNOWN, false);
    wmAreaMarkVisitedState(static_cast<City>(area), VisitedState::Known);
    NpcBrainEffect visit = fetch;
    visit.kind = "visit";
    visit.location = "Klamath";
    visit.title = "Guard visit";
    visit.objective = "Visit Klamath";
    apply({ visit });
    complete.id = npcStoryState().quests.back().id;
    apply({ complete });
    check(npcStoryState().quests.back().state == "active", "known area does not satisfy visit");
    wmAreaMarkVisitedState(static_cast<City>(area), VisitedState::Visited);
    apply({ complete });
    check(npcStoryState().quests.back().state == "done" && itemGetTotalCaps(gDude) == initialCaps + 7, "visited area completes and pays");
    int paid = itemGetTotalCaps(gDude);
    apply({ complete });
    check(itemGetTotalCaps(gDude) == paid, "quest reward cannot be replayed");
    NpcBrainEffect hostile;
    hostile.type = "turn_hostile";
    npcStoryBeginConversation();
    apply({ hostile }, "hostile");
    apply({ hostile }, "hostile");
    check(!scriptsHasPendingRequests(), "party member cannot be made hostile");
    partyMemberRemove(npc);
    npcStoryBeginConversation();
    apply({ hostile }, "hostile");
    check(!scriptsHasPendingRequests(), "first hostile reply cannot start combat");
    apply({}, "neutral");
    apply({ hostile }, "hostile");
    check(!scriptsHasPendingRequests(), "neutral reply breaks hostile streak");
    npcStoryBeginConversation();
    apply({ hostile }, "hostile");
    check(!scriptsHasPendingRequests(), "new conversation resets hostile streak");
    apply({ hostile }, "hostile");
    check(scriptsHasPendingRequests(), "second hostile reply queues native attack");
    return true;
}
} // namespace fallout
