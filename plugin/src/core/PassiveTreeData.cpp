#include "core/PassiveTreeData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace SkyrimPoE::Core
{
    namespace
    {
        using json = nlohmann::json;

        void SetError(std::string* out, std::string msg)
        {
            if (out) {
                *out = std::move(msg);
            }
        }
    }

    bool PassiveTreeData::LoadFromFile(const std::filesystem::path& path, std::string* errorOut)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            SetError(errorOut, "Failed to open passive tree file: " + path.string());
            return false;
        }
        std::ostringstream oss;
        oss << ifs.rdbuf();
        return LoadFromJsonString(oss.str(), errorOut);
    }

    bool PassiveTreeData::LoadFromJsonString(std::string_view jsonText, std::string* errorOut)
    {
        try {
            const json root = json::parse(jsonText);
            if (!root.is_object() || !root.contains("classes") || !root.contains("nodes")) {
                SetError(errorOut, "Passive tree JSON missing required 'classes' or 'nodes'.");
                return false;
            }

            std::vector<PassiveClassDef> nextClasses;
            for (const auto& cj : root.at("classes")) {
                PassiveClassDef cls;
                cls.id = static_cast<std::uint8_t>(cj.value("id", 0));
                cls.name = cj.value("name", std::string{});
                cls.baseStr = cj.value("baseStr", 20u);
                cls.baseDex = cj.value("baseDex", 20u);
                cls.baseInt = cj.value("baseInt", 20u);
                cls.startNodeId = cj.value("startNodeId", 0u);
                if (cj.contains("startNeighbors") && cj["startNeighbors"].is_array()) {
                    for (const auto& nb : cj["startNeighbors"]) {
                        cls.startNeighbors.push_back(nb.get<std::uint32_t>());
                    }
                }
                if (cj.contains("ascendancies") && cj["ascendancies"].is_array()) {
                    for (const auto& aj : cj["ascendancies"]) {
                        PassiveAscendancyDef asc;
                        asc.id = aj.value("id", std::string{});
                        asc.name = aj.value("name", asc.id);
                        asc.classId = cls.id;
                        asc.startNodeId = aj.value("startNodeId", 0u);
                        if (!asc.id.empty()) {
                            cls.ascendancies.push_back(std::move(asc));
                        }
                    }
                }
                nextClasses.push_back(std::move(cls));
            }

            if (nextClasses.size() != 7) {
                SetError(errorOut, "Passive tree JSON must define exactly 7 classes.");
                return false;
            }
            std::sort(nextClasses.begin(), nextClasses.end(),
                [](const PassiveClassDef& a, const PassiveClassDef& b) { return a.id < b.id; });
            for (std::uint8_t i = 0; i < 7; ++i) {
                if (nextClasses[i].id != i) {
                    SetError(errorOut, "Passive tree JSON class IDs must be contiguous 0..6.");
                    return false;
                }
            }

            std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> nextGroupNotables;
            if (root.contains("masteryGroups") && root["masteryGroups"].is_object()) {
                for (const auto& [gStr, arr] : root["masteryGroups"].items()) {
                    const auto gId = static_cast<std::uint32_t>(std::stoul(gStr));
                    auto& vec = nextGroupNotables[gId];
                    if (arr.is_array()) {
                        for (const auto& v : arr) {
                            vec.push_back(v.get<std::uint32_t>());
                        }
                    }
                }
            }

            std::unordered_map<std::uint32_t, PassiveMasteryEffectDef> nextEffects;
            if (root.contains("masteryEffects") && root["masteryEffects"].is_object()) {
                nextEffects.reserve(root["masteryEffects"].size());
                for (const auto& [eStr, ej] : root["masteryEffects"].items()) {
                    PassiveMasteryEffectDef eff;
                    eff.id = ej.value("id", static_cast<std::uint32_t>(std::stoul(eStr)));
                    if (ej.contains("stats") && ej["stats"].is_array()) {
                        for (const auto& s : ej["stats"]) {
                            if (s.is_string()) {
                                eff.stats.push_back(s.get<std::string>());
                            }
                        }
                    }
                    nextEffects.emplace(eff.id, std::move(eff));
                }
            }

            std::unordered_map<std::uint32_t, PassiveNodeDef> nextNodes;
            std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> nextHubOptions;
            const auto& nodesArr = root.at("nodes");
            if (!nodesArr.is_array() || nodesArr.empty()) {
                SetError(errorOut, "Passive tree JSON 'nodes' must be a non-empty array.");
                return false;
            }
            nextNodes.reserve(nodesArr.size());

            for (const auto& nj : nodesArr) {
                PassiveNodeDef node;
                node.id = nj.value("id", 0u);
                if (node.id == 0) {
                    continue;
                }
                const auto kVal = nj.value("k", 0u);
                node.kind = (kVal <= static_cast<std::uint32_t>(PassiveNodeKind::AscendancyStart))
                    ? static_cast<PassiveNodeKind>(kVal)
                    : PassiveNodeKind::Normal;
                node.groupId = nj.value("g", 0u);
                node.name = nj.value("name", std::string{});
                node.ascendancyId = nj.value("asc", std::string{});
                node.isMultipleChoice = nj.value("mc", false);
                node.isMultipleChoiceOption = nj.value("mco", false);
                node.choiceParentId = nj.value("mcParent", 0u);
                node.grantedPassivePoints = nj.value("grantPts", 0u);
                node.grantsClassStart = static_cast<std::int8_t>(nj.value("grantStart", -1));
                node.deferred = nj.value("deferred", false);

                if (nj.contains("adj") && nj["adj"].is_array()) {
                    node.adjacency.reserve(nj["adj"].size());
                    for (const auto& nb : nj["adj"]) {
                        node.adjacency.push_back(nb.get<std::uint32_t>());
                    }
                }
                if (nj.contains("stats") && nj["stats"].is_array()) {
                    node.stats.reserve(nj["stats"].size());
                    for (const auto& s : nj["stats"]) {
                        if (s.is_string()) {
                            node.stats.push_back(s.get<std::string>());
                        }
                    }
                }
                if (nj.contains("effects") && nj["effects"].is_array()) {
                    node.masteryEffectIds.reserve(nj["effects"].size());
                    for (const auto& eid : nj["effects"]) {
                        node.masteryEffectIds.push_back(eid.get<std::uint32_t>());
                    }
                }
                if (node.isMultipleChoiceOption && node.choiceParentId != 0) {
                    nextHubOptions[node.choiceParentId].push_back(node.id);
                }
                nextNodes.emplace(node.id, std::move(node));
            }

            classes_ = std::move(nextClasses);
            groupNotables_ = std::move(nextGroupNotables);
            masteryEffects_ = std::move(nextEffects);
            nodes_ = std::move(nextNodes);
            hubOptions_ = std::move(nextHubOptions);
            for (auto& [hid, opts] : hubOptions_) {
                std::sort(opts.begin(), opts.end());
            }
            loaded_ = true;
            return true;
        } catch (const std::exception& e) {
            SetError(errorOut, std::string("PassiveTreeData parse error: ") + e.what());
            return false;
        }
    }

    const PassiveClassDef* PassiveTreeData::FindClass(std::uint8_t classId) const noexcept
    {
        if (classId >= classes_.size()) {
            return nullptr;
        }
        return &classes_[classId];
    }

    const PassiveClassDef* PassiveTreeData::FindClassByName(std::string_view name) const noexcept
    {
        for (const auto& cls : classes_) {
            if (cls.name == name) {
                return &cls;
            }
        }
        return nullptr;
    }

    const PassiveAscendancyDef* PassiveTreeData::FindAscendancy(std::string_view ascId) const noexcept
    {
        for (const auto& cls : classes_) {
            for (const auto& asc : cls.ascendancies) {
                if (asc.id == ascId) {
                    return &asc;
                }
            }
        }
        return nullptr;
    }

    const PassiveNodeDef* PassiveTreeData::FindNode(std::uint32_t nodeId) const noexcept
    {
        const auto it = nodes_.find(nodeId);
        return (it != nodes_.end()) ? &it->second : nullptr;
    }

    const PassiveMasteryEffectDef* PassiveTreeData::FindMasteryEffect(std::uint32_t effectId) const noexcept
    {
        const auto it = masteryEffects_.find(effectId);
        return (it != masteryEffects_.end()) ? &it->second : nullptr;
    }

    const std::vector<std::uint32_t>* PassiveTreeData::FindGroupNotables(std::uint32_t groupId) const noexcept
    {
        const auto it = groupNotables_.find(groupId);
        return (it != groupNotables_.end()) ? &it->second : nullptr;
    }

    const std::vector<std::uint32_t>* PassiveTreeData::FindHubOptions(std::uint32_t hubNodeId) const noexcept
    {
        const auto it = hubOptions_.find(hubNodeId);
        return (it != hubOptions_.end()) ? &it->second : nullptr;
    }
}
