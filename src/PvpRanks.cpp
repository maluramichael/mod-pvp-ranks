/*
 * mod-pvp-ranks
 *
 * Seasonal, 14-rank PvP progression system for the mod-playerbots AzerothCore
 * fork (WotLK 3.3.5a). Every honorable player kill (OnPlayerPVPKill) awards
 * PvpRanks.PointsPerKill points to the killer, capped at PvpRanks.WeeklyCap per
 * epoch week. Cumulative points map onto the classic 14 PvP ranks (Private /
 * Scout ... Grand Marshal / High Warlord), picked by the killer's faction.
 *
 * Per-character progress lives in the characters DB table `pvp_rank_points`
 * (programmatic schema - this fork aborts worldserver boot on a failing module
 * SQL file, so we create the table at runtime instead, matching mod-self-found
 * and mod-guild-tax) and is cached in memory while the character is online.
 * Every point award persists immediately, so the cache is pure read
 * acceleration, not the source of truth.
 *
 * Season handling: PvpRanks.Season is the "current" season number. A
 * character whose stored season doesn't match is archived into
 * `pvp_rank_points_archive` and reset to rank 1 / 0 points the next time it
 * logs in. `.pvprank resetseason` (GM) forces that for every stored character
 * immediately, without waiting for each one to log back in.
 *
 * Released under GNU GPL v2; redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"

// Playerbots fork header, used only to recognise a bot-controlled session so
// PvpRanks.CountBots=0 can exclude bot kills from earning rank points.
// Enforcement itself is guid based and applies the same for a human- or
// bot-controlled character otherwise.
#include "Playerbots.h"

#include "PvpRanks.h"

#include <algorithm>
#include <unordered_map>

namespace PvpRanks
{
    Config& GetConfig()
    {
        static Config cfg;
        return cfg;
    }
}

using namespace Acore::ChatCommands;
using PvpRanks::Config;
using PvpRanks::GetConfig;

namespace
{
    // ---------------------------------------------------------------
    //  Rank thresholds and names
    // ---------------------------------------------------------------

    // Cumulative points required to HOLD each rank. Index 0 == rank 1 (always
    // 0 - everyone starts here). Roughly geometric; tuned around the default
    // PointsPerKill=10 / WeeklyCap=2000 so a character playing every week to
    // the cap reaches rank 14 in about 12 weeks of a season.
    constexpr uint32 kThresholds[PvpRanks::kRankCount] =
    {
        0,     // 1
        200,   // 2
        450,   // 3
        800,   // 4
        1300,  // 5
        2000,  // 6
        3000,  // 7
        4500,  // 8
        6500,  // 9
        9000,  // 10
        12000, // 11
        15500, // 12
        19500, // 13
        24000, // 14
    };

    // Classic WotLK-era PvP rank titles, 1-based (index 0 == rank 1).
    constexpr char const* kAllianceNames[PvpRanks::kRankCount] =
    {
        "Private", "Corporal", "Sergeant", "Master Sergeant", "Sergeant Major",
        "Knight", "Knight-Lieutenant", "Knight-Captain", "Knight-Champion",
        "Lieutenant Commander", "Commander", "Marshal", "Field Marshal", "Grand Marshal",
    };

    constexpr char const* kHordeNames[PvpRanks::kRankCount] =
    {
        "Scout", "Grunt", "Sergeant", "Senior Sergeant", "First Sergeant",
        "Stone Guard", "Blood Guard", "Legionnaire", "Centurion",
        "Champion", "Lieutenant General", "General", "Warlord", "High Warlord",
    };

    // ---------------------------------------------------------------
    //  In-memory cache
    // ---------------------------------------------------------------

    struct RankRow
    {
        uint32  points       = 0;
        uint8   rankIdx      = 1;
        uint32  season       = 0;
        uint32  weeklyPoints = 0;
        uint32  week         = 0;
    };

    // Only ever touched from the world update thread (chat commands and
    // script hooks both run there on this core), so no locking is needed -
    // the same assumption mod-self-found's and mod-guild-tax's in-memory
    // state relies on.
    std::unordered_map<uint32, RankRow>& Cache()
    {
        static std::unordered_map<uint32, RankRow> cache;
        return cache;
    }

    RankRow* FindCachedRow(uint32 guidLow)
    {
        auto it = Cache().find(guidLow);
        return it != Cache().end() ? &it->second : nullptr;
    }

    // ---------------------------------------------------------------
    //  DB helpers
    // ---------------------------------------------------------------

    void PersistRow(uint32 guidLow, RankRow const& row)
    {
        CharacterDatabase.Execute(
            "INSERT INTO `pvp_rank_points` (`guid`, `points`, `rankIdx`, `season`, `weekly_points`, `week`) "
            "VALUES ({}, {}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE `points` = VALUES(`points`), `rankIdx` = VALUES(`rankIdx`), "
            "`season` = VALUES(`season`), `weekly_points` = VALUES(`weekly_points`), `week` = VALUES(`week`)",
            guidLow, row.points, uint32(row.rankIdx), row.season, row.weeklyPoints, row.week);
    }

    void ArchiveRow(uint32 guidLow, RankRow const& row)
    {
        CharacterDatabase.Execute(
            "INSERT INTO `pvp_rank_points_archive` (`guid`, `season`, `points`, `rankIdx`, `ts`) "
            "VALUES ({}, {}, {}, {}, {})",
            guidLow, row.season, row.points, uint32(row.rankIdx), uint32(GameTime::GetGameTime().count()));
    }

    // True if the given player is a playerbot (has bot AI). Real players
    // never have a bot AI.
    bool IsBot(Player* player)
    {
        return player && GET_PLAYERBOT_AI(player) != nullptr;
    }
}

namespace PvpRanks
{
    uint32 PointThresholdForRank(uint8_t rankIdx)
    {
        if (rankIdx < 1)
            rankIdx = 1;
        if (rankIdx > kRankCount)
            rankIdx = kRankCount;

        return kThresholds[rankIdx - 1];
    }

    std::string const& RankName(uint8_t rankIdx, TeamId teamId)
    {
        if (rankIdx < 1)
            rankIdx = 1;
        if (rankIdx > kRankCount)
            rankIdx = kRankCount;

        static std::string const allianceNames[kRankCount] =
        {
            kAllianceNames[0], kAllianceNames[1], kAllianceNames[2], kAllianceNames[3],
            kAllianceNames[4], kAllianceNames[5], kAllianceNames[6], kAllianceNames[7],
            kAllianceNames[8], kAllianceNames[9], kAllianceNames[10], kAllianceNames[11],
            kAllianceNames[12], kAllianceNames[13],
        };
        static std::string const hordeNames[kRankCount] =
        {
            kHordeNames[0], kHordeNames[1], kHordeNames[2], kHordeNames[3],
            kHordeNames[4], kHordeNames[5], kHordeNames[6], kHordeNames[7],
            kHordeNames[8], kHordeNames[9], kHordeNames[10], kHordeNames[11],
            kHordeNames[12], kHordeNames[13],
        };

        return (teamId == TEAM_HORDE) ? hordeNames[rankIdx - 1] : allianceNames[rankIdx - 1];
    }

    uint8_t RankForPoints(uint32 points)
    {
        uint8_t rankIdx = 1;
        for (uint8_t i = 1; i <= kRankCount; ++i)
        {
            if (points >= kThresholds[i - 1])
                rankIdx = i;
            else
                break;
        }
        return rankIdx;
    }

    void EnsureSchema()
    {
        CharacterDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS `pvp_rank_points` ("
            "`guid` INT UNSIGNED NOT NULL, "
            "`points` INT UNSIGNED NOT NULL DEFAULT 0, "
            "`rankIdx` TINYINT UNSIGNED NOT NULL DEFAULT 1, "
            "`season` INT UNSIGNED NOT NULL DEFAULT 0, "
            "`weekly_points` INT UNSIGNED NOT NULL DEFAULT 0, "
            "`week` INT UNSIGNED NOT NULL DEFAULT 0, "
            "PRIMARY KEY (`guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");

        CharacterDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS `pvp_rank_points_archive` ("
            "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
            "`guid` INT UNSIGNED NOT NULL, "
            "`season` INT UNSIGNED NOT NULL, "
            "`points` INT UNSIGNED NOT NULL, "
            "`rankIdx` TINYINT UNSIGNED NOT NULL, "
            "`ts` INT UNSIGNED NOT NULL, "
            "PRIMARY KEY (`id`), "
            "KEY `idx_guid` (`guid`), "
            "KEY `idx_season` (`season`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
    }

    uint32 CurrentWeekKey()
    {
        // Epoch week number (UTC seconds / 604800). Changes exactly once
        // every 7 days (each boundary lands on Thursday 00:00 UTC, since the
        // Unix epoch itself was a Thursday) and is fully monotonic - good
        // enough for a weekly cap and far simpler/more robust than a
        // calendar ISO-8601 week (no year-boundary edge cases to get wrong).
        return static_cast<uint32>(GameTime::GetGameTime().count() / 604800);
    }

    void LoadOrCreate(Player* player)
    {
        if (!player)
            return;

        uint32 guidLow = player->GetGUID().GetCounter();
        RankRow row;

        if (QueryResult result = CharacterDatabase.Query(
            "SELECT `points`, `rankIdx`, `season`, `weekly_points`, `week` "
            "FROM `pvp_rank_points` WHERE `guid` = {}", guidLow))
        {
            Field* fields = result->Fetch();
            row.points       = fields[0].Get<uint32>();
            row.rankIdx      = fields[1].Get<uint8>();
            row.season       = fields[2].Get<uint32>();
            row.weeklyPoints = fields[3].Get<uint32>();
            row.week         = fields[4].Get<uint32>();
        }
        else
        {
            row.points       = 0;
            row.rankIdx      = 1;
            row.season       = GetConfig().Season;
            row.weeklyPoints = 0;
            row.week         = CurrentWeekKey();
        }

        // Season rollover: the character's stored season no longer matches
        // the configured one - archive what they had and reset them.
        if (row.season != GetConfig().Season)
        {
            ArchiveRow(guidLow, row);
            row.points       = 0;
            row.rankIdx      = 1;
            row.season       = GetConfig().Season;
            row.weeklyPoints = 0;
            row.week         = CurrentWeekKey();
        }

        Cache()[guidLow] = row;
        PersistRow(guidLow, row); // creates the row on first login / persists any reset above
    }

    void Evict(Player* player)
    {
        if (!player)
            return;

        Cache().erase(player->GetGUID().GetCounter());
    }

    void AwardKillPoints(Player* killer)
    {
        Config const& cfg = GetConfig();
        if (!cfg.Enable || !killer)
            return;

        uint32 guidLow = killer->GetGUID().GetCounter();
        RankRow* row = FindCachedRow(guidLow);
        if (!row)
            return; // no cached row - LoadOrCreate should have run at login

        uint32 weekKey = CurrentWeekKey();
        if (row->week != weekKey)
        {
            row->week = weekKey;
            row->weeklyPoints = 0;
        }

        if (row->weeklyPoints >= cfg.WeeklyCap)
            return; // weekly cap already reached

        uint32 award = std::min(cfg.PointsPerKill, cfg.WeeklyCap - row->weeklyPoints);
        if (award == 0)
            return;

        uint8_t oldRank = row->rankIdx;
        row->points += award;
        row->weeklyPoints += award;
        row->rankIdx = RankForPoints(row->points);

        PersistRow(guidLow, *row);

        if (cfg.AnnounceRankUp && row->rankIdx > oldRank)
        {
            ChatHandler(nullptr).SendWorldText("|cffff8000[PvP Rank]|r {} has been promoted to {}!",
                killer->GetName(), RankName(row->rankIdx, killer->GetTeamId()));
        }
    }

    RankSnapshot GetSnapshot(Player* player)
    {
        RankSnapshot snap;
        if (!player)
            return snap;

        RankRow* row = FindCachedRow(player->GetGUID().GetCounter());
        if (!row)
            return snap;

        snap.found        = true;
        snap.points       = row->points;
        snap.rankIdx      = row->rankIdx;
        snap.weeklyPoints = row->weeklyPoints;
        snap.weeklyCap    = GetConfig().WeeklyCap;
        return snap;
    }

    uint32 ResetAllToCurrentSeason()
    {
        Config const& cfg = GetConfig();
        uint32 weekKey = CurrentWeekKey();
        uint32 count = 0;

        if (QueryResult result = CharacterDatabase.Query(
            "SELECT `guid`, `points`, `rankIdx`, `season` FROM `pvp_rank_points`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 guidLow = fields[0].Get<uint32>();
                uint32 points  = fields[1].Get<uint32>();
                uint8  rankIdx = fields[2].Get<uint8>();
                uint32 season  = fields[3].Get<uint32>();

                RankRow archived;
                archived.points  = points;
                archived.rankIdx = rankIdx;
                archived.season  = season;
                ArchiveRow(guidLow, archived);

                CharacterDatabase.Execute(
                    "UPDATE `pvp_rank_points` SET `points` = 0, `rankIdx` = 1, `season` = {}, "
                    "`weekly_points` = 0, `week` = {} WHERE `guid` = {}",
                    cfg.Season, weekKey, guidLow);

                if (RankRow* cached = FindCachedRow(guidLow))
                {
                    cached->points       = 0;
                    cached->rankIdx      = 1;
                    cached->season       = cfg.Season;
                    cached->weeklyPoints = 0;
                    cached->week         = weekKey;
                }

                ++count;
            } while (result->NextRow());
        }

        return count;
    }
}

// =====================================================================
//  CommandScript: `.pvprank` / `.pvprank top` / `.pvprank resetseason`
// =====================================================================
class pvprank_commandscript : public CommandScript
{
public:
    pvprank_commandscript() : CommandScript("pvprank_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable pvpRankTable =
        {
            { "top",         HandlePvpRankTopCommand,         SEC_PLAYER,     Console::No },
            { "resetseason", HandlePvpRankResetSeasonCommand, SEC_GAMEMASTER, Console::Yes },
            { "",            HandlePvpRankCommand,             SEC_PLAYER,    Console::No },
        };

        static ChatCommandTable commandTable =
        {
            { "pvprank", pvpRankTable },
        };

        return commandTable;
    }

    static bool HandlePvpRankCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
        {
            handler->SendSysMessage("This command can only be used in-game.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!GetConfig().Enable)
        {
            handler->SendSysMessage("PvP ranks are currently disabled on this server.");
            return true;
        }

        PvpRanks::RankSnapshot snap = PvpRanks::GetSnapshot(player);
        if (!snap.found)
        {
            handler->SendSysMessage("No PvP rank data yet - go get some honorable kills.");
            return true;
        }

        std::string const& rankName = PvpRanks::RankName(snap.rankIdx, player->GetTeamId());

        if (snap.rankIdx >= PvpRanks::kRankCount)
        {
            handler->PSendSysMessage(
                "PvP Rank: {} (rank {}/{}) - {} points. Weekly: {}/{}. Maximum rank reached.",
                rankName, snap.rankIdx, PvpRanks::kRankCount, snap.points, snap.weeklyPoints, snap.weeklyCap);
        }
        else
        {
            uint32 nextThreshold = PvpRanks::PointThresholdForRank(snap.rankIdx + 1);
            uint32 needed = nextThreshold > snap.points ? nextThreshold - snap.points : 0;
            std::string const& nextName = PvpRanks::RankName(snap.rankIdx + 1, player->GetTeamId());

            handler->PSendSysMessage(
                "PvP Rank: {} (rank {}/{}) - {} points. Weekly: {}/{}. {} points to {}.",
                rankName, snap.rankIdx, PvpRanks::kRankCount, snap.points, snap.weeklyPoints, snap.weeklyCap,
                needed, nextName);
        }

        return true;
    }

    static bool HandlePvpRankTopCommand(ChatHandler* handler, Optional<uint32> count)
    {
        uint32 limit = count.value_or(10);
        if (limit == 0)
            limit = 10;
        if (limit > 25)
            limit = 25;

        Config const& cfg = GetConfig();
        QueryResult result = CharacterDatabase.Query(
            "SELECT c.`name`, c.`race`, p.`points`, p.`rankIdx` FROM `pvp_rank_points` p "
            "INNER JOIN `characters` c ON c.`guid` = p.`guid` "
            "WHERE p.`season` = {} ORDER BY p.`points` DESC LIMIT {}",
            cfg.Season, limit);

        if (!result)
        {
            handler->SendSysMessage("No PvP rank data for the current season yet.");
            return true;
        }

        handler->PSendSysMessage("=== Top PvP Ranks (season {}) ===", cfg.Season);
        uint32 place = 1;
        do
        {
            Field* fields = result->Fetch();
            std::string name = fields[0].Get<std::string>();
            uint8 race       = fields[1].Get<uint8>();
            uint32 points    = fields[2].Get<uint32>();
            uint8 rankIdx    = fields[3].Get<uint8>();

            TeamId team = Player::TeamIdForRace(race);
            handler->PSendSysMessage("{}. {} - {} ({} points)",
                place++, name, PvpRanks::RankName(rankIdx, team), points);
        } while (result->NextRow());

        return true;
    }

    static bool HandlePvpRankResetSeasonCommand(ChatHandler* handler)
    {
        uint32 count = PvpRanks::ResetAllToCurrentSeason();
        handler->PSendSysMessage(
            "PvP Ranks: reset {} character(s) to season {} (rank 1, 0 points). Prior standings archived.",
            count, GetConfig().Season);
        return true;
    }
};

// =====================================================================
//  PlayerScript: award points on PvP kill + cache lifecycle.
// =====================================================================
class PvpRanksPlayerScript : public PlayerScript
{
public:
    PvpRanksPlayerScript() : PlayerScript("PvpRanks_PlayerScript") { }

    void OnPlayerLogin(Player* player) override
    {
        if (!GetConfig().Enable || !player)
            return;

        PvpRanks::LoadOrCreate(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;

        PvpRanks::Evict(player);
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        Config const& cfg = GetConfig();
        if (!cfg.Enable || !killer || !killed || killer == killed)
            return;

        // Never award points for a bot-controlled killer when CountBots is
        // disabled - this covers bot-vs-bot (and bot-vs-player) kills alike.
        if (!cfg.CountBots && IsBot(killer))
            return;

        // Same account (e.g. two of Michael's own altbots dueling to the
        // death, or a griefed same-account alt) never earns rank points.
        if (killer->GetSession() && killed->GetSession() &&
            killer->GetSession()->GetAccountId() == killed->GetSession()->GetAccountId())
            return;

        PvpRanks::AwardKillPoints(killer);
    }
};

// =====================================================================
//  WorldScript: config load + schema bootstrap.
// =====================================================================
class PvpRanksWorldScript : public WorldScript
{
public:
    PvpRanksWorldScript() : WorldScript("PvpRanks_WorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        Config& cfg = GetConfig();
        cfg.Enable         = sConfigMgr->GetOption<bool>("PvpRanks.Enable", true);
        cfg.PointsPerKill  = sConfigMgr->GetOption<uint32>("PvpRanks.PointsPerKill", 10);
        cfg.WeeklyCap      = sConfigMgr->GetOption<uint32>("PvpRanks.WeeklyCap", 2000);
        cfg.Season         = sConfigMgr->GetOption<uint32>("PvpRanks.Season", 1);
        cfg.CountBots      = sConfigMgr->GetOption<bool>("PvpRanks.CountBots", true);
        cfg.AnnounceRankUp = sConfigMgr->GetOption<bool>("PvpRanks.AnnounceRankUp", true);

        // A weekly cap smaller than a single kill's award would silently
        // block all progress - clamp it up so PointsPerKill always fits.
        if (cfg.WeeklyCap < cfg.PointsPerKill)
            cfg.WeeklyCap = cfg.PointsPerKill;
    }

    void OnStartup() override
    {
        PvpRanks::EnsureSchema();
    }
};

// =====================================================================
//  Registration
// =====================================================================
void AddPvpRanksScripts()
{
    new pvprank_commandscript();
    new PvpRanksPlayerScript();
    new PvpRanksWorldScript();
}
