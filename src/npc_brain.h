#ifndef FALLOUT_NPC_BRAIN_H_
#define FALLOUT_NPC_BRAIN_H_

#include <memory>
#include <string>
#include <vector>

namespace fallout {

// CE (Wasteland Reborn): free talk with NPCs through a local "brain" service
// (docs/BRAIN-API.md in the workspace). The service proposes a line and an
// intent; the engine validates the intent and runs any game action itself.
//
// Configured in fallout2.cfg:
//   [ai]
//   enabled=1
//   server=http://127.0.0.1:8820
//   token=<bearer token>
//   timeout_ms=25000
//
// Off, unreachable or without a persona for the speaker, no free talk is
// offered and dialogs are the original ones.

struct NpcBrainItem {
    int pid;
    std::string name;
    int quantity;
};

// CE: Typed story/effect contract; only the game thread may apply effects.
struct NpcBrainTimeline {
    std::string branch;
    long long head = 0;
    bool fork = false;
    bool present = true;
};
struct NpcBrainQuest {
    std::string location, text, state;
};
struct NpcBrainTown {
    std::string name, title;
    int reputation = 0;
};
struct WastelandQuest {
    std::string id, giver, giverName, title, objective, kind, location;
    int pid = 0, quantity = 1, rewardCaps = 0, rewardXp = 0;
    std::string state = "active";
};
struct WastelandDiscount {
    std::string npc;
    int percent = 0;
};
struct WastelandState {
    std::vector<WastelandDiscount> discounts;
    std::vector<WastelandQuest> quests;
    std::vector<std::string> revealed;
    int questCounter = 0;
};
struct NpcBrainStory {
    std::vector<NpcBrainQuest> quests;
    std::vector<NpcBrainTown> towns;
    std::string karmaTitle;
    std::vector<std::string> perksTitles, party, knownLocations;
    int killsTotal = 0, killsMen = 0, killsWomen = 0, killsChildren = 0;
    unsigned int gameTicks = 0;
    int npcTeam = 0;
    std::vector<WastelandQuest> wastelandQuests;
    int discountPercent = 0;
};
struct NpcBrainAllowedEffects {
    int maxDiscount = 0;
    std::vector<NpcBrainItem> gifts; // quantity is max_qty
    std::vector<std::string> revealLocations;
    bool sideQuest = false;
    int maxCaps = 200, maxXp = 100;
    std::vector<NpcBrainItem> fetchItems;
    std::vector<std::string> visitLocations, completeQuests;
    bool turnHostile = false;
};
struct NpcBrainEffect {
    std::string type, name, title, objective, kind, location, id;
    int percent = 0, pid = 0, quantity = 1, rewardCaps = 0, rewardXp = 0;
};

struct NpcBrainRequest {
    NpcBrainStory story;
    NpcBrainTimeline timeline;
    NpcBrainAllowedEffects allowedEffects;
    std::string campaign;
    std::string contentId = "fallout2";
    std::string npcInstance;
    int npcPid = 0;
    std::string npcName;
    std::string npcDescription;
    std::string map;
    std::string mapName;
    bool partyMember = false;
    bool canBarter = false;
    std::vector<NpcBrainItem> inventory;
    int npcCaps = 0;
    std::string playerName;
    std::string playerSex;
    int playerLevel = 1;
    int karma = 0;
    int stats[7] = { 0 };
    int playerCaps = 0;
    std::string date;
    std::string time;
    std::string reply;
    std::vector<std::string> options;
    std::vector<std::string> allowedIntents;
    std::string say;
};

enum class NpcBrainStatus {
    Pending,
    Done,
    Failed,
};

struct NpcBrainResponse {
    NpcBrainTimeline timeline { "", 0, false, false };
    std::vector<NpcBrainEffect> effects;
    std::string npcId;
    std::string say;
    std::string mood;
    std::string intent;
    std::string source;
    std::string error;
};

// CE: Save JSON is bounded and validated before replacing live state.
std::string wastelandStateToJson(const WastelandState& state);
bool wastelandStateFromJson(const std::string& json, WastelandState* state);
std::string npcBrainTimelineToJson(const NpcBrainTimeline& timeline);
bool npcBrainTimelineFromJson(const std::string& json, NpcBrainTimeline* timeline);

// One talk request running on its own thread. Safe to drop before it ends.
class NpcBrainCall {
public:
    virtual ~NpcBrainCall() = default;
    virtual NpcBrainStatus status() = 0;
    // Valid once status() is not Pending.
    virtual NpcBrainResponse response() = 0;
};

void npcBrainInit();
void npcBrainExit();

// Free talk is offered to [pid]: enabled, the service answered its persona
// list, and the list has [pid]. Never blocks (the list is fetched in the
// background, refreshed every minute).
bool npcBrainOffersTalk(int pid);

std::shared_ptr<NpcBrainCall> npcBrainTalk(const NpcBrainRequest& request);

// The engine's wait for an answer (the service's own timeout plus a margin).
unsigned int npcBrainTimeoutMs();

// Keeps printable ASCII (the game's fonts), at most [maxLength] characters.
std::string npcBrainSanitize(const std::string& text, size_t maxLength);

// Serialization and parsing, exposed for tests.
std::string npcBrainRequestToJson(const NpcBrainRequest& request);
bool npcBrainParseResponse(const std::string& body, const std::vector<std::string>& allowedIntents, NpcBrainResponse* response);

} // namespace fallout

#endif /* FALLOUT_NPC_BRAIN_H_ */
