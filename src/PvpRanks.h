/*
 * mod-pvp-ranks - shared declarations.
 *
 * Seasonal, 14-rank PvP progression system (WoW-Forever style) layered on top of
 * honorable player kills. Per-character progress is stored in the characters DB
 * table `pvp_rank_points` (programmatic schema, see PvpRanks::EnsureSchema) and
 * cached in memory while the character is online.
 *
 * Released under GNU GPL v2; redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef MOD_PVP_RANKS_H
#define MOD_PVP_RANKS_H

#include <cstdint>
#include <string>

class Player;
enum TeamId : uint8_t;

namespace PvpRanks
{
    // Cached config (populated in WorldScript::OnAfterConfigLoad).
    struct Config
    {
        bool   Enable          = true;  // module master switch
        uint32 PointsPerKill   = 10;    // points awarded per honorable PvP kill
        uint32 WeeklyCap       = 2000;  // max points a character can earn per week
        uint32 Season          = 1;     // current season number
        bool   CountBots       = true;  // false = playerbot kills award no points
        bool   AnnounceRankUp  = true;  // broadcast to the server on a rank-up
    };

    Config& GetConfig();

    // Number of ranks in the ladder (1-based: rank 1 .. rank kRankCount).
    constexpr uint8_t kRankCount = 14;

    // Cumulative point threshold to HOLD each rank (index 0 == rank 1, always 0).
    // Roughly geometric, tuned so a character playing every week up to the
    // default WeeklyCap (2000) reaches rank 14 in about 12 weeks of a season.
    uint32 PointThresholdForRank(uint8_t rankIdx);

    // Classic-style rank title, picked by faction. rankIdx is 1-based (1..14).
    std::string const& RankName(uint8_t rankIdx, TeamId teamId);

    // Highest rank whose threshold `points` satisfies (points >= threshold).
    uint8_t RankForPoints(uint32 points);

    // Creates the programmatic pvp_rank_points (+ archive) tables in the
    // characters DB if they do not already exist. Safe to call repeatedly.
    void EnsureSchema();

    // Current weekly-cap "week" key: an epoch week number derived from
    // GameTime (UTC seconds / 604800), NOT a calendar ISO week. It still
    // changes exactly once every 7 days and is fully monotonic, which is all
    // the weekly cap needs - and it sidesteps ISO week / year-boundary edge
    // cases entirely.
    uint32 CurrentWeekKey();

    // Ensures a character has an in-memory row (loading/creating it from the
    // characters DB as needed) and applies the season reset if the row's
    // stored season no longer matches PvpRanks.Season. Call on OnPlayerLogin.
    void LoadOrCreate(Player* player);

    // Drops the character's in-memory row (already persisted eagerly on every
    // change, so this is just cache hygiene). Call on OnPlayerLogout.
    void Evict(Player* player);

    // Awards PvpRanks.PointsPerKill to `killer`, respecting the weekly cap and
    // rolling it over when the epoch week has changed. No-op if the module is
    // disabled, the weekly cap is already reached, or `killer` has no row
    // (i.e. LoadOrCreate was never called - shouldn't happen for an online
    // player). Persists immediately and announces a rank-up if configured.
    void AwardKillPoints(Player* killer);

    // Read-only snapshot used by the `.pvprank` command.
    struct RankSnapshot
    {
        bool     found        = false;
        uint32   points       = 0;
        uint8_t  rankIdx      = 1;
        uint32   weeklyPoints = 0;
        uint32   weeklyCap    = 0;
    };
    RankSnapshot GetSnapshot(Player* player);

    // Immediately archives and resets EVERY stored character to the currently
    // configured season / rank 1 / 0 points, regardless of what season is
    // currently stored for them. Used by the GM command `.pvprank resetseason`
    // to force a season rollover without waiting for each character's next
    // login. Returns the number of characters reset.
    uint32 ResetAllToCurrentSeason();
}

#endif // MOD_PVP_RANKS_H
