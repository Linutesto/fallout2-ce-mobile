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

struct NpcBrainRequest {
    std::string campaign;
    std::string contentId = "fallout2";
    std::string npcInstance;
    int npcPid = 0;
    std::string npcName;
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
    std::string npcId;
    std::string say;
    std::string mood;
    std::string intent;
    std::string source;
    std::string error;
};

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
