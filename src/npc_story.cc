#include "npc_story.h"

#include <algorithm>
#include <climits>
#include <cstring>

#include "character_editor.h"
#include "combat.h"
#include "config.h"
#include "critter.h"
#include "db.h"
#include "debug.h"
#include "game.h"
#include "game_config.h"
#include "item.h"
#include "object.h"
#include "party_member.h"
#include "pipboy.h"
#include "proto.h"
#include "scripts.h"
#include "sfall_filesystem.h"
#include "stat.h"
#include "worldmap.h"

namespace fallout {
namespace {
    WastelandState state;
    NpcBrainTimeline timeline;
    std::string previousNpc, previousMood;

    std::string identity(Object* npc)
    {
        return std::to_string(scriptsSetUniqueObjectId(npc));
    }

    // CE: Fallout has no universal quest-item flag. Only these ordinary
    // consumables are offered; scripted, hidden and equipped items never are.
    bool safeItem(Object* item)
    {
        if (item == nullptr || item->sid != -1 || itemIsHidden(item)
            || (item->flags & OBJECT_EQUIPPED) != OBJECT_NONE) return false;
        static const int pids[] = { 19, 40, 48, 49, 144, 273 };
        if (std::find(std::begin(pids), std::end(pids), item->pid) == std::end(pids)) return false;
        Proto* proto = nullptr;
        return protoGetProto(item, &proto) == 0 && proto->item.sid == -1
            && (proto->item.extendedFlags & PROTO_EXT_FLAG_HIDDEN) == 0;
    }

    Object* inventoryItem(Object* owner, int pid, int quantity)
    {
        for (int i = 0; i < owner->data.inventory.length; i++) {
            auto& entry = owner->data.inventory.items[i];
            if (entry.item->pid == pid && entry.quantity >= quantity && safeItem(entry.item)) return entry.item;
        }
        return nullptr;
    }

    std::string areaName(int index)
    {
        char name[41] = {};
        wmGetAreaIdxName(static_cast<City>(index), name);
        name[40] = '\0';
        return name;
    }

    int findArea(const std::string& name)
    {
        for (int i = 0; i < wmAreaMaxCount(); i++)
            if (areaName(i) == name) return i;
        return -1;
    }

    std::vector<std::string> locations()
    {
        std::vector<std::string> names;
        File* file = fileOpen("wasteland/locations.txt", "rt");
        if (file == nullptr) return names; // no whitelist means no reveals
        char line[304];
        while (names.size() < 64 && fileReadString(line, sizeof(line), file) != nullptr) {
            std::string name = line;
            while (!name.empty() && (name.back() == '\r' || name.back() == '\n'))
                name.pop_back();
            if (!name.empty() && name[0] != '#' && findArea(name) != -1
                && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        }
        fileClose(file);
        return names;
    }

    bool requirement(const WastelandQuest& q)
    {
        if (q.state != "active") return false;
        if (q.kind == "fetch") return inventoryItem(gDude, q.pid, q.quantity) != nullptr;
        int area = findArea(q.location);
        return q.kind == "visit" && area != -1 && wmAreaVisitedState(static_cast<City>(area)) == VisitedState::Visited;
    }

    NpcBrainAllowedEffects allowed(Object* npc)
    {
        NpcBrainAllowedEffects out;
        if (!critterIsActive(npc) || isInCombat()) return out;
        out.maxDiscount = 20;
        int budget = 50 * std::min(std::max(pcGetStat(PC_STAT_LEVEL), 1), 99);
        for (int i = 0; i < npc->data.inventory.length && out.gifts.size() < 40; i++) {
            auto& entry = npc->data.inventory.items[i];
            if (!safeItem(entry.item)) continue;
            int cost = itemGetCost(entry.item);
            if (cost < 0) continue;
            int qty = std::min(entry.quantity, std::min(2, cost > 0 ? budget / cost : 2));
            if (qty > 0) out.gifts.push_back({ entry.item->pid, objectGetName(entry.item), qty });
        }
        auto whitelist = locations();
        out.visitLocations = whitelist;
        for (const auto& name : whitelist) {
            int area = findArea(name);
            if (area >= 0 && !wmAreaIsKnown(static_cast<City>(area))) out.revealLocations.push_back(name);
        }
        std::string id = identity(npc);
        out.sideQuest = state.quests.size() < 128 && state.questCounter < 1000000;
        for (const auto& q : state.quests) {
            if (q.giver != id || q.state != "active") continue;
            out.sideQuest = false;
            if (requirement(q)) out.completeQuests.push_back(q.id);
        }
        // CE: Ordinary rocks and medicine; no story-critical prototype is requested.
        out.fetchItems = { { 19, "Rock", 1 }, { 40, "Stimpak", 1 } };
        out.turnHostile = !objectIsPartyMember(npc);
        return out;
    }

    bool contains(const std::vector<std::string>& values, const std::string& value)
    {
        return std::find(values.begin(), values.end(), value) != values.end();
    }

    bool saveKey(const char* key, const std::string& json)
    {
        int id = sfallFileSystemFind(key);
        if (id == -1)
            id = sfallFileSystemCreate(key, json.size());
        else
            sfallFileSystemResize(id, json.size());
        if (id == -1 || sfallFileSystemSize(id) != static_cast<int>(json.size())) return false;
        sfallFileSystemSeek(id, 0);
        sfallFileSystemWriteString(id, json.c_str(), false);
        sfallFileSystemResize(id, -1); // CE VFS trailer persists the named entry
        return true;
    }

    std::string loadKey(const char* key)
    {
        int id = sfallFileSystemFind(key);
        int size = sfallFileSystemSize(id);
        if (id < 0 || size < 1 || size > 256 * 1024) return {};
        sfallFileSystemSeek(id, 0);
        std::string json;
        for (int i = 0; i < size; i++)
            json += static_cast<char>(sfallFileSystemRead(id, 1));
        return json;
    }
} // namespace

void npcStoryReset()
{
    state = WastelandState();
    timeline = NpcBrainTimeline();
    npcStoryBeginConversation();
}

void npcStoryBeginConversation()
{
    previousNpc.clear();
    previousMood.clear();
}

int npcStoryDiscount(Object* npc)
{
    if (npc == nullptr) return 0;
    std::string id = identity(npc);
    for (const auto& d : state.discounts)
        if (d.npc == id) return d.percent;
    return 0;
}

double npcStoryPriceMultiplier(Object* npc)
{
    bool enabled = false;
    configGetBool(&gGameConfig, "ai", "enabled", &enabled, false);
    if (!enabled) return 1.0;
    return (100 - npcStoryDiscount(npc)) * 0.01;
}

const WastelandState& npcStoryState() { return state; }
const NpcBrainTimeline& npcStoryTimeline() { return timeline; }

bool npcStoryAbandon(const std::string& id)
{
    for (auto& q : state.quests)
        if (q.id == id && q.state == "active") {
            q.state = "abandoned";
            return true;
        }
    return false;
}

void npcStoryFillRequest(Object* npc, NpcBrainRequest* request)
{
    request->timeline = timeline;
    request->allowedEffects = allowed(npc);
    auto& story = request->story;
    // CE: The Pip-Boy owns display/completion thresholds, including conversions.
    pipboyFillStoryQuests(&story);
    characterEditorFillStory(&story);
    for (KillType type = KILL_TYPE_FIRST; type < KILL_TYPE_DEFAULT_COUNT; type++)
        story.killsTotal += killsGetByType(type);
    story.killsMen = killsGetByType(KILL_TYPE_MAN);
    story.killsWomen = killsGetByType(KILL_TYPE_WOMAN);
    story.killsChildren = killsGetByType(KILL_TYPE_CHILD);
    for (Object* member : get_all_party_members_objects(false)) {
        if (member != gDude && story.party.size() < 64) story.party.push_back(critterGetName(member));
    }
    for (int i = 0; i < wmAreaMaxCount() && story.knownLocations.size() < 64; i++) {
        if (wmAreaIsKnown(static_cast<City>(i))) story.knownLocations.push_back(areaName(i));
    }
    story.gameTicks = gameTimeGetTime();
    story.npcTeam = npc->data.critter.combat.team;
    story.discountPercent = npcStoryDiscount(npc);
    for (const auto& q : state.quests)
        if (q.state == "active" && story.wastelandQuests.size() < 64) story.wastelandQuests.push_back(q);
}

std::vector<std::string> npcStoryApplyResponse(Object* npc, const NpcBrainResponse& response)
{
    std::vector<std::string> messages;
    if (response.timeline.present) {
        timeline = response.timeline;
        timeline.fork = false;
    }
    std::string id = identity(npc);
    bool twiceHostile = previousNpc == id && previousMood == "hostile" && response.mood == "hostile";
    previousNpc = id;
    previousMood = response.mood;
    int giftSpent = 0;
    for (size_t index = 0; index < response.effects.size() && index < 2; index++) {
        const auto& effect = response.effects[index];
        // CE: Recompute after each mutation. A second effect cannot spend stale inventory or duplicate a quest.
        auto bounds = allowed(npc);
        if (effect.type == "discount" && effect.percent > 0 && effect.percent <= bounds.maxDiscount) {
            auto it = std::find_if(state.discounts.begin(), state.discounts.end(), [&](const WastelandDiscount& d) { return d.npc == id; });
            if (it == state.discounts.end()) {
                if (state.discounts.size() >= 128) continue;
                state.discounts.push_back({ id, effect.percent });
            } else
                it->percent = std::max(it->percent, effect.percent);
            messages.push_back(std::string(critterGetName(npc)) + " offers a " + std::to_string(npcStoryDiscount(npc)) + "% discount.");
        } else if (effect.type == "gift" && effect.quantity > 0) {
            for (const auto& item : bounds.gifts)
                if (item.pid == effect.pid && effect.quantity <= item.quantity) {
                    Object* obj = inventoryItem(npc, effect.pid, effect.quantity);
                    int budget = 50 * std::min(std::max(pcGetStat(PC_STAT_LEVEL), 1), 99);
                    int cost = obj != nullptr ? itemStackCost(obj, effect.quantity) : -1;
                    if (cost < 0 || cost > budget - giftSpent) break;
                    std::string name = objectGetName(obj);
                    if (itemMove(npc, gDude, obj, effect.quantity) == 0) {
                        giftSpent += cost;
                        messages.push_back(std::string(critterGetName(npc)) + " gives you " + std::to_string(effect.quantity) + " " + name + ".");
                    }
                    break;
                }
        } else if (effect.type == "reveal_location" && contains(bounds.revealLocations, effect.name)) {
            int area = findArea(effect.name);
            if (area != -1 && wmAreaSetVisibleState(static_cast<City>(area), CITY_STATE_KNOWN, false)
                && wmAreaMarkVisitedState(static_cast<City>(area), VisitedState::Known)) {
                if (!contains(state.revealed, effect.name) && state.revealed.size() < 64) state.revealed.push_back(effect.name);
                messages.push_back(effect.name + " marked on your map.");
            }
        } else if (effect.type == "side_quest" && bounds.sideQuest && !effect.title.empty() && !effect.objective.empty()
            && effect.rewardCaps >= 0 && effect.rewardCaps <= bounds.maxCaps && effect.rewardXp >= 0 && effect.rewardXp <= bounds.maxXp) {
            bool valid = effect.kind == "visit" && contains(bounds.visitLocations, effect.location);
            for (const auto& item : bounds.fetchItems)
                if (effect.kind == "fetch" && effect.pid == item.pid && effect.quantity >= 1 && effect.quantity <= 10) valid = true;
            if (!valid) continue;
            WastelandQuest q;
            q.id = "q-" + std::to_string(++state.questCounter);
            q.giver = id;
            q.giverName = critterGetName(npc);
            q.title = npcBrainSanitize(effect.title, 60);
            q.objective = npcBrainSanitize(effect.objective, 200);
            q.kind = effect.kind;
            q.location = effect.location;
            q.pid = effect.pid;
            q.quantity = effect.quantity;
            q.rewardCaps = effect.rewardCaps;
            q.rewardXp = effect.rewardXp;
            state.quests.push_back(q);
            messages.push_back("New quest: " + q.title + ".");
        } else if (effect.type == "complete_quest" && contains(bounds.completeQuests, effect.id)) {
            for (auto& q : state.quests)
                if (q.id == effect.id && q.giver == id && requirement(q)) {
                    // CE: Move the fetch to its giver with the inventory API, never modify quest scripts.
                    if (q.kind == "fetch") {
                        Object* item = inventoryItem(gDude, q.pid, q.quantity);
                        if (item == nullptr || itemMoveForce(gDude, npc, item, q.quantity) != 0) break;
                    }
                    if (itemCapsAdjust(gDude, q.rewardCaps) != 0) {
                        if (q.kind == "fetch") {
                            Object* item = inventoryItem(npc, q.pid, q.quantity);
                            if (item != nullptr) itemMoveForce(npc, gDude, item, q.quantity);
                        }
                        break;
                    }
                    q.state = "done";
                    pcAddExperience(q.rewardXp);
                    messages.push_back("Quest completed: " + q.title + ". +" + std::to_string(q.rewardCaps) + " caps, +" + std::to_string(q.rewardXp) + " XP.");
                    break;
                }
        } else if (effect.type == "turn_hostile" && bounds.turnHostile && twiceHostile && critterIsActive(gDude)) {
            CombatStartData combat = {};
            combat.attacker = npc;
            combat.defender = gDude;
            combat.maxDamage = INT_MAX;
            scriptsRequestCombat(&combat);
            messages.push_back(std::string(critterGetName(npc)) + " attacks!");
        }
    }
    for (const auto& msg : messages)
        debugPrint("\nWasteland: %s\n", msg.c_str());
    return messages;
}

bool npcStorySave()
{
    return saveKey("wasteland.timeline", npcBrainTimelineToJson(timeline))
        && saveKey("wasteland.state", wastelandStateToJson(state));
}

void npcStoryLoad()
{
    npcStoryReset();
    std::string json = loadKey("wasteland.timeline");
    if (!json.empty()) npcBrainTimelineFromJson(json, &timeline);
    timeline.fork = true;
    json = loadKey("wasteland.state");
    if (!json.empty() && !wastelandStateFromJson(json, &state)) debugPrint("\nWasteland: invalid saved state ignored.\n");
}
} // namespace fallout
