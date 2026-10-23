#ifndef FALLOUT_NPC_STORY_H_
#define FALLOUT_NPC_STORY_H_
#include "db.h"
#include "npc_brain.h"
namespace fallout {
struct Object;
void npcStoryReset();
void npcStoryBeginConversation();
void npcStoryFillRequest(Object* speaker, NpcBrainRequest* request);
std::vector<std::string> npcStoryApplyResponse(Object* speaker, const NpcBrainResponse& response);
int npcStoryDiscount(Object* speaker);
double npcStoryPriceMultiplier(Object* speaker);
const WastelandState& npcStoryState();
const NpcBrainTimeline& npcStoryTimeline();
bool npcStoryAbandon(const std::string& id);
bool npcStorySave();
void npcStoryLoad();
} // namespace fallout
#endif
