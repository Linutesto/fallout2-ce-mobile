#include "mui_screens.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "art.h"
#include "critter.h"
#include "item.h"
#include "mui_icons.h"
#include "mui_notify.h"
#include "object.h"
#include "proto_types.h"

namespace fallout {

namespace {

    // Indexed by `MuiItemFilter`.
    const MuiIcon kFilterIcons[static_cast<int>(MuiItemFilter::Count)] = {
        MuiIcon::FilterAll,
        MuiIcon::FilterWeapons,
        MuiIcon::FilterWeaponsAndAmmo,
        MuiIcon::FilterArmor,
        MuiIcon::FilterDrugs,
        MuiIcon::FilterAmmo,
        MuiIcon::FilterMisc,
    };

} // namespace

bool muiItemMatchesFilter(Object* item, const MuiItemFilterState& state)
{
    int type = itemGetType(item);
    switch (state.filter) {
    case MuiItemFilter::All:
        return true;
    case MuiItemFilter::Weapons:
        if (type != ITEM_TYPE_WEAPON) {
            return false;
        }
        // Firearms and energy weapons use ammo.
        return !state.gunsOnly || ammoGetCapacity(item) > 0;
    case MuiItemFilter::WeaponsAndAmmo:
        return type == ITEM_TYPE_WEAPON || type == ITEM_TYPE_AMMO;
    case MuiItemFilter::Armor:
        return type == ITEM_TYPE_ARMOR;
    case MuiItemFilter::Drugs:
        return type == ITEM_TYPE_DRUG;
    case MuiItemFilter::Ammo:
        return type == ITEM_TYPE_AMMO;
    case MuiItemFilter::Misc:
        return type == ITEM_TYPE_MISC || type == ITEM_TYPE_CONTAINER || type == ITEM_TYPE_KEY;
    default:
        return true;
    }
}

void muiItemFilterRow(MuiContext& ui, const std::string& id, const MuiRect& rect, MuiItemFilterState* state)
{
    const MuiTheme& theme = muiTheme();
    int count = static_cast<int>(MuiItemFilter::Count);
    float gap = ui.dp(3.0f);
    float width = (rect.w - gap * (count - 1)) / count;

    for (int index = 0; index < count; index++) {
        MuiRect button = { rect.x + index * (width + gap), rect.y, width, rect.h };
        MuiItemFilter filter = static_cast<MuiItemFilter>(index);

        bool pressed;
        bool longPressed;
        bool tapped = ui.touchable(id + "." + std::to_string(index), button, &pressed, &longPressed);

        if (tapped) {
            state->filter = filter;
            state->gunsOnly = false;
        } else if (longPressed && filter == MuiItemFilter::Weapons) {
            state->filter = filter;
            state->gunsOnly = !state->gunsOnly;
        }

        bool active = state->filter == filter;
        MuiColor fill = pressed ? theme.buttonPressed : (active ? theme.buttonPrimary : theme.button);
        MuiColor color = active && !pressed ? theme.buttonPrimaryText : theme.accent;
        muiFillRoundRect(button, ui.dp(5.0f), fill);
        muiStrokeRoundRect(button, ui.dp(5.0f), ui.dp(1.0f), active ? theme.accent : theme.buttonBorder);

        float size = std::min(button.w, button.h) * 0.52f;
        muiDrawIcon(kFilterIcons[index], button.centerX(), button.centerY(), size, ui.dp(1.6f), color);

        // Firearms only: a dot in the corner.
        if (active && filter == MuiItemFilter::Weapons && state->gunsOnly) {
            muiFillCircle(button.right() - ui.dp(5.0f), button.y + ui.dp(5.0f), ui.dp(2.5f), color);
        }
    }
}

std::vector<MuiListItem> muiInventoryItems(Object* owner, const MuiItemFilterState& state)
{
    std::vector<MuiListItem> items;
    if (owner == nullptr) {
        return items;
    }

    // The game lists the last added item first.
    Inventory* inventory = &(owner->data.inventory);
    for (int index = inventory->length - 1; index >= 0; index--) {
        InventoryItem* inventoryItem = &(inventory->items[index]);
        if (muiItemMatchesFilter(inventoryItem->item, state)) {
            items.push_back({ inventoryItem->item, inventoryItem->quantity });
        }
    }

    // Money first.
    std::stable_partition(items.begin(), items.end(), [](const MuiListItem& entry) {
        return ProtoId(entry.item) == ItemProtoTypeId::Money;
    });

    return items;
}

int muiItemsWeight(const std::vector<MuiListItem>& items)
{
    int weight = 0;
    for (const MuiListItem& entry : items) {
        weight += itemStackWeight(entry.item, entry.quantity);
    }
    return weight;
}

void muiNotifyPartyMemberOutOfReach(Object* critter)
{
    // Text in `game\ce.msg`.
    constexpr int kTextOutOfReach = 326;

    char text[160];
    snprintf(text, sizeof(text), muiText(kTextOutOfReach, "%s is too far away."), critterGetName(critter));
    muiNotify(text, MuiNoticeKind::Warning);
}

void muiDrawItemImage(MuiContext& ui, const MuiRect& rect, Object* item, bool pressed)
{
    const MuiTheme& theme = muiTheme();
    muiFillRoundRect(rect, ui.dp(5.0f), pressed ? theme.buttonPressed : theme.button);
    muiStrokeRoundRect(rect, ui.dp(5.0f), ui.dp(1.0f), pressed ? theme.accent : theme.buttonBorder);

    int width;
    int height;
    SDL_Texture* texture = muiItemArtTexture(itemGetInventoryFrmId(item), 0, &width, &height);
    if (texture != nullptr && width > 0 && height > 0) {
        // Integer scale keeps pixels even, smaller art is fitted.
        MuiRect area = rect.inset(ui.dp(4.0f));
        float scale = std::min(area.w / width, area.h / height);
        if (scale >= 1.0f) {
            scale = std::floor(scale);
        }
        float w = width * scale;
        float h = height * scale;
        muiDrawTexture(texture, { area.centerX() - w / 2.0f, area.centerY() - h / 2.0f, w, h });
    }
}

void muiDrawItemCell(MuiContext& ui, const MuiRect& rect, Object* item, int quantity, bool pressed)
{
    const MuiTheme& theme = muiTheme();
    muiDrawItemImage(ui, rect, item, pressed);

    bool money = ProtoId(item) == ItemProtoTypeId::Money;
    int amount = itemStackAmount(item, quantity);
    if (money || amount > 1 || itemGetType(item) == ITEM_TYPE_AMMO) {
        std::string label = (money ? "$" : "x") + std::to_string(amount);
        float size = ui.dp(11.0f);
        MuiRect labelRect = { rect.x, rect.bottom() - muiLineHeight(size) - ui.dp(2.0f), rect.w - ui.dp(4.0f), muiLineHeight(size) };
        muiDrawTextAligned(muiDecodeUtf8(label.c_str()), labelRect, size, theme.text, MuiAlign::End, MuiAlign::Start);
    }
}

void muiDrawDraggedItem(MuiContext& ui, Object* item, int quantity, int price)
{
    const MuiTheme& theme = muiTheme();

    float size = ui.dp(64.0f);
    MuiRect cell = { ui.pointerX() - size / 2.0f, ui.pointerY() - size * 1.1f, size, size };
    muiDrawItemCell(ui, cell, item, quantity, true);

    std::u32string name = muiDecodeGameText(objectGetName(item));
    std::u32string amount = muiDecodeUtf8(("x" + std::to_string(itemStackAmount(item, 1))).c_str());
    std::u32string weight = muiDecodeUtf8(std::to_string(itemStackWeight(item, 1)).c_str());
    std::u32string cost = price >= 0 ? muiDecodeUtf8(("$" + std::to_string(price)).c_str()) : U"";

    float textSize = ui.dp(13.0f);
    float iconSize = ui.dp(13.0f);
    float gap = ui.dp(10.0f);
    float width = ui.dp(10.0f) + muiTextWidth(name, textSize)
        + gap + muiTextWidth(amount, textSize)
        + gap + iconSize + ui.dp(4.0f) + muiTextWidth(weight, textSize)
        + (cost.empty() ? 0.0f : gap + muiTextWidth(cost, textSize))
        + ui.dp(10.0f);
    MuiRect bubble = { std::clamp(ui.pointerX() - width / 2.0f, 0.0f, ui.screenRect().w - width), cell.y - ui.dp(30.0f), width, ui.dp(26.0f) };
    muiFillRoundRect(bubble, ui.dp(6.0f), theme.panel);
    muiStrokeRoundRect(bubble, ui.dp(6.0f), ui.dp(1.0f), theme.accent);

    float x = bubble.x + ui.dp(10.0f);
    auto text = [&](const std::u32string& value, const MuiColor& color) {
        muiDrawTextAligned(value, { x, bubble.y, width, bubble.h }, textSize, color, MuiAlign::Start, MuiAlign::Center);
        x += muiTextWidth(value, textSize);
    };
    text(name, theme.text);
    x += gap;
    text(amount, theme.textDim);
    x += gap;
    muiDrawIcon(MuiIcon::Weight, x + iconSize / 2.0f, bubble.centerY(), iconSize, ui.dp(1.3f), theme.textDim);
    x += iconSize + ui.dp(4.0f);
    text(weight, theme.text);
    if (!cost.empty()) {
        x += gap;
        text(cost, theme.text);
    }
}

} // namespace fallout
