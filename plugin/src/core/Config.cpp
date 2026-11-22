#include "core/Config.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>

namespace SkyrimPoE::Core
{
    namespace
    {
        std::string_view Trim(std::string_view sv)
        {
            constexpr std::string_view ws = " \t\r\n";
            const auto first = sv.find_first_not_of(ws);
            if (first == std::string_view::npos) {
                return {};
            }
            const auto last = sv.find_last_not_of(ws);
            return sv.substr(first, last - first + 1);
        }

        bool ParseBool(std::string_view v, bool& out)
        {
            if (v == "true" || v == "1" || v == "TRUE" || v == "True") {
                out = true;
                return true;
            }
            if (v == "false" || v == "0" || v == "FALSE" || v == "False") {
                out = false;
                return true;
            }
            return false;
        }

        bool ParseUint(std::string_view v, std::uint32_t& out)
        {
            int base = 10;
            if (v.starts_with("0x") || v.starts_with("0X")) {
                v.remove_prefix(2);
                base = 16;
            }
            if (v.empty()) {
                return false;
            }
            std::uint32_t parsed = 0;
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), parsed, base);
            if (ec != std::errc{} || ptr != v.data() + v.size()) {
                return false;
            }
            out = parsed;
            return true;
        }

        bool ParseInt(std::string_view v, std::int32_t& out)
        {
            if (v.empty()) {
                return false;
            }
            std::int32_t parsed = 0;
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), parsed, 10);
            if (ec != std::errc{} || ptr != v.data() + v.size()) {
                return false;
            }
            out = parsed;
            return true;
        }

        bool ParseDouble(std::string_view v, double& out)
        {
            if (v.empty()) {
                return false;
            }
            double parsed = 0.0;
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), parsed);
            if (ec != std::errc{} || ptr != v.data() + v.size() || !std::isfinite(parsed)) {
                return false;
            }
            out = parsed;
            return true;
        }
    }

    ConfigLoadResult LoadConfigFromString(std::string_view content)
    {
        ConfigLoadResult result;
        std::string section;
        bool firstLine = true;
        std::size_t pos = 0;
        while (pos <= content.size()) {
            const auto nl = content.find('\n', pos);
            const auto rawLine = (nl == std::string_view::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
            pos = (nl == std::string_view::npos) ? (content.size() + 1) : (nl + 1);

            auto line = rawLine;
            if (firstLine) {
                firstLine = false;
                if (line.starts_with("\xEF\xBB\xBF")) {
                    line.remove_prefix(3);
                }
            }
            if (const auto comment = line.find_first_of(";#"); comment != std::string_view::npos) {
                line = line.substr(0, comment);
            }
            line = Trim(line);
            if (line.empty()) {
                continue;
            }
            if (line.front() == '[' && line.back() == ']') {
                section = std::string(Trim(line.substr(1, line.size() - 2)));
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string_view::npos) {
                result.warnings.emplace_back("Malformed INI line ignored: " + std::string(line));
                continue;
            }
            const auto key = Trim(line.substr(0, eq));
            const auto val = Trim(line.substr(eq + 1));

            auto& c = result.config;
            auto warnInvalid = [&]() {
                result.warnings.emplace_back("Invalid INI value for [" + section + "] " + std::string(key) + " = " + std::string(val));
            };

            if (section == "General") {
                if (key == "PanelKey") {
                    std::uint32_t u = 0;
                    if (ParseUint(val, u) && u <= 0xFF) c.panelKey = u; else warnInvalid();
                } else if (key == "YieldToFrenzy") {
                    if (!ParseBool(val, c.yieldToFrenzy)) warnInvalid();
                } else if (key == "GiveStarterGems") {
                    if (!ParseBool(val, c.giveStarterGems)) warnInvalid();
                } else if (key == "StarterSockets") {
                    std::uint32_t u = 0;
                    if (ParseUint(val, u) && u >= 1 && u <= kMaxGemSockets) c.starterSockets = static_cast<std::uint8_t>(u); else warnInvalid();
                } else if (key == "MainUpdateBudgetMs") {
                    double d = 0.0;
                    if (ParseDouble(val, d) && d >= 0.0 && d <= 50.0) c.mainUpdateBudgetMs = d; else warnInvalid();
                } else {
                    warnInvalid();
                }
            } else if (section == "Conversion") {
                double d = 0.0;
                std::uint32_t u = 0;
                if (key == "OffenseK") {
                    if (ParseDouble(val, d) && d > 0.0) c.offenseK = d; else warnInvalid();
                } else if (key == "OffenseBaseHits") {
                    if (ParseDouble(val, d) && d > 0.0) c.offenseBaseHits = d; else warnInvalid();
                } else if (key == "BaseHitRate") {
                    if (ParseDouble(val, d) && d > 0.0) c.baseHitRate = d; else warnInvalid();
                } else if (key == "OffenseRollback") {
                    if (ParseDouble(val, d) && d >= 0.0 && d <= 1.0) c.offenseRollback = d; else warnInvalid();
                } else if (key == "DefenseRollback") {
                    if (ParseDouble(val, d) && d >= 0.0 && d <= 1.0) c.defenseRollback = d; else warnInvalid();
                } else if (key == "DefenseDref") {
                    if (ParseDouble(val, d) && d > 0.0) c.defenseDref = d; else warnInvalid();
                } else if (key == "ExpectedArmorIncrease") {
                    if (ParseDouble(val, d) && d >= 0.0) c.expectedArmorIncrease = d; else warnInvalid();
                } else if (key == "ExpectedLifeIncrease") {
                    if (ParseDouble(val, d) && d >= 0.0) c.expectedLifeIncrease = d; else warnInvalid();
                } else if (key == "ExpectedManaIncrease") {
                    if (ParseDouble(val, d) && d >= 0.0) c.expectedManaIncrease = d; else warnInvalid();
                } else if (key == "ExpectedResistAffixes") {
                    if (ParseDouble(val, d) && d >= 0.0) c.expectedResistAffixes = d; else warnInvalid();
                } else if (key == "Act5Level") {
                    if (ParseUint(val, u) && u >= 1 && u <= kMaxExileLevel) c.act5Level = u; else warnInvalid();
                } else if (key == "Act10Level") {
                    if (ParseUint(val, u) && u >= 1 && u <= kMaxExileLevel) c.act10Level = u; else warnInvalid();
                } else {
                    warnInvalid();
                }
            } else if (section == "Experience") {
                double d = 0.0;
                std::int32_t i = 0;
                if (key == "LevelDiffClamp") {
                    if (ParseInt(val, i) && i >= 0 && i <= 50) c.levelDiffClamp = i; else warnInvalid();
                } else if (key == "BossLevelBonus") {
                    if (ParseInt(val, i) && i >= 0 && i <= 20) c.bossLevelBonus = i; else warnInvalid();
                } else if (key == "MaxBossLevelBonus") {
                    if (ParseInt(val, i) && i >= 0 && i <= 20) c.maxBossLevelBonus = i; else warnInvalid();
                } else if (key == "HigherEnemyBonusPerLevel") {
                    if (ParseDouble(val, d) && d >= 0.0) c.higherEnemyBonusPerLevel = d; else warnInvalid();
                } else if (key == "HigherEnemyMaxMultiplier") {
                    if (ParseDouble(val, d) && d >= 1.0) c.higherEnemyMaxMultiplier = d; else warnInvalid();
                } else if (key == "LowerEnemyMinMultiplier") {
                    if (ParseDouble(val, d) && d >= 0.0 && d <= 1.0) c.lowerEnemyMinMultiplier = d; else warnInvalid();
                } else if (key == "KillsBase") {
                    if (ParseDouble(val, d) && d > 0.0) c.killsBase = d; else warnInvalid();
                } else if (key == "KillsSlope") {
                    if (ParseDouble(val, d) && d >= 0.0) c.killsSlope = d; else warnInvalid();
                } else if (key == "BaseKillXp") {
                    if (ParseDouble(val, d) && d > 0.0) c.baseKillXp = d; else warnInvalid();
                } else if (key == "GemXpRatio") {
                    if (ParseDouble(val, d) && d >= 0.0 && d <= 1.0) c.gemXpRatio = d; else warnInvalid();
                } else {
                    warnInvalid();
                }
            } else if (section == "Cyclone") {
                double d = 0.0;
                if (key == "PowerAttackInput") {
                    if (val == "rightHand" || val == "leftHand" || val == "both") {
                        c.powerAttackInput = std::string(val);
                    } else {
                        warnInvalid();
                    }
                } else if (key == "MinChannelSeconds") {
                    if (ParseDouble(val, d) && d >= 0.0 && d <= 5.0) c.minChannelSeconds = d; else warnInvalid();
                } else if (key == "ChannelIgnoreStagger") {
                    if (ParseDouble(val, d) && d >= 0.0 && d <= 10.0) c.channelIgnoreStagger = d; else warnInvalid();
                } else {
                    warnInvalid();
                }
            } else if (section == "PassiveTree") {
                std::uint32_t u = 0;
                std::int32_t i = 0;
                if (key == "TreePanelKey") {
                    if (ParseUint(val, u) && u <= 0xFF) c.treePanelKey = u; else warnInvalid();
                } else if (key == "FreePassiveRefund") {
                    if (!ParseBool(val, c.freePassiveRefund)) warnInvalid();
                } else if (key == "SimpleImeMode") {
                    if (val == "auto" || val == "on" || val == "off") {
                        c.simpleImeMode = std::string(val);
                    } else {
                        warnInvalid();
                    }
                } else if (key == "BonusPassivePoints") {
                    if (ParseInt(val, i) && i >= 0 && i <= 200) c.bonusPassivePoints = i; else warnInvalid();
                } else if (key == "BonusAscendancyPoints") {
                    if (ParseInt(val, i) && i >= 0 && i <= static_cast<std::int32_t>(kMaxAscendancyPoints)) {
                        c.bonusAscendancyPoints = i;
                    } else {
                        warnInvalid();
                    }
                } else {
                    warnInvalid();
                }
            } else if (section == "Items") {
                std::uint32_t u = 0;
                if (key == "GearPanelKey") {
                    if (ParseUint(val, u) && u <= 0xFF) c.gearPanelKey = u; else warnInvalid();
                } else if (key == "GiveStarterCurrency") {
                    if (!ParseBool(val, c.giveStarterCurrency)) warnInvalid();
                } else if (key == "EnforceGemRequirements") {
                    if (!ParseBool(val, c.enforceGemRequirements)) warnInvalid();
                } else if (key == "IncludeModdedBasesInTokenPool") {
                    if (!ParseBool(val, c.includeModdedBasesInTokenPool)) warnInvalid();
                } else if (key == "AscendancyRefundCost") {
                    if (ParseUint(val, u) && u >= 1 && u <= 50) c.ascendancyRefundCost = u; else warnInvalid();
                } else {
                    warnInvalid();
                }
            } else if (section == "Loot") {
                double d = 0.0;
                std::uint32_t u = 0;
                if (key == "EnableLootDrops") {
                    if (!ParseBool(val, c.enableLootDrops)) warnInvalid();
                } else if (key == "TokenDropChance") {
                    if (ParseDouble(val, d) && d >= 0.0 && d <= 1.0) c.tokenDropChance = d; else warnInvalid();
                } else if (key == "RareTokenRatio") {
                    if (ParseDouble(val, d) && d >= 0.0 && d <= 1.0) c.rareTokenRatio = d; else warnInvalid();
                } else if (key == "CurrencyDropChance") {
                    if (ParseDouble(val, d) && d >= 0.0 && d <= 1.0) c.currencyDropChance = d; else warnInvalid();
                } else if (key == "BossDropMultiplier") {
                    if (ParseDouble(val, d) && d >= 1.0 && d <= 10.0) c.bossDropMultiplier = d; else warnInvalid();
                } else if (key == "WorldDropEnabled") {
                    if (!ParseBool(val, c.worldDropEnabled)) warnInvalid();
                } else if (key == "WorldDropMinTier") {
                    if (ParseUint(val, u) && u <= 2) c.worldDropMinTier = static_cast<std::uint8_t>(u); else warnInvalid();
                } else if (key == "WorldDropMaxCount") {
                    if (ParseUint(val, u) && u >= 1 && u <= 64) c.worldDropMaxCount = u; else warnInvalid();
                } else if (key == "WorldDropReturnSeconds") {
                    if (ParseDouble(val, d) && d >= 5.0 && d <= 3600.0) c.worldDropReturnSeconds = d; else warnInvalid();
                } else if (key == "LootBeamEnabled") {
                    if (!ParseBool(val, c.lootBeamEnabled)) warnInvalid();
                } else if (key == "LootSoundEnabled") {
                    if (!ParseBool(val, c.lootSoundEnabled)) warnInvalid();
                } else {
                    warnInvalid();
                }
            } else if (section == "Spirit") {
                std::uint32_t u = 0;
                if (key == "SpiritBase") {
                    if (ParseUint(val, u) && u <= 2000) c.spiritBase = u; else warnInvalid();
                } else if (key == "SpiritPerMilestone") {
                    if (ParseUint(val, u) && u <= 500) c.spiritPerMilestone = u; else warnInvalid();
                } else {
                    warnInvalid();
                }
            } else if (section == "Charms") {
                if (key == "EnableCharms") {
                    if (!ParseBool(val, c.enableCharms)) warnInvalid();
                } else {
                    warnInvalid();
                }
            } else if (section == "Monsters") {
                double d = 0.0;
                if (key == "EnableMonsterPromotion") {
                    if (!ParseBool(val, c.enableMonsterPromotion)) warnInvalid();
                } else if (key == "MagicMonsterChance") {
                    if (ParseDouble(val, d) && d >= 0.0 && d <= 1.0) c.magicMonsterChance = d; else warnInvalid();
                } else if (key == "RareMonsterChance") {
                    if (ParseDouble(val, d) && d >= 0.0 && d <= 1.0) c.rareMonsterChance = d; else warnInvalid();
                } else if (key == "UniqueDropChance") {
                    if (ParseDouble(val, d) && d >= 0.0 && d <= 1.0) c.uniqueDropChance = d; else warnInvalid();
                } else if (key == "MonsterEsRatio") {
                    if (ParseDouble(val, d) && d >= 0.0 && d <= 2.0) c.monsterEsRatio = d; else warnInvalid();
                } else {
                    warnInvalid();
                }
            } else if (section == "Maps") {
                std::uint32_t u = 0;
                double d = 0.0;
                if (key == "MapPanelKey") {
                    if (ParseUint(val, u) && u <= 0xFF) c.mapPanelKey = u; else warnInvalid();
                } else if (key == "MapDeathInterceptEnabled") {
                    if (!ParseBool(val, c.mapDeathInterceptEnabled)) warnInvalid();
                } else if (key == "MapBasePackCount") {
                    if (ParseUint(val, u) && u >= 1 && u <= 6) c.mapBasePackCount = u; else warnInvalid();
                } else if (key == "MapTravelTimeoutSec") {
                    if (ParseDouble(val, d) && d >= 5.0 && d <= 120.0) result.mapTravel.timeoutSec = d; else warnInvalid();
                } else if (key == "MapTravelArrivalRadius") {
                    if (ParseDouble(val, d) && d >= 64.0 && d <= 4096.0) result.mapTravel.arrivalRadius = d; else warnInvalid();
                } else if (key == "MapTravelMaxAttempts") {
                    if (ParseUint(val, u) && u >= 1 && u <= 5) result.mapTravel.maxAttempts = u; else warnInvalid();
                } else {
                    warnInvalid();
                }
            } else {
                // C-11: 알 수 없는 INI 섹션(오타 등)은 조용히 무시하지 않고 경고를 남긴다.
                result.warnings.emplace_back("Unknown INI section [" + section + "] for key " + std::string(key));
            }
        }

        if (result.config.act5Level > result.config.act10Level) {
            result.config.act10Level = result.config.act5Level;
            result.warnings.emplace_back("Act5Level exceeded Act10Level; clamped Act10Level to Act5Level.");
        }
        return result;
    }

    ConfigLoadResult LoadConfigFromFile(const std::filesystem::path& path)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            ConfigLoadResult res;
            res.fileFound = false;
            return res;
        }
        std::ostringstream oss;
        oss << ifs.rdbuf();
        auto res = LoadConfigFromString(oss.str());
        res.fileFound = true;
        return res;
    }
}
