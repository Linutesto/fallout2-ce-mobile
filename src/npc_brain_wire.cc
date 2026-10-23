#include "npc_brain.h"
#include <climits>
#include <cmath>
#include <cctype>
#include <set>
#include "npc_brain_json.h"
namespace fallout {
using namespace brainJson;
namespace {
    // CE: Construct JSON values rather than accepting raw fragments from game text.
    JsonValue obj() { JsonValue v; v.type=JsonValue::Type::Object; return v; }
    JsonValue arr() { JsonValue v; v.type=JsonValue::Type::Array; return v; }
    JsonValue str(const std::string& s, size_t limit=300) { JsonValue v; v.type=JsonValue::Type::String; v.string=s.substr(0,limit); return v; }
    JsonValue num(long long n) { JsonValue v; v.type=JsonValue::Type::Number; v.number=n; return v; }
    JsonValue boolean(bool b) { JsonValue v; v.type=JsonValue::Type::Bool; v.boolean=b; return v; }
    void set(JsonValue& v, const char* k, JsonValue x) { v.object.emplace_back(k,std::move(x)); }
    void text(JsonValue& v, const char* k, const std::string& s, size_t limit=300) { set(v,k,str(s,limit)); }
    void integer(JsonValue& v, const char* k, long long n) { set(v,k,num(n)); }
    JsonValue strings(const std::vector<std::string>& values) {
        JsonValue v=arr(); for (const auto& x:values) { if(v.array.size()==64)break; v.array.push_back(str(x)); } return v;
    }
    std::string encode(const JsonValue& v) {
        std::string out;
        switch(v.type) {
        case JsonValue::Type::String: jsonAppendString(out,v.string);break;
        case JsonValue::Type::Number: out=std::to_string(static_cast<long long>(v.number));break;
        case JsonValue::Type::Bool: out=v.boolean?"true":"false";break;
        case JsonValue::Type::Array:
            out="[";for(size_t i=0;i<v.array.size();i++) { if(i)out+=',';out+=encode(v.array[i]); }out+=']';break;
        case JsonValue::Type::Object:
            out="{";for(size_t i=0;i<v.object.size();i++) { if(i)out+=',';jsonAppendString(out,v.object[i].first);out+=':';out+=encode(v.object[i].second); }out+='}';break;
        default: out="null";
        }
        return out;
    }
    bool getInt(const JsonValue* v, long long* n, long long lo=0, long long hi=INT_MAX) {
        if(!v || v->type!=JsonValue::Type::Number || !std::isfinite(v->number) || std::floor(v->number)!=v->number || v->number<lo || v->number>hi)return false;
        *n=static_cast<long long>(v->number);return true;
    }
    bool getInt(const JsonValue& v,const char* key,int* n,int lo=0,int hi=INT_MAX) {
        long long value; if(!getInt(v.get(key),&value,lo,hi))return false;*n=static_cast<int>(value);return true;
    }
    bool getText(const JsonValue& v,const char* key,std::string* out,size_t limit=300,bool empty=false) {
        auto x=v.get(key);if(!x || x->type!=JsonValue::Type::String || x->string.size()>limit || (!empty && x->string.empty()))return false;
        *out=npcBrainSanitize(x->string,limit);return empty || !out->empty();
    }
    bool parseTimeline(const JsonValue& v,NpcBrainTimeline* out,bool allowEmpty=false) {
        NpcBrainTimeline t;
        if(!getText(v,"branch",&t.branch,128,allowEmpty) || !getInt(v.get("head"),&t.head,0,9007199254740991LL))return false;
        for(unsigned char c:t.branch)if(!std::isalnum(c) && c!='-' && c!='_' && c!=':' && c!='|' && c!=' ')return false;
        if(auto f=v.get("fork")) { if(f->type!=JsonValue::Type::Bool)return false;t.fork=f->boolean; }
        *out=t;return true;
    }
    JsonValue timelineValue(const NpcBrainTimeline& t) {
        auto v=obj();text(v,"branch",t.branch,128);integer(v,"head",t.head);set(v,"fork",boolean(t.fork));return v;
    }
    JsonValue questValue(const WastelandQuest& q,bool full) {
        auto v=obj();text(v,"id",q.id);text(v,"giver",full?q.giver:q.giverName);text(v,"title",q.title,60);text(v,"state",q.state);
        if(full) {
            text(v,"giver_name",q.giverName);text(v,"objective",q.objective,200);text(v,"kind",q.kind);text(v,"location",q.location);
            integer(v,"pid",q.pid);integer(v,"qty",q.quantity);integer(v,"reward_caps",q.rewardCaps);integer(v,"reward_xp",q.rewardXp);
        }
        return v;
    }
    JsonValue storyValue(const NpcBrainStory& s) {
        auto v=obj(), quests=arr(),towns=arr(),kills=obj(),wasteland=obj(),wq=arr();
        for(const auto& q:s.quests) { if(quests.array.size()==40)break;auto x=obj();text(x,"location",q.location);text(x,"text",q.text);text(x,"state",q.state);quests.array.push_back(x); }
        for(const auto& t:s.towns) { if(towns.array.size()==20)break;auto x=obj();text(x,"name",t.name);text(x,"title",t.title);integer(x,"reputation",t.reputation);towns.array.push_back(x); }
        for(const auto& q:s.wastelandQuests) { if(wq.array.size()==64)break;wq.array.push_back(questValue(q,false)); }
        integer(kills,"total",s.killsTotal);integer(kills,"men",s.killsMen);integer(kills,"women",s.killsWomen);integer(kills,"children",s.killsChildren);
        set(v,"quests",quests);set(v,"towns",towns);text(v,"karma_title",s.karmaTitle);set(v,"kills",kills);
        set(v,"party",strings(s.party));set(v,"perks_titles",strings(s.perksTitles));set(v,"known_locations",strings(s.knownLocations));
        integer(v,"game_ticks",s.gameTicks);integer(v,"npc_team",s.npcTeam);
        set(wasteland,"quests",wq);integer(wasteland,"discount_percent",s.discountPercent);set(v,"wasteland",wasteland);return v;
    }
    JsonValue itemsValue(const std::vector<NpcBrainItem>& items,bool gifts) {
        auto v=arr();for(const auto& item:items) { if(v.array.size()==64)break;auto x=obj();integer(x,"pid",item.pid);text(x,"name",item.name);if(gifts)integer(x,"max_qty",item.quantity);v.array.push_back(x); }return v;
    }
    JsonValue allowedValue(const NpcBrainAllowedEffects& e) {
        auto v=obj();
        if(e.maxDiscount>0) { auto x=obj();integer(x,"max_percent",e.maxDiscount);set(v,"discount",x); }
        if(!e.gifts.empty()) { auto x=obj();set(x,"items",itemsValue(e.gifts,true));set(v,"gift",x); }
        if(!e.revealLocations.empty()) { auto x=obj();set(x,"names",strings(e.revealLocations));set(v,"reveal_location",x); }
        if(e.sideQuest) { auto x=obj();integer(x,"max_caps",e.maxCaps);integer(x,"max_xp",e.maxXp);set(x,"fetch_items",itemsValue(e.fetchItems,false));set(x,"visit_locations",strings(e.visitLocations));set(v,"side_quest",x); }
        if(!e.completeQuests.empty()) { auto x=obj();set(x,"ids",strings(e.completeQuests));set(v,"complete_quest",x); }
        if(e.turnHostile)set(v,"turn_hostile",obj());return v;
    }
    bool parseEffect(const JsonValue& v,NpcBrainEffect* e) {
        if(!getText(v,"type",&e->type,32))return false;
        if(e->type=="discount")return getInt(v,"percent",&e->percent,1,20);
        if(e->type=="gift")return getInt(v,"pid",&e->pid) && getInt(v,"qty",&e->quantity,1,10);
        if(e->type=="reveal_location")return getText(v,"name",&e->name);
        if(e->type=="complete_quest")return getText(v,"id",&e->id,64);
        if(e->type=="turn_hostile")return true;
        if(e->type!="side_quest" || !getText(v,"title",&e->title,60) || !getText(v,"objective",&e->objective,200) || !getText(v,"kind",&e->kind,16)
            || !getInt(v,"reward_caps",&e->rewardCaps,0,200) || !getInt(v,"reward_xp",&e->rewardXp,0,100))return false;
        if(e->kind=="fetch")return getInt(v,"pid",&e->pid) && getInt(v,"qty",&e->quantity,1,10);
        return e->kind=="visit" && getText(v,"location",&e->location);
    }
}

std::string npcBrainTimelineToJson(const NpcBrainTimeline& t) { return encode(timelineValue(t)); }
bool npcBrainTimelineFromJson(const std::string& json,NpcBrainTimeline* out) {
    if(out==nullptr || json.size()>1024)return false;
    JsonValue v;return JsonParser(json).parse(&v) && parseTimeline(v,out,true);
}
std::string wastelandStateToJson(const WastelandState& s) {
    auto v=obj(),discounts=arr(),quests=arr();
    for(const auto& d:s.discounts) { auto x=obj();text(x,"npc",d.npc);integer(x,"percent",d.percent);discounts.array.push_back(x); }
    for(const auto& q:s.quests)quests.array.push_back(questValue(q,true));
    set(v,"discounts",discounts);set(v,"quests",quests);set(v,"revealed",strings(s.revealed));integer(v,"quest_counter",s.questCounter);return encode(v);
}
bool wastelandStateFromJson(const std::string& json,WastelandState* out) {
    if(!out || json.size()>256*1024)return false;
    JsonValue v;if(!JsonParser(json).parse(&v))return false;
    WastelandState s; if(!getInt(v,"quest_counter",&s.questCounter,0,1000000))return false;
    auto ds=v.get("discounts"),qs=v.get("quests"),rs=v.get("revealed");
    if(!ds || ds->type!=JsonValue::Type::Array || ds->array.size()>128 || !qs || qs->type!=JsonValue::Type::Array || qs->array.size()>128
        || !rs || rs->type!=JsonValue::Type::Array || rs->array.size()>64)return false;
    std::set<std::string> ids,npcs;
    for(const auto& x:ds->array) {
        WastelandDiscount d;if(!getText(x,"npc",&d.npc,128) || !getInt(x,"percent",&d.percent,1,20) || !npcs.insert(d.npc).second)return false;s.discounts.push_back(d);
    }
    for(const auto& x:qs->array) {
        WastelandQuest q;
        if(!getText(x,"id",&q.id,64) || !ids.insert(q.id).second || !getText(x,"giver",&q.giver,128) || !getText(x,"giver_name",&q.giverName)
            || !getText(x,"title",&q.title,60) || !getText(x,"objective",&q.objective,200) || !getText(x,"kind",&q.kind,16)
            || !getText(x,"state",&q.state,16) || (q.state!="active" && q.state!="done" && q.state!="abandoned")
            || !getInt(x,"reward_caps",&q.rewardCaps,0,200) || !getInt(x,"reward_xp",&q.rewardXp,0,100))return false;
        if(q.kind=="fetch") { if(!getInt(x,"pid",&q.pid) || !getInt(x,"qty",&q.quantity,1,10))return false; }
        else if(q.kind=="visit") { if(!getText(x,"location",&q.location))return false; }
        else return false;
        s.quests.push_back(q);
    }
    for(const auto& x:rs->array) { if(x.type!=JsonValue::Type::String || x.string.empty() || x.string.size()>300)return false;s.revealed.push_back(npcBrainSanitize(x.string,300)); }
    *out=std::move(s);return true;
}

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
    jsonAppendKey(out, "story");
    out += encode(storyValue(request.story)) + ",";
    if (request.timeline.present) {
        jsonAppendKey(out, "timeline");
        out += encode(timelineValue(request.timeline)) + ",";
    }
    jsonAppendKey(out, "allowed_effects");
    out += encode(allowedValue(request.allowedEffects)) + ",";
    jsonAppendKey(out, "say");
    jsonAppendString(out, request.say);
    out += "}";
    return out;
}

bool npcBrainParseResponse(const std::string& body, const std::vector<std::string>& allowedIntents, NpcBrainResponse* response)
{
    if (response == nullptr) return false;
    *response = NpcBrainResponse();
    if (body.size() > 256 * 1024) return false;
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
    if (const auto* t = root.get("timeline")) parseTimeline(*t, &response->timeline);
    if (const auto* effects = root.get("effects")) {
        if (effects->type == JsonValue::Type::Array) {
            for (const auto& item : effects->array) {
                if (response->effects.size() == 2) break;
                NpcBrainEffect effect;
                if (parseEffect(item, &effect)) response->effects.push_back(effect);
            }
        }
    }
    return true;
}

}
