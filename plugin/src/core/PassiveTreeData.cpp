#include "core/PassiveTreeData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

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

        bool IsContentId(std::string_view id, std::string_view prefix)
        {
            return id.starts_with(prefix) && id.size() > prefix.size() && id.size() <= 64 &&
                id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._") ==
                    std::string_view::npos;
        }

        PassiveAscendancyDef ReadAscendancy(const json& entry, std::uint8_t classId,
            std::uint32_t generation, std::unordered_set<std::string>& seenIds)
        {
            PassiveAscendancyDef asc;
            asc.id = entry.at("id").get<std::string>();
            asc.contentId = generation == 0 && !asc.id.starts_with("asc.") ? "asc." + asc.id : asc.id;
            if (!IsContentId(asc.contentId, "asc.") || !seenIds.insert(asc.contentId).second) {
                throw std::runtime_error("Invalid or duplicate ascendancy content ID: " + asc.contentId);
            }
            asc.name = entry.value("name", asc.id);
            asc.classId = classId;
            asc.startNodeId = entry.value("startNodeId", 0u);
            return asc;
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

            const auto generation = root.contains("meta") ? root.at("meta").value("contentGeneration", 0u) : 0u;
            const auto& classes = root.at("classes");
            if (!classes.is_array() || classes.empty() ||
                classes.size() > std::numeric_limits<std::uint8_t>::max() + 1u) {
                SetError(errorOut, "Passive tree JSON must define a non-empty class array with byte-sized IDs.");
                return false;
            }
            std::vector<PassiveClassDef> nextClasses;
            std::unordered_set<std::string> classIds;
            std::unordered_set<std::string> ascendancyIds;
            for (const auto& cj : classes) {
                PassiveClassDef cls;
                const auto& id = cj.at("id");
                if (!id.is_number_integer() || id.get<std::uint64_t>() >= classes.size()) {
                    SetError(errorOut, "Passive tree JSON class IDs must be contiguous from zero.");
                    return false;
                }
                cls.id = id.get<std::uint8_t>();
                cls.name = cj.value("name", std::string{});
                cls.contentId = generation == 0 ? cj.value("contentId", "cls." + cls.name)
                                                : cj.at("contentId").get<std::string>();
                if (!IsContentId(cls.contentId, "cls.") || !classIds.insert(cls.contentId).second) {
                    SetError(errorOut, "Invalid or duplicate class content ID: " + cls.contentId);
                    return false;
                }
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
                        auto asc = ReadAscendancy(aj, cls.id, generation, ascendancyIds);
                        if (aj.contains("replacements")) {
                            for (const auto& rj : aj.at("replacements")) {
                                auto replacement = ReadAscendancy(rj, cls.id, generation, ascendancyIds);
                                if (replacement.startNodeId != asc.startNodeId) {
                                    throw std::runtime_error("Replacement start differs from " + asc.contentId);
                                }
                                asc.replacements.push_back(std::move(replacement));
                            }
                        }
                        cls.ascendancies.push_back(std::move(asc));
                    }
                }
                nextClasses.push_back(std::move(cls));
            }

            std::sort(nextClasses.begin(), nextClasses.end(),
                [](const PassiveClassDef& a, const PassiveClassDef& b) { return a.id < b.id; });
            for (std::size_t i = 0; i < nextClasses.size(); ++i) {
                if (nextClasses[i].id != i) {
                    SetError(errorOut, "Passive tree JSON class IDs must be contiguous from zero.");
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
                const auto& kind = nj.at("k");
                if (!kind.is_number_integer() ||
                    kind.get<std::uint64_t>() > static_cast<std::uint64_t>(PassiveNodeKind::Attribute)) {
                    SetError(errorOut, "Unknown passive node kind at node " + std::to_string(node.id));
                    return false;
                }
                if (node.id == 0) {
                    continue;
                }
                node.kind = static_cast<PassiveNodeKind>(kind.get<std::uint8_t>());
                node.groupId = nj.value("g", 0u);
                node.name = nj.value("name", std::string{});
                node.ascendancyId = nj.value("asc", std::string{});
                node.isMultipleChoice = nj.value("mc", false);
                node.isMultipleChoiceOption = nj.value("mco", false);
                node.choiceParentId = nj.value("mcParent", 0u);
                node.grantedPassivePoints = nj.value("grantPts", 0u);
                node.grantsClassStart = static_cast<std::int8_t>(nj.value("grantStart", -1));
                node.deferred = nj.value("deferred", false);
                node.unlock = nj.value("unlock", std::vector<std::uint32_t>{});
                node.freeAlloc = nj.value("freeAlloc", false);
                node.jewelSocket = nj.value("jewelSocket", false);
                node.alias = nj.value("alias", std::string{});
                if (nj.contains("attrOptions")) {
                    for (const auto& option : nj.at("attrOptions")) {
                        const auto optionId = option.at("id").get<std::uint32_t>();
                        if (optionId == 0 || node.FindAttributeOption(optionId)) {
                            throw std::runtime_error("Invalid attribute option at node " + std::to_string(node.id));
                        }
                        node.attrOptions.push_back({ { option.at("name").get<std::string>(),
                            option.at("stats").get<std::vector<std::string>>() }, optionId });
                    }
                }
                if (node.kind == PassiveNodeKind::Attribute && node.attrOptions.empty()) {
                    throw std::runtime_error("Missing attribute options at node " + std::to_string(node.id));
                }
                if (nj.contains("variants")) {
                    for (const auto& [id, variant] : nj.at("variants").items()) {
                        node.variants.emplace(id, PassiveNodeVariant{
                            { variant.at("name").get<std::string>(), variant.at("stats").get<std::vector<std::string>>() },
                            variant.value("sourceId", 0u), variant.value("asc", std::string{}) });
                    }
                }

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
            generation_ = generation;
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

    const PassiveClassDef* PassiveTreeData::FindClassByContentId(std::string_view id) const noexcept
    {
        const auto found = std::ranges::find(classes_, id, &PassiveClassDef::contentId);
        return found != classes_.end() ? &*found : nullptr;
    }

    const PassiveAscendancyDef* PassiveTreeData::FindAscendancy(std::string_view ascId) const noexcept
    {
        for (const auto& cls : classes_) {
            for (const auto& asc : cls.ascendancies) {
                if (asc.id == ascId || asc.contentId == ascId) {
                    return &asc;
                }
                const auto found = std::ranges::find(asc.replacements, ascId, &PassiveAscendancyDef::contentId);
                if (found != asc.replacements.end()) {
                    return &*found;
                }
            }
        }
        return nullptr;
    }

    const PassiveAttributeOption* PassiveNodeDef::FindAttributeOption(std::uint32_t optionId) const noexcept
    {
        const auto found = std::ranges::find(attrOptions, optionId, &PassiveAttributeOption::id);
        return found != attrOptions.end() ? &*found : nullptr;
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
