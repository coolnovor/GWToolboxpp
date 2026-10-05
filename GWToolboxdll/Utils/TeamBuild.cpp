#include "stdafx.h"

#include <Utils/TeamBuild.h>

#include <GWCA/Constants/Skills.h>
#include <GWCA/Context/GameContext.h>
#include <GWCA/Context/WorldContext.h>
#include <GWCA/GameContainers/Array.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Hero.h>
#include <GWCA/GameEntities/Map.h>
#include <GWCA/GameEntities/Party.h>
#include <GWCA/GameEntities/Skill.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/SkillbarMgr.h>
#include <GWCA/Managers/UIMgr.h>

#include <Color.h>
#include <GWCA/Managers/PartyMgr.h>
#include <Logger.h>
#include <Modules/Resources.h>
#include <Timer.h>
#include <Utils/GuiUtils.h>
#include <Utils/TextUtils.h>
#include <Windows/BuildsWindow.h>
#include <Windows/PconsWindow.h>
#include <Windows/RerollWindow.h>
#include "TeamBuildEncoder.h"
#include "ToolboxUtils.h"

namespace {

    IDirect3DTexture9** skill_toggle_sprite = nullptr;

    const Color build_edit_pcon_enabled_color = Colors::ARGB(102, 0, 255, 0);

    using GW::Constants::HeroID;

    // ----------------------------------------------------------------
    // 异步Build加载基础设施
    // ----------------------------------------------------------------

    struct PendingBuildLoad {
        Build build;
        enum Stage : uint8_t { AddHero, WaitForHero, Finished } stage = AddHero;
        size_t party_hero_index = 0xFFFFFFFF;
        clock_t started = 0;

        bool Process();
    };

    std::vector<PendingBuildLoad> pending_build_loads;
    std::queue<std::wstring> send_queue;
    Build* pending_reroll_build = 0;
    std::wstring pending_reroll_character;
    clock_t pending_reroll_timer = 0;
    clock_t send_timer = 0;
    clock_t kickall_timer = 0;

    size_t GetPlayerHeroCount()
    {
        size_t ret = 0;
        const GW::PartyInfo* party_info = GW::PartyMgr::GetPartyInfo();
        if (!party_info) return ret;
        const GW::HeroPartyMemberArray& heroes = party_info->heroes;
        if (!heroes.valid()) return ret;
        const GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
        if (!me) return ret;
        const uint32_t my_id = me->login_number;
        for (size_t i = 0; i < heroes.size(); i++) {
            if (heroes[i].owner_player_id == my_id) ret++;
        }
        return ret;
    }

    const GW::HeroFlag* GetHeroFlagInfo(const uint32_t hero_id)
    {
        const GW::GameContext* g = GW::GetGameContext();
        if (!g || !g->world) return nullptr;
        for (const GW::HeroFlag& flag : g->world->hero_flags) {
            if (flag.hero_id == hero_id) return &flag;
        }
        return nullptr;
    }

    GW::HeroPartyMember* GetPartyHeroByID(const HeroID hero_id, size_t* out_hero_index)
    {
        GW::PartyInfo* party_info = GW::PartyMgr::GetPartyInfo();
        if (!party_info) return nullptr;
        GW::HeroPartyMemberArray& heroes = party_info->heroes;
        if (!heroes.valid()) return nullptr;
        const GW::AgentLiving* me = GW::Agents::GetControlledCharacter();
        if (!me) return nullptr;
        const uint32_t my_id = me->login_number;
        for (size_t i = 0; i < heroes.size(); i++) {
            if (heroes[i].owner_player_id == my_id && heroes[i].hero_id == hero_id) {
                if (out_hero_index) *out_hero_index = i + 1;
                return &heroes[i];
            }
        }
        return nullptr;
    }

    void LoadPcons(const Build& build)
    {
        if (build.pcons.empty()) return;
        PconsWindow* pcw = &PconsWindow::Instance();
        std::vector<Pcon*> loaded, not_visible;
        for (auto* pcon : pcw->pcons) {
            const bool enable = build.pcons.contains(pcon->ini);
            if (enable) {
                if (!pcon->IsVisible()) {
                    not_visible.push_back(pcon);
                    continue;
                }
                pcon->SetEnabled(false);
                loaded.push_back(pcon);
            }
            pcon->SetEnabled(enable);
        }
        if (!loaded.empty()) {
            std::string s;
            size_t i = 0;
            for (const auto* p : loaded) {
                if (i++) s += ", ";
                s += p->abbrev;
            }
            Log::Flash("已加载 Pcons：%s", s.c_str());
        }
        if (!not_visible.empty()) {
            std::string s;
            size_t i = 0;
            for (const auto* p : not_visible) {
                if (i++) s += ", ";
                s += p->abbrev;
            }
            Log::Warning("Pcons 未加载（在 Pcons 窗口中不可见）：%s", s.c_str());
        }
    }

    bool PendingBuildLoad::Process()
    {
        if (build.IsPlayerBuild()) {
            const auto decoded = build.Decode();
            if (decoded && GW::SkillbarMgr::LoadSkillTemplate(*decoded)) {
                LoadPcons(build);
                Log::Flash("<quote>已加载Build：%s", build.GetChatBuildCode().c_str());
            }
            return true;
        }

        // 英雄Build：异步 — 添加英雄，然后等待它出现在队伍中。
        if (!started) started = TIMER_INIT();
        if (TIMER_DIFF(started) > 1000) return true; // 超时

        switch (stage) {
            case AddHero:
                if (!ToolboxUtils::IsHeroUnlocked(build.hero_id)) return true;
                if (!GW::PartyMgr::AddHero(build.hero_id)) {
                    Log::Warning("添加英雄 %d 失败", build.hero_id);
                    return true;
                }
                stage = WaitForHero;
                [[fallthrough]];
            case WaitForHero: {
                const GW::HeroPartyMember* hero = GetPartyHeroByID(build.hero_id, &party_hero_index);
                if (!hero) break;
                const GW::HeroFlag* flag = GetHeroFlagInfo(build.hero_id);
                if (!flag) break;
                if (!build.code.empty()) GW::SkillbarMgr::LoadSkillTemplate(hero->agent_id, build.code.c_str());
                for (uint32_t k = 0; k < 8; k++)
                    GW::PartyMgr::SetHeroSkillDisabled(hero->agent_id, k, ((build.disabled_skills >> k) & 1) != 0);
                GW::UI::SendUIMessage(build.show_panel ? GW::UI::UIMessage::kShowHeroPanel : GW::UI::UIMessage::kHideHeroPanel, reinterpret_cast<void*>(static_cast<uintptr_t>(build.hero_id)));
                const auto behavior = static_cast<GW::HeroBehavior>(build.behavior);
                if (behavior <= GW::HeroBehavior::AvoidCombat && flag->hero_behavior != behavior) GW::PartyMgr::SetHeroBehavior(hero->agent_id, behavior);
                stage = Finished;
                [[fallthrough]];
            }
            case Finished:
                return true;
        }
        return false;
    }

    // ----------------------------------------------------------------

    // 英雄 ID 顺序与 GW 客户端英雄面板相同。
    // Razah 出现在幻术师之后，因为大多数没有雇佣兵的玩家将其设为幻术师。
    constexpr std::array HeroIndexToID = {
        HeroID::NoHero,
        HeroID::Goren,
        HeroID::Koss,
        HeroID::Jora,
        HeroID::AcolyteJin,
        HeroID::MargridTheSly,
        HeroID::PyreFierceshot,
        HeroID::Tahlkora,
        HeroID::Dunkoro,
        HeroID::Ogden,
        HeroID::MasterOfWhispers,
        HeroID::Olias,
        HeroID::Livia,
        HeroID::Norgu,
        HeroID::Razah,
        HeroID::Gwen,
        HeroID::AcolyteSousuke,
        HeroID::ZhedShadowhoof,
        HeroID::Vekk,
        HeroID::Zenmai,
        HeroID::Anton,
        HeroID::Miku,
        HeroID::Xandra,
        HeroID::ZeiRi,
        HeroID::GeneralMorgahn,
        HeroID::KeiranThackeray,
        HeroID::Hayda,
        HeroID::Melonni,
        HeroID::MOX,
        HeroID::Kahmu,
        HeroID::Merc1,
        HeroID::Merc2,
        HeroID::Merc3,
        HeroID::Merc4,
        HeroID::Merc5,
        HeroID::Merc6,
        HeroID::Merc7,
        HeroID::Merc8,
        HeroID::Devona,
        HeroID::GhostOfAlthea
    };

    // 按名称排序的英雄 ID；每帧重新排序，直到所有名称解码完毕。
    const std::vector<HeroID>& SortedHeroIDs()
    {
        static std::vector<HeroID> sorted;
        static bool is_sorted = false;
        if (!is_sorted) {
            sorted.clear();
            for (const auto id : HeroIndexToID) {
                if (id != HeroID::NoHero) sorted.push_back(id);
            }
            bool all_decoded = true;
            for (const auto id : sorted) {
                if (Resources::GetHeroName(id)->string().empty()) {
                    all_decoded = false;
                    break;
                }
            }
            std::ranges::sort(sorted, [](const HeroID a, const HeroID b) {
                return _stricmp(Resources::GetHeroName(a)->string().c_str(), Resources::GetHeroName(b)->string().c_str()) < 0;
            });
            is_sorted = all_decoded;
        }
        return sorted;
    }

    void DefaultView(const Build& build)
    {
        GW::GameThread::Enqueue([code = build.code, name = build.name] {
            GW::UI::ChatTemplate t{};
            auto code_ws = TextUtils::StringToWString(code);
            t.code.m_buffer = code_ws.data();
            t.code.m_size = t.code.m_capacity = code_ws.size() + 1;
            auto name_ws = TextUtils::StringToWString(name);
            t.name = name_ws.data();
            GW::UI::SendUIMessage(GW::UI::UIMessage::kOpenTemplate, &t);
        });
    }

} // namespace

// ============================================================
// Build
// ============================================================

Build::Build(std::string_view n, std::string_view c, GW::Constants::HeroID hero_id_, int show_panel_, uint32_t behavior_, uint8_t disabled_skills_)
    : name(n), code(c), hero_id(hero_id_), behavior(behavior_), show_panel(show_panel_ != 0), disabled_skills(disabled_skills_)
{}

Build::~Build() {
    if (pending_reroll_build == this) 
        pending_reroll_build = 0;
}

const std::string& Build::GetFallbackBuildName()
{
    if (fallback_src_code_ != code
        || (fallback_elite_skill_ != GW::Constants::SkillID::No_Skill
            && Resources::GetSkillName(fallback_elite_skill_)->string() != fallback_name_)) {
        fallback_src_code_ = code;
        fallback_name_.clear();
        fallback_elite_skill_ = GW::Constants::SkillID::No_Skill;
        if (const auto decoded = Decode()) {
            for (const auto skill_id : decoded->skills) {
                if (skill_id == GW::Constants::SkillID::No_Skill) continue;
                const auto* skill = GW::SkillbarMgr::GetSkillConstantData(skill_id);
                if (!skill || !skill->IsElite()) continue;
                fallback_elite_skill_ = skill_id;
                fallback_name_ = Resources::GetSkillName(skill_id)->string();
                break;
            }
        }
    }
    return fallback_name_;
}

GW::SkillbarMgr::SkillTemplate* Build::Decode()
{
    if (!decode_attempted_) {
        decode_attempted_ = true;
        GW::SkillbarMgr::DecodeSkillTemplate(skill_template_, code.c_str());
    }
    return IsDecoded() ? &skill_template_ : nullptr;
}

bool Build::IsDecoded() const
{
    return decode_attempted_ && !(skill_template_.primary == GW::Constants::Profession::None && skill_template_.secondary == GW::Constants::Profession::None);
}

void Build::ResetDecodeCache()
{
    decode_attempted_ = false;
    skill_template_.primary = skill_template_.secondary = GW::Constants::Profession::None;
}
const GW::Constants::SkillID* Build::Skills()
{
    return Decode() ? skill_template_.skills : nullptr;
}

void Build::View() const
{
    GW::GameThread::Enqueue([code = code, name = name] {
        GW::UI::ChatTemplate t = {0};

        auto code_ws = TextUtils::StringToWString(code);
        t.code.m_buffer = code_ws.data();
        t.code.m_size = t.code.m_capacity = code_ws.size() + 1;

        auto name_ws = TextUtils::StringToWString(name);
        t.name = name_ws.data();
        GW::UI::SendUIMessage(GW::UI::UIMessage::kOpenTemplate, &t);
    });
}
const std::string& Build::DisplayName()
{
    bool dirty = display_name_src_name_ != name
        || display_name_src_hero_id_ != hero_id;
    if (!dirty && hero_id != GW::Constants::HeroID::NoHero)
        dirty = Resources::GetHeroName(hero_id)->string() != display_name_src_hero_name_;
    if (!dirty && name.empty())
        dirty = GetFallbackBuildName() != display_name_src_fallback_;
    if (dirty) {
        display_name_src_name_ = name;
        display_name_src_hero_id_ = hero_id;
        auto gen_name = name;
        if (gen_name.empty()) {
            gen_name = GetFallbackBuildName();
            display_name_src_fallback_ = gen_name;
        }
        if (hero_id != GW::Constants::HeroID::NoHero) {
            display_name_src_hero_name_ = Resources::GetHeroName(hero_id)->string();
            if (gen_name.empty())
                gen_name = display_name_src_hero_name_;
            else
                gen_name = std::format("{} ({})", name, display_name_src_hero_name_);
        }
        display_name_ = std::move(gen_name);
    }
    return display_name_;
}

std::string Build::GetChatBuildCode() {
    constexpr size_t kMaxLen = 120;
    const auto gen = DisplayName();

    std::string msg;
    if (code.empty()) {
        msg = gen.substr(0, kMaxLen);
    }
    else {
        // "[{gen};{code}]" — overhead is 3 chars: '[', ';', ']'
        constexpr size_t kOverhead = 3;
        const size_t nameMax = kMaxLen > code.size() + kOverhead ? kMaxLen - code.size() - kOverhead : 0;
        msg = std::format("[{};{}]", gen.substr(0, nameMax), code);
    }
    return msg;
}

void Build::Send()
{
    EnqueueSend(GetChatBuildCode());
}

void Build::Copy()
{
    const auto msg = GetChatBuildCode();
    if (msg.empty()) return;
    ImGui::SetClipboardText(msg.c_str());
    Log::Flash("Build代码已复制到剪贴板");
}

void Build::EnqueueSend(std::string msg)
{
    send_queue.push(TextUtils::StringToWString(msg));
}

void Build::RerollAndLoad(const wchar_t* character_name)
{
    if (!character_name || code.empty()) return;
    if (RerollWindow::Instance().Reroll(character_name, true, true)) {
        pending_reroll_character = character_name;
        pending_reroll_timer = TIMER_INIT();
        pending_reroll_build = this;
    }
}


void Build::Load() const
{
    if (code.empty() && hero_id == GW::Constants::HeroID::NoHero) return;
    pending_build_loads.push_back({*this});
}

void Build::Update()
{
    const auto instance_type = GW::Map::GetInstanceType();
    if (instance_type == GW::Constants::InstanceType::Loading) {
        pending_build_loads.clear();
        while (!send_queue.empty())
            send_queue.pop();
        kickall_timer = 0;
        return;
    }

    if (pending_reroll_build && TIMER_DIFF(pending_reroll_timer) > 10000) 
        pending_reroll_build = 0;

    if (pending_reroll_build && !RerollWindow::IsRerolling()) {
        if (pending_reroll_character != GW::AccountMgr::GetCurrentPlayerName()) {
            pending_reroll_build = 0;
            return;
        }
        pending_reroll_build->Load();
        pending_reroll_build = 0;
    }

    if (!send_queue.empty() && TIMER_DIFF(send_timer) > 600) {
        if (GW::Agents::GetControlledCharacter()) {
            GW::Chat::SendChat('#', send_queue.front().c_str());
            send_queue.pop();
            send_timer = TIMER_INIT();
        }
    }

    if (kickall_timer) {
        if (TIMER_DIFF(kickall_timer) > 500 || instance_type != GW::Constants::InstanceType::Outpost || !GetPlayerHeroCount()) {
            kickall_timer = 0;
        }
        return;
    }

    for (size_t i = 0; i < pending_build_loads.size(); i++) {
        auto& pending = pending_build_loads[i];
        if (!pending.build.IsPlayerBuild() && instance_type != GW::Constants::InstanceType::Outpost) {
            pending_build_loads.clear();
            break;
        }
        if (pending.Process()) {
            pending_build_loads.erase(pending_build_loads.begin() + i);
            break;
        }
    }
}

// ============================================================
// TeamBuild
// ============================================================

uint32_t TeamBuild::s_cur_ui_id = 0;

void TeamBuild::RefreshTitles()
{
    edit_winname_src_ = name;
    edit_winname_ = std::format("{}###teambuild_{}", name, ui_id);
    detached_winname_src_ = name;
    detached_winname_ = std::format("{}###detached_{}", name, ui_id);
}

TeamBuild::TeamBuild()
{
    RefreshTitles();
}

TeamBuild::TeamBuild(std::string_view n, std::string_view id) : name(n), ui_id(id.empty() ? std::to_string(++s_cur_ui_id) : std::string(id))
{
    RefreshTitles();
}

TeamBuild::TeamBuild(const TeamBuild& other)
    : edit_open(other.edit_open), focus_next_frame(other.focus_next_frame), mode(other.mode), show_numbers(other.show_numbers), has_hero_slots(other.has_hero_slots), name(other.name), group(other.group), ui_id(std::to_string(++s_cur_ui_id)),
      builds(other.builds)
{
    RefreshTitles();
}

TeamBuild& TeamBuild::operator=(const TeamBuild& other)
{
    if (this != &other) {
        edit_open = other.edit_open;
        focus_next_frame = other.focus_next_frame;
        mode = other.mode;
        show_numbers = other.show_numbers;
        has_hero_slots = other.has_hero_slots;
        name = other.name;
        group = other.group;
        ui_id = std::to_string(++s_cur_ui_id);
        builds = other.builds;
    }
    RefreshTitles();
    return *this;
}

void TeamBuild::SetSkillToggleSprite(IDirect3DTexture9** sprite)
{
    skill_toggle_sprite = sprite;
}

const std::wstring& TeamBuild::GetEncoded() const
{
    if (!encoded_cache_.has_value()) encoded_cache_ = TeamBuildEncoder::TeamBuildToEncoded(*this);
    return *encoded_cache_;
}

void TeamBuild::ResetEncodedCache() const
{
    encoded_cache_.reset();
}

void TeamBuild::Send(bool one_by_one)
{
    if (!name.empty()) Build::EnqueueSend(name);
    if (one_by_one) {
        for (auto& build : builds)
            build.Send();
    }
    else {
        const auto& encoded = GetEncoded();
        if (!encoded.empty()) send_queue.push(std::format(L"[TB;{}]", encoded));
    }
}

void TeamBuild::Copy() const
{
    const auto& encoded = GetEncoded();
    if (encoded.empty()) return;
    const auto msg = TextUtils::WStringToString(std::format(L"[TB;{}]", encoded));
    ImGui::SetClipboardText(msg.c_str());
    Log::Flash("团队Build代码已复制到剪贴板");
}
TeamBuild TeamBuild::Duplicate()
{
    TeamBuild copy = *this;
    copy.name += "（副本）";
    return std::move(copy);
}

void TeamBuild::DrawTooltip()
{
    for (auto& build : builds) {
        const bool has_hero = build.hero_id != GW::Constants::HeroID::NoHero;
        const bool has_name = !build.name.empty();
        const auto decoded = build.Decode();

        if (has_hero_slots) {
            if (!has_hero && !has_name && !decoded) continue;
        }
        else {
            if (!has_name && !decoded) continue;
        }

        ImGui::Spacing();

        if (has_hero_slots) {
            const auto hero_name = has_hero ? Resources::GetHeroName(build.hero_id)->string() : std::string("玩家");
            const auto display_name = has_name ? build.name : build.GetFallbackBuildName();
            const auto full_name = std::format("{} ({})", display_name, hero_name);
            ImGui::TextUnformatted(full_name.c_str());
        }
        else {
            ImGui::TextUnformatted(build.name.c_str());
        }

        GW::SkillbarMgr::SkillTemplate st{};
        if (decoded) {
            GuiUtils::DrawSkillbar(decoded, false);
        }
        else {
            ImGui::TextColored({1.f, 0.3f, 0.3f, 1.f}, "未定义Build");
        }

        ImGui::Spacing();
    }
}

bool TeamBuild::ChatCodeTooLong() const
{
    const auto& encoded = GetEncoded();
    // [TB;<encoded>] = 4 + encoded.size() + 1 = encoded.size() + 5; must be < 120
    return !encoded.empty() && encoded.size() + 5 >= 120;
}

void TeamBuild::Load() const
{
    if (GW::Map::GetInstanceType() != GW::Constants::InstanceType::Outpost) {
        return;
    }
    if (has_hero_slots) {
        if (!GW::PartyMgr::KickAllHeroes()) {
            Log::Warning("踢出所有英雄失败");
            return;
        }
        kickall_timer = TIMER_INIT();
    }
    if (mode > 0) {
        GW::PartyMgr::SetHardMode(mode == 2) || (Log::Warning("设置困难模式失败"), true);
    }
    for (const auto& build : builds) {
        build.Load();
    }
}

// ------------------------------------------------------------
// 玩家Build布局（BuildsWindow 风格）
// ------------------------------------------------------------
void TeamBuild::DrawPlayerBuildsContent(bool& builds_modified, bool editable)
{
    const float font_scale = ImGui::FontScale();
    const auto row_height = ImGui::CalcTextSize(" ").y * 2.f;
    const auto icon_btn_size = ImVec2(row_height, row_height);
    const float spacing = 4.f * font_scale;
    const auto min_row_width = row_height * 15.f;
    // 可编辑时可见图标按钮数：聊天、加载/切换角色、编辑、下拉菜单 = 4
    // 只读时：聊天、加载/切换角色、下拉菜单 = 3
    const size_t icon_btns = editable ? 4 : 3;

    const auto* me = GW::Agents::GetControlledCharacter();
    const auto player_profession = me ? static_cast<GW::Constants::Profession>(me->primary) : GW::Constants::Profession::None;

    bool tmp = builds_modified;
    builds_modified = false;

    for (size_t j = 0; j < builds.size(); j++) {
        Build& build = builds[j];
        ImGui::PushID(static_cast<int>(j));

        const bool editing = editable && editing_build_idx_ == static_cast<int>(j);

        // ---- 行：编号 + 名称（编辑模式下可编辑）+ 图标按钮 ----
        ImGui::Text("#%zu", j + 1);
        ImGui::SameLine(0);
        ImGui::Indent();

        const auto btns_start = min_row_width + ImGui::GetIndent() - (icon_btns * (icon_btn_size.x + spacing));
        const float name_width = btns_start - spacing - ImGui::GetIndent();

        if (editing) {
            ImGui::PushItemWidth(name_width);
            ImGui::InputText("###name", build.name, 128);
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Build名称/标签");
            }

            ImGui::PushItemWidth(name_width);
            if (ImGui::InputText("###code", build.code, 128)) {
                build.ResetDecodeCache();
                ResetEncodedCache();
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip([&build]() {
                    ImGui::TextUnformatted("Build代码");
                    if (const auto decoded = build.Decode()) {
                        ImGui::Spacing();
                        GuiUtils::DrawSkillbar(build.Decode());
                    }
                });
            }
        }
        else {
            ImGui::PushItemWidth(name_width);
            const auto& disp = !build.name.empty() ? build.name : build.GetFallbackBuildName();
            ImGui::TextUnformatted(disp.c_str());
            if (ImGui::IsItemHovered() && !build.code.empty()) {
                ImGui::SetTooltip([&build]() {
                    GuiUtils::DrawSkillbar(build.Decode(), false);
                });
            }
            ImGui::PopItemWidth();

            if (const auto decoded = build.Decode()) {
                GuiUtils::DrawSkillbar(decoded, false);
            }
            else {
                const auto width = (row_height * 8) + row_height / 2.f;
                ImGui::Dummy({width, row_height});
            }
        }


        // --- 发送到聊天 ---
        ImGui::SameLine(btns_start);
        if (GuiUtils::IconButton("##chat", GuiUtils::GwButtonIcon::ChatIcon, icon_btn_size)) build.Send();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("发送到队伍聊天");

        // --- 加载 / 切换角色 ---
        ImGui::SameLine(0, spacing);
        {
            const auto skillbar = build.Decode();
            bool can_load = false;
            const wchar_t* reroll_to = 0;
            GW::Constants::Profession build_prof = GW::Constants::Profession::None;
            if (!skillbar) {
                ImGui::Dummy(icon_btn_size);
            }
            else {
                if (build.IsPlayerBuild()) {
                    build_prof = skillbar->primary;
                    can_load = player_profession != GW::Constants::Profession::None && build_prof == player_profession;
                    if (!can_load && build_prof != GW::Constants::Profession::None) reroll_to = RerollWindow::FindAvailableCharForProfession(build_prof);
                }
                else {
                    can_load = ToolboxUtils::IsHeroUnlocked(build.hero_id);
                }
                if (can_load) {
                    if (GuiUtils::IconButton("##load", GuiUtils::GwButtonIcon::LoadFromTemplate, icon_btn_size)) build.Load();
                }
                else if (reroll_to) {
                    if (GuiUtils::IconButtonConfirm("##reroll", GuiUtils::GwButtonIcon::ManageTemplates, icon_btn_size)) build.RerollAndLoad(reroll_to);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(std::format("切换角色至 {} 并加载Build", TextUtils::WStringToString(reroll_to)).c_str());
                }
                else {
                    ImGui::Dummy(icon_btn_size);
                }
            }
        }

        // --- 编辑开关 ---
        ImGui::SameLine(0, spacing);
        if (editable) {
            if (editing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(ICON_FA_EDIT "##edit", icon_btn_size)) editing_build_idx_ = editing ? -1 : static_cast<int>(j);
            if (editing) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(editing ? "停止编辑" : "编辑Build");
        }

        // --- 下拉菜单（查看、复制、删除） ---
        ImGui::SameLine(0, spacing);
        if (ImGui::Button(ICON_FA_ELLIPSIS_V, icon_btn_size)) ImGui::OpenPopup("##build_menu");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("更多选项");

        if (ImGui::BeginPopup("##build_menu")) {
            if (ImGui::MenuItem(ICON_FA_EYE "  查看Build")) build.View();
            if (ImGui::MenuItem(ICON_FA_COPY "  复制Build代码")) build.Copy();
            if (editable) {
                ImGui::Separator();
                bool delete_confirmed = false;
                if (ImGui::ConfirmButton(ICON_FA_TRASH "  删除Build", &delete_confirmed, "删除Build\n\n确定吗？\n此操作无法撤销。")) {
                    if (editing_build_idx_ == static_cast<int>(j))
                        editing_build_idx_ = -1;
                    else if (editing_build_idx_ > static_cast<int>(j))
                        editing_build_idx_--;
                    builds.erase(builds.begin() + static_cast<ptrdiff_t>(j));
                    ResetEncodedCache();
                    builds_modified = true;
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::EndPopup();
        }

        // ---- 展开的编辑面板（行下方） ----
        if (editing) {
            ImGui::TextUnformatted("Pcons：");
            ImGui::ShowHelp("加载此Build时启用或禁用 Pcons");
            const auto& pcons = PconsWindow::Instance().pcons;
            const float skill_h = ImGui::CalcTextSize(" ").y * 2.f;
            ImGui::StartSpacedElements(skill_h + ImGui::GetStyle().ItemSpacing.x);
            size_t i = 0;
            for (const auto pcon : pcons) {
                ImGui::PushID(i++);
                const bool active = build.pcons.contains(pcon->ini);
                ImGui::NextSpacedElement();
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, build_edit_pcon_enabled_color);
                if (ImGui::IconButton("", *pcon->GetTexture(), {skill_h, skill_h})) {
                    if (!active)
                        build.pcons.emplace(pcon->ini);
                    else
                        build.pcons.erase(pcon->ini);
                }
                if (active) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(pcon->chat.c_str());
                ImGui::PopID();
            }
        }
        ImGui::Unindent();
        ImGui::PopID();
        if (builds_modified) break;
    }

    builds_modified |= tmp;

    ImGui::Spacing();

    if (editable) {
        ImGui::Checkbox("显示编号", &show_numbers);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("发送到聊天时在Build名称前添加索引");

        ImGui::SameLine();
        const float add_btn_width = 140.f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - add_btn_width);
        if (ImGui::Button("添加Build", ImVec2(add_btn_width, 0))) {
            builds.emplace_back("", "");
            ResetEncodedCache();
            editing_build_idx_ = static_cast<int>(builds.size()) - 1;
            builds_modified = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("添加另一行Build");
    }
}
// ------------------------------------------------------------
// 英雄Build布局（HeroBuildsWindow 风格）
// ------------------------------------------------------------
void TeamBuild::DrawHeroBuildsContent(bool& builds_modified, bool editable)
{
    const float font_scale = ImGui::FontScale();
    const auto row_height = ImGui::CalcTextSize(" ").y * 2.f;
    const auto icon_btn_size = ImVec2(row_height, row_height);
    const float spacing = 4.f * font_scale;
    const auto min_row_width = row_height * 15.f;
    // 可编辑时可见图标按钮数：发送、加载、编辑、下拉菜单 = 4
    // 只读时：发送、加载、下拉菜单 = 3
    const size_t icon_btns = editable ? 4 : 3;

    const auto* me = GW::Agents::GetControlledCharacter();
    const auto player_profession = me ? static_cast<GW::Constants::Profession>(me->primary) : GW::Constants::Profession::None;

    bool tmp = builds_modified;
    builds_modified = false;

    size_t player_idx = builds.size();
    for (size_t j = 0; j < builds.size(); ++j) {
        if (builds[j].hero_id == GW::Constants::HeroID::NoHero) {
            player_idx = j;
            break;
        }
    }

    uint32_t hero_count = 1;

    for (size_t j = 0; j < builds.size(); j++) {
        Build& build = builds[j];
        ImGui::PushID(static_cast<int>(j));

        const bool editing = editable && editing_build_idx_ == static_cast<int>(j);
        const bool is_player = j == player_idx;

        // ---- 行标签 ----
        if (is_player)
            ImGui::Text("P");
        else
            ImGui::Text("#%u", hero_count++);

        ImGui::SameLine(0);
        ImGui::Indent();

        const auto btns_start = min_row_width + ImGui::GetIndent() - (icon_btns * (icon_btn_size.x + spacing));
        const float name_width = btns_start - spacing - ImGui::GetIndent();

        // ---- 名称 + 代码（编辑模式下可编辑） ----
        if (editing) {
            ImGui::PushItemWidth(name_width);
            ImGui::InputText("###name", build.name, 128);
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Build名称/标签");

            ImGui::PushItemWidth(name_width);
            if (ImGui::InputText("###code", build.code, 128)) {
                build.ResetDecodeCache();
                ResetEncodedCache();
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip([&build]() {
                    ImGui::TextUnformatted("Build代码");
                    if (const auto decoded = build.Decode()) {
                        ImGui::Spacing();
                        GuiUtils::DrawSkillbar(decoded);
                    }
                });
            }
        }
        else {
            ImGui::TextUnformatted(build.DisplayName().c_str());
            if (ImGui::IsItemHovered() && !build.code.empty()) {
                ImGui::SetTooltip([&build]() {
                    GuiUtils::DrawSkillbar(build.Decode(), false);
                });
            }

            if (const auto decoded = build.Decode()) {
                GuiUtils::DrawSkillbar(decoded, false);
            }
            else {
                const float width = (row_height * 8) + row_height / 2.f;
                ImGui::Dummy({width, row_height});
            }
        }

        // --- 发送到聊天 ---
        ImGui::SameLine(btns_start);
        if (GuiUtils::IconButton("##chat", GuiUtils::GwButtonIcon::ChatIcon, icon_btn_size)) build.Send();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("发送到队伍聊天");

        // --- 加载 / 切换角色 ---
        ImGui::SameLine(0, spacing);
        {
            const auto skillbar = build.Decode();
            bool can_load = false;
            const wchar_t* reroll_to = nullptr;
            GW::Constants::Profession build_prof = GW::Constants::Profession::None;

            if (!skillbar) {
                ImGui::Dummy(icon_btn_size);
            }
            else if (is_player) {
                build_prof = skillbar->primary;
                can_load = player_profession != GW::Constants::Profession::None && build_prof == player_profession;
                if (!can_load && build_prof != GW::Constants::Profession::None) reroll_to = RerollWindow::FindAvailableCharForProfession(build_prof);

                if (can_load) {
                    if (GuiUtils::IconButton("##load", GuiUtils::GwButtonIcon::LoadFromTemplate, icon_btn_size)) build.Load();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("加载Build");
                }
                else if (reroll_to) {
                    if (GuiUtils::IconButtonConfirm("##reroll", GuiUtils::GwButtonIcon::ManageTemplates, icon_btn_size)) build.RerollAndLoad(reroll_to);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(std::format("切换角色至 {} 并加载Build", TextUtils::WStringToString(reroll_to)).c_str());
                }
                else {
                    ImGui::Dummy(icon_btn_size);
                }
            }
            else {
                can_load = ToolboxUtils::IsHeroUnlocked(build.hero_id);
                if (can_load) {
                    const auto* map_info = GW::Map::GetMapInfo();
                    const bool party_full = map_info && GW::PartyMgr::GetPartySize() >= map_info->max_party_size;
                    const bool hero_in_party = GetHeroFlagInfo(static_cast<uint32_t>(build.hero_id)) != nullptr;
                    const bool no_space = party_full && !hero_in_party;
                    if (no_space) ImGui::BeginDisabled();
                    if (GuiUtils::IconButton("加载##load", GuiUtils::GwButtonIcon::LoadFromTemplate, icon_btn_size)) build.Load();
                    if (no_space) ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip(no_space ? "队伍中无空位加载此英雄" : "在英雄上加载Build");
                }
                else {
                    ImGui::Dummy(icon_btn_size);
                }
            }
        }

        // --- 编辑开关 ---
        ImGui::SameLine(0, spacing);
        if (editable) {
            if (editing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(ICON_FA_EDIT "##edit", icon_btn_size)) editing_build_idx_ = editing ? -1 : static_cast<int>(j);
            if (editing) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(editing ? "停止编辑" : "编辑Build");
        }

        // --- 下拉菜单（查看、复制、移动、删除） ---
        ImGui::SameLine(0, spacing);
        if (ImGui::Button(ICON_FA_ELLIPSIS_V, icon_btn_size)) ImGui::OpenPopup("##build_menu");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("更多选项");

        if (ImGui::BeginPopup("##build_menu")) {
            if (ImGui::MenuItem(ICON_FA_EYE "  查看Build")) build.View();
            if (ImGui::MenuItem(ICON_FA_COPY "  复制Build代码")) build.Copy();
            if (editable) {

                if (!is_player) {
                    const bool prev_is_player = j > 0 && j - 1 == player_idx;
                    const bool next_is_player = j + 1 < builds.size() && j + 1 == player_idx;
                    const bool can_move_up = j > 0 && !prev_is_player;
                    const bool can_move_down = j + 1 < builds.size() && !next_is_player;
                    ImGui::Separator();
                    if (!can_move_up) ImGui::BeginDisabled();
                    if (ImGui::MenuItem(ICON_FA_ARROW_UP "  上移")) {
                        std::swap(builds[j - 1], builds[j]);
                        ResetEncodedCache();
                        builds_modified = true;
                        ImGui::EndPopup();
                        ImGui::Unindent();
                        ImGui::PopID();
                        break;
                    }
                    if (!can_move_up) ImGui::EndDisabled();
                    if (!can_move_down) ImGui::BeginDisabled();
                    if (ImGui::MenuItem(ICON_FA_ARROW_DOWN "  下移")) {
                        std::swap(builds[j], builds[j + 1]);
                        ResetEncodedCache();
                        builds_modified = true;
                        ImGui::EndPopup();
                        ImGui::Unindent();
                        ImGui::PopID();
                        break;
                    }
                    if (!can_move_down) ImGui::EndDisabled();
                }

                ImGui::Separator();
                bool delete_confirmed = false;
                if (ImGui::ConfirmButton(ICON_FA_TRASH "  删除Build", &delete_confirmed, "删除Build\n\n确定吗？\n此操作无法撤销。")) {
                    if (editing_build_idx_ == static_cast<int>(j))
                        editing_build_idx_ = -1;
                    else if (editing_build_idx_ > static_cast<int>(j))
                        editing_build_idx_--;
                    builds.erase(builds.begin() + static_cast<ptrdiff_t>(j));
                    ResetEncodedCache();
                    builds_modified = true;
                    ImGui::EndPopup();
                    ImGui::Unindent();
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::EndPopup();
        }

        // ---- 展开的编辑区域 ----
        if (editing) {
            if (!is_player) {
                const auto& sorted_heroes = SortedHeroIDs();
                const auto hero_it = std::ranges::find(sorted_heroes, build.hero_id);
                int combo_idx = hero_it != sorted_heroes.end() ? static_cast<int>(std::distance(sorted_heroes.begin(), hero_it)) : -1;
                ImGui::PushItemWidth(name_width);
                if (ImGui::MyCombo(
                        "###heroid", "选择英雄", &combo_idx,
                        [](void*, const int idx, const char** out_text) -> bool {
                            const auto& heroes = SortedHeroIDs();
                            if (idx < 0 || idx >= static_cast<int>(heroes.size())) return false;
                            *out_text = Resources::GetHeroName(heroes[idx])->string().c_str();
                            return true;
                        },
                        nullptr, static_cast<int>(sorted_heroes.size())
                    )) {
                    build.hero_id = (combo_idx >= 0 && combo_idx < static_cast<int>(sorted_heroes.size())) ? sorted_heroes[combo_idx] : HeroID::NoHero;
                    ResetEncodedCache();
                }
                ImGui::PopItemWidth();

                ImGui::SameLine(0, spacing);
                const auto* panel_icon = reinterpret_cast<const char*>(build.show_panel ? ICON_FA_EYE : ICON_FA_EYE_SLASH);
                if (ImGui::Button(panel_icon, icon_btn_size)) {
                    build.show_panel = !build.show_panel;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(build.show_panel ? "英雄面板：显示" : "英雄面板：隐藏");

                ImGui::SameLine(0, spacing);
                const char* behavior_icon = reinterpret_cast<const char*>(ICON_FA_SHIELD_ALT);
                const char* behavior_tooltip = "英雄行为：防御";
                switch (build.behavior) {
                    case 0:
                        behavior_icon = reinterpret_cast<const char*>(ICON_FA_FIST_RAISED);
                        behavior_tooltip = "英雄行为：攻击";
                        break;
                    case 2:
                        behavior_icon = reinterpret_cast<const char*>(ICON_FA_DOVE);
                        behavior_tooltip = "英雄行为：回避战斗";
                        break;
                }
                if (ImGui::Button(behavior_icon, icon_btn_size)) {
                    if (++build.behavior > 2) build.behavior = 0;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(behavior_tooltip);

                ImGui::SameLine(0, spacing);
                int enabled_count = 8;
                for (int k = 0; k < 8; k++)
                    if ((build.disabled_skills >> k) & 1) enabled_count--;
                char skills_label[8];
                snprintf(skills_label, sizeof(skills_label), "%d/8", enabled_count);
                if (ImGui::Button(skills_label, icon_btn_size)) ImGui::OpenPopup("技能开关");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("切换加载时禁用哪些技能");

                if (ImGui::BeginPopup("技能开关")) {
                    const auto decoded = build.Decode();
                    constexpr float skill_px = 48.0f;
                    for (int k = 0; k < 8; k++) {
                        if (k > 0) ImGui::SameLine(0, 0);
                        const bool is_disabled = (build.disabled_skills >> k) & 1;
                        const auto skill_id = decoded ? decoded->skills[k] : GW::Constants::SkillID::No_Skill;
                        auto* skill_tex = *Resources::GetSkillImage(skill_id);
                        ImGui::PushID(k);
                        const ImVec2 pos = ImGui::GetCursorScreenPos();
                        if (ImGui::InvisibleButton("##skill_slot", ImVec2(skill_px, skill_px))) {
                            if (is_disabled)
                                build.disabled_skills &= static_cast<uint8_t>(~(1u << k));
                            else
                                build.disabled_skills |= static_cast<uint8_t>(1u << k);
                            builds_modified = true;
                        }
                        const ImVec2 p_max(pos.x + skill_px, pos.y + skill_px);
                        auto* dl = ImGui::GetWindowDrawList();
                        if (skill_id != GW::Constants::SkillID::No_Skill && skill_tex)
                            dl->AddImage(reinterpret_cast<ImTextureID>(skill_tex), pos, p_max);
                        else
                            dl->AddRectFilled(pos, p_max, IM_COL32(50, 50, 50, 255));
                        if (is_disabled) {
                            dl->AddRectFilled(pos, p_max, IM_COL32(0, 0, 0, 140));
                            if (skill_toggle_sprite && *skill_toggle_sprite) dl->AddImage(reinterpret_cast<ImTextureID>(*skill_toggle_sprite), pos, p_max, ImVec2(0.f, 0.5f), ImVec2(0.5f, 1.f));
                        }
                        if (ImGui::IsItemHovered()) dl->AddRect(pos, p_max, IM_COL32(255, 255, 255, 200), 0.f, 0, 2.f);
                        ImGui::PopID();
                    }
                    ImGui::EndPopup();
                }
            }

            if (is_player) {
                ImGui::TextUnformatted("Pcons：");
                ImGui::ShowHelp("加载此Build时启用或禁用 Pcons");
                const auto& pcons = PconsWindow::Instance().pcons;
                const float skill_h = ImGui::CalcTextSize(" ").y * 2.f;
                ImGui::StartSpacedElements(skill_h + ImGui::GetStyle().ItemSpacing.x);
                size_t i = 0;
                for (const auto pcon : pcons) {
                    ImGui::PushID(i++);
                    const bool active = build.pcons.contains(pcon->ini);
                    ImGui::NextSpacedElement();
                    if (active) ImGui::PushStyleColor(ImGuiCol_Button, build_edit_pcon_enabled_color);
                    if (ImGui::IconButton("", *pcon->GetTexture(), {skill_h, skill_h})) {
                        if (!active)
                            build.pcons.emplace(pcon->ini);
                        else
                            build.pcons.erase(pcon->ini);
                    }
                    if (active) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(pcon->chat.c_str());
                    ImGui::PopID();
                }
            }
        }

        ImGui::Unindent();
        if (editing) ImGui::Spacing();
        ImGui::PopID();
        if (builds_modified) break;
    }

    builds_modified |= tmp;

    ImGui::Spacing();

    if (editable) {
        const bool has_player_slot = player_idx < builds.size();
        if (!has_player_slot && builds.size() < 8) {
            if (ImGui::Button("添加玩家槽位")) {
                builds.insert(builds.begin(), Build("", "", HeroID::NoHero, 0, 1));
                ResetEncodedCache();
                builds_modified = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("添加玩家Build槽位");
            ImGui::SameLine();
        }
        if (builds.size() < 8) {
            if (ImGui::Button("添加英雄槽位")) {
                builds.push_back(Build("", "", HeroID::NoHero, 0, 1));
                ResetEncodedCache();
                builds_modified = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("添加英雄Build槽位");
        }
    }
}

// ------------------------------------------------------------
// DrawEditWindow
// ------------------------------------------------------------

bool TeamBuild::DrawEditWindow(size_t index, std::vector<TeamBuild>& all_builds, bool& builds_modified)
{
    if (edit_winname_src_ != name) {
        edit_winname_src_ = name;
        edit_winname_ = std::format("{}###teambuild_{}", name, ui_id);
    }
    const auto& winname = edit_winname_;
    ImGui::SetNextWindowCenter(ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 0), ImGuiCond_FirstUseEver);
    if (focus_next_frame) {
        ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowCollapsed(false);
        focus_next_frame = false;
    }

    if (!ImGui::Begin(winname.c_str(), &edit_open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return true;
    }

    if (has_hero_slots) {
        ImGui::InputText("英雄Build名称", name, 128);
        ImGui::InputText("分组", group, 128);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("分配到一个分组。共享相同分组名称的Build会显示在一个可折叠标题下。");
        }
        DrawHeroBuildsContent(builds_modified);
    }
    else {
        ImGui::PushItemWidth(-120.f);
        ImGui::InputText("Build名称", name, 128);
        ImGui::PopItemWidth();
        DrawPlayerBuildsContent(builds_modified);
    }

    ImGui::Spacing();

    if (ImGui::Button("上移") && index > 0) {
        std::swap(all_builds[index - 1], all_builds[index]);
        builds_modified = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("在列表中上移此团队Build");

    ImGui::SameLine();
    if (ImGui::Button("下移") && index + 1 < all_builds.size()) {
        std::swap(all_builds[index], all_builds[index + 1]);
        builds_modified = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("在列表中下移此团队Build");

    ImGui::SameLine();
    if (ImGui::Button("复制")) {
        auto cpy = Duplicate();
        cpy.has_hero_slots = has_hero_slots;
        cpy.edit_open = true;
        edit_open = false;
        all_builds.push_back(std::move(cpy));
        builds_modified = true;
        ImGui::End();
        return false;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("复制团队Build");
    }
    bool deleted = false;
    if (ImGui::ConfirmButton("删除", &deleted, "删除团队Build？\n\n确定吗？\n此操作无法撤销。")) {
        all_builds.erase(all_builds.begin() + static_cast<ptrdiff_t>(index));
        builds_modified = true;
        ImGui::End();
        return false;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("删除此团队Build");

    if (has_hero_slots) {
        ImGui::SameLine();
        ImGui::PushItemWidth(110.f);
        constexpr const char* modes[] = {"不更改", "普通模式", "困难模式"};
        ImGui::Combo("模式", &mode, modes, 3);
        ImGui::PopItemWidth();

        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 40);
        if (ImGui::Button("关闭", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            edit_open = false;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("关闭此窗口");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const bool chat_code_too_long = ChatCodeTooLong();
    if (chat_code_too_long) ImGui::BeginDisabled();
    if (ImGui::Button("在聊天中发送团队Build代码")) {
        this->Send();
    }
    if (chat_code_too_long) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (chat_code_too_long) {
            ImGui::SetTooltip("团队Build代码太长，无法在聊天中发送");
        }
        else {
            ImGui::SetTooltip("将编码后的团队Build链接发送到队伍聊天。\n其他工具箱用户可直接点击链接，不会刷屏。");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("复制团队Build代码")) {
        this->Copy();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("将编码后的团队Build链接复制到剪贴板。\n可粘贴到任何地方分享你的团队Build。");
    }
    ImGui::SameLine();
    if (ImGui::ConfirmButton("在聊天中发送所有Build", &send_all_confirming_, "在聊天中发送所有Build\n\n这将在队伍聊天中将每个Build作为单独消息发送。\n确定吗？")) {
        this->Send(true);
        send_all_confirming_ = false;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("将每个Build作为独立的技能模板链接发送到队伍聊天。\n非工具箱用户也能逐一点击加载。");
    }

    ImGui::End();
    return true;
}

// ------------------------------------------------------------
// DrawDetachedWindow
// ------------------------------------------------------------
void TeamBuild::DrawDetachedWindow(std::vector<TeamBuild>& hero_builds, bool& builds_modified)
{
    if (!edit_open) return;
    if (detached_winname_src_ != name) {
        detached_winname_src_ = name;
        detached_winname_ = std::format("{}###detached_{}", name, ui_id);
    }
    const auto& winname = detached_winname_;
    ImGui::SetNextWindowCenter(ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (focus_next_frame) {
        ImGui::SetNextWindowCollapsed(false);
        ImGui::SetNextWindowFocus();
        focus_next_frame = false;
    }
    if (!ImGui::Begin(winname.c_str(), &edit_open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    if (has_hero_slots)
        DrawHeroBuildsContent(builds_modified, false);
    else
        DrawPlayerBuildsContent(builds_modified, false);

    // ---- 底部按钮（分离窗口专用） ----
    if (has_hero_slots) {
        if (ImGui::Button("全部加载")) Load();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("将所有Build加载到你的英雄上");
        ImGui::SameLine();
    }
    if (ImGui::Button("添加到我的Build")) {
        TeamBuild copy = *this;
        copy.edit_open = false;
        if (copy.has_hero_slots) {
            hero_builds.push_back(std::move(copy));
            builds_modified = true;
        }
        else {
            BuildsWindow::Instance().AddTeambuild(std::move(copy));
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(has_hero_slots ? "将此团队Build保存到你的英雄Build列表" : "将此团队Build保存到你的Build列表");

    ImGui::SameLine();
    if (ImGui::Button("关闭")) edit_open = false;

    ImGui::End();
}
