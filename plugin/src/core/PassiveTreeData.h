#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SkyrimPoE::Core
{
    enum class PassiveNodeKind : std::uint8_t
    {
        Normal = 0,
        Notable = 1,
        Keystone = 2,
        Mastery = 3,
        Socket = 4,
        ClassStart = 5,
        AscendancyStart = 6,
        Attribute = 7
    };

    struct PassiveAscendancyDef
    {
        std::string id;
        std::string contentId;
        std::string name;  // 표시명 (예: "Warden", "Juggernaut")
        std::uint8_t classId = 0;
        std::uint32_t startNodeId = 0;
        std::vector<PassiveAscendancyDef> replacements;
    };

    struct PassiveClassDef
    {
        std::uint8_t id = 0;
        std::string contentId;
        std::string name;
        std::uint32_t baseStr = 20;
        std::uint32_t baseDex = 20;
        std::uint32_t baseInt = 20;
        std::uint32_t startNodeId = 0;
        std::vector<std::uint32_t> startNeighbors;
        std::vector<PassiveAscendancyDef> ascendancies;
    };

    struct PassiveNodeText
    {
        std::string name;
        std::vector<std::string> stats;
    };

    struct PassiveAttributeOption : PassiveNodeText
    {
        std::uint32_t id = 0;
    };

    struct PassiveNodeVariant : PassiveNodeText
    {
        std::uint32_t sourceId = 0;
        std::string ascendancyId;
    };

    struct PassiveNodeDef
    {
        std::uint32_t id = 0;
        PassiveNodeKind kind = PassiveNodeKind::Normal;
        std::uint32_t groupId = 0;
        std::string name;
        std::string ascendancyId;  // 비어 있으면 일반 트리
        std::vector<std::uint32_t> adjacency;  // 규칙 그래프 인접 목록 (classStart·mastery 제외)
        std::vector<std::string> stats;        // \n 분할 정규화된 영문 스탯 줄
        bool isMultipleChoice = false;
        bool isMultipleChoiceOption = false;
        std::uint32_t choiceParentId = 0;
        std::uint32_t grantedPassivePoints = 0;
        std::int8_t grantsClassStart = -1;     // Scion Ascendant Path of the ... (0..6, 없으면 -1)
        std::vector<std::uint32_t> masteryEffectIds;
        bool deferred = false;
        std::vector<PassiveAttributeOption> attrOptions;
        std::vector<std::uint32_t> unlock;
        bool freeAlloc = false;
        std::unordered_map<std::string, PassiveNodeVariant> variants;
        bool jewelSocket = false;
        std::string alias;

        [[nodiscard]] const PassiveAttributeOption* FindAttributeOption(std::uint32_t optionId) const noexcept;
    };

    struct PassiveMasteryEffectDef
    {
        std::uint32_t id = 0;
        std::vector<std::string> stats;
    };

    // data/packs/tree_3_30.json 경량 로더(C++ 코어 전용: 좌표·원호·한글 필드는 스킵해 kDataLoaded 지연 최소화)
    class PassiveTreeData
    {
    public:
        bool LoadFromFile(const std::filesystem::path& path, std::string* errorOut = nullptr);
        bool LoadFromJsonString(std::string_view jsonText, std::string* errorOut = nullptr);

        [[nodiscard]] bool IsLoaded() const noexcept { return loaded_; }
        [[nodiscard]] std::uint32_t Generation() const noexcept { return generation_; }
        [[nodiscard]] const std::vector<PassiveClassDef>& Classes() const noexcept { return classes_; }
        [[nodiscard]] const PassiveClassDef* FindClass(std::uint8_t classId) const noexcept;
        [[nodiscard]] const PassiveClassDef* FindClassByName(std::string_view name) const noexcept;
        [[nodiscard]] const PassiveClassDef* FindClassByContentId(std::string_view id) const noexcept;
        [[nodiscard]] const PassiveAscendancyDef* FindAscendancy(std::string_view ascId) const noexcept;
        [[nodiscard]] const PassiveNodeDef* FindNode(std::uint32_t nodeId) const noexcept;
        [[nodiscard]] const PassiveMasteryEffectDef* FindMasteryEffect(std::uint32_t effectId) const noexcept;
        [[nodiscard]] const std::vector<std::uint32_t>* FindGroupNotables(std::uint32_t groupId) const noexcept;
        [[nodiscard]] const std::vector<std::uint32_t>* FindHubOptions(std::uint32_t hubNodeId) const noexcept;

        [[nodiscard]] std::size_t NodeCount() const noexcept { return nodes_.size(); }
        [[nodiscard]] std::size_t MasteryEffectCount() const noexcept { return masteryEffects_.size(); }
        [[nodiscard]] const std::unordered_map<std::uint32_t, PassiveNodeDef>& AllNodes() const noexcept { return nodes_; }

    private:
        bool loaded_ = false;
        std::uint32_t generation_ = 0;
        std::vector<PassiveClassDef> classes_;
        std::unordered_map<std::uint32_t, PassiveNodeDef> nodes_;
        std::unordered_map<std::uint32_t, PassiveMasteryEffectDef> masteryEffects_;
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> groupNotables_;
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> hubOptions_;
    };
}
