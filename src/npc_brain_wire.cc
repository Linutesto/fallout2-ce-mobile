#include "npc_brain.h"
#include "npc_brain_json.h"
namespace fallout {
using namespace brainJson;
std::string npcBrainSanitize(const std::string& text, size_t maxLength)
{
    std::string out;
    for (size_t index = 0; index < text.size() && out.size() < maxLength; index++) {
        unsigned char ch = static_cast<unsigned char>(text[index]);
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            if (!out.empty() && out.back() != ' ') {
                out += ' ';
            }
        } else if (ch >= 0x20 && ch < 0x7F) {
            out += static_cast<char>(ch);
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string npcBrainRequestToJson(const NpcBrainRequest& request)
{
    std::string out = "{";
    jsonAppendKey(out, "v");
    out += "1,";
    jsonAppendKey(out, "campaign");
    jsonAppendString(out, request.campaign);
    out += ",";
    jsonAppendKey(out, "content");
    jsonAppendString(out, request.contentId);

    out += ",";
    jsonAppendKey(out, "npc");
    out += "{";
    jsonAppendKey(out, "pid");
    out += std::to_string(request.npcPid) + ",";
    jsonAppendKey(out, "name");
    jsonAppendString(out, request.npcName);
    out += ",";
    jsonAppendKey(out, "description");
    jsonAppendString(out, request.npcDescription);
    out += ",";
    jsonAppendKey(out, "instance");
    jsonAppendString(out, request.npcInstance);
    out += ",";
    jsonAppendKey(out, "map");
    jsonAppendString(out, request.map);
    out += ",";
    jsonAppendKey(out, "party_member");
    out += request.partyMember ? "true," : "false,";
    jsonAppendKey(out, "can_barter");
    out += request.canBarter ? "true," : "false,";
    jsonAppendKey(out, "inventory");
    out += "[";
    for (size_t index = 0; index < request.inventory.size(); index++) {
        const NpcBrainItem& item = request.inventory[index];
        if (index != 0) {
            out += ",";
        }
        out += "{";
        jsonAppendKey(out, "pid");
        out += std::to_string(item.pid) + ",";
        jsonAppendKey(out, "name");
        jsonAppendString(out, item.name);
        out += ",";
        jsonAppendKey(out, "qty");
        out += std::to_string(item.quantity) + "}";
    }
    out += "],";
    jsonAppendKey(out, "caps");
    out += std::to_string(request.npcCaps) + "}";

    out += ",";
    jsonAppendKey(out, "player");
    out += "{";
    jsonAppendKey(out, "name");
    jsonAppendString(out, request.playerName);
    out += ",";
    jsonAppendKey(out, "sex");
    jsonAppendString(out, request.playerSex);
    out += ",";
    jsonAppendKey(out, "level");
    out += std::to_string(request.playerLevel) + ",";
    jsonAppendKey(out, "karma");
    out += std::to_string(request.karma) + ",";
    jsonAppendKey(out, "stats");
    static const char* kStatKeys[7] = { "st", "pe", "en", "ch", "in", "ag", "lk" };
    out += "{";
    for (int index = 0; index < 7; index++) {
        if (index != 0) {
            out += ",";
        }
        jsonAppendKey(out, kStatKeys[index]);
        out += std::to_string(request.stats[index]);
    }
    out += "},";
    jsonAppendKey(out, "caps");
    out += std::to_string(request.playerCaps) + "}";

    out += ",";
    jsonAppendKey(out, "world");
    out += "{";
    jsonAppendKey(out, "date");
    jsonAppendString(out, request.date);
    out += ",";
    jsonAppendKey(out, "time");
    jsonAppendString(out, request.time);
    out += ",";
    jsonAppendKey(out, "map_name");
    jsonAppendString(out, request.mapName);
    out += "}";

    out += ",";
    jsonAppendKey(out, "dialog");
    out += "{";
    jsonAppendKey(out, "reply");
    jsonAppendString(out, request.reply);
    out += ",";
    jsonAppendKey(out, "options");
    jsonAppendStringArray(out, request.options);
    out += "}";

    out += ",";
    jsonAppendKey(out, "allowed_intents");
    jsonAppendStringArray(out, request.allowedIntents);
    out += ",";
    jsonAppendKey(out, "say");
    jsonAppendString(out, request.say);
    out += "}";
    return out;
}

bool npcBrainParseResponse(const std::string& body, const std::vector<std::string>& allowedIntents, NpcBrainResponse* response)
{
    JsonValue root;
    JsonParser parser(body);
    if (!parser.parse(&root) || root.type != JsonValue::Type::Object) {
        response->error = "answer is not a JSON object";
        return false;
    }

    const JsonValue* say = root.get("say");
    if (say == nullptr || say->type != JsonValue::Type::String) {
        response->error = "answer has no line";
        return false;
    }

    response->say = npcBrainSanitize(say->string, 400);
    if (response->say.empty()) {
        response->error = "answer line is empty";
        return false;
    }

    auto stringField = [&root](const char* key) {
        const JsonValue* value = root.get(key);
        return value != nullptr && value->type == JsonValue::Type::String ? value->string : std::string();
    };

    response->npcId = npcBrainSanitize(stringField("npc_id"), 64);
    response->mood = npcBrainSanitize(stringField("mood"), 16);
    response->source = npcBrainSanitize(stringField("source"), 16);

    // The engine decides: an intent it didn't offer is none.
    std::string intent = stringField("intent");
    response->intent = "none";
    for (const std::string& allowed : allowedIntents) {
        if (intent == allowed) {
            response->intent = intent;
            break;
        }
    }
    return true;
}

}
