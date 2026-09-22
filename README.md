# mod-pvp-ranks

An [AzerothCore](https://www.azerothcore.org/) module (WotLK 3.3.5a) that adds a seasonal,
14-rank PvP progression system on top of honorable player kills, in the spirit of classic
WoW's old PvP rank ladder.

## What it does

Every time a character lands an honorable PvP kill, the killer earns points (capped per
week). Cumulative points map onto 14 ascending ranks, named by faction:

| Rank | Alliance              | Horde                |
|------|-----------------------|-----------------------|
| 1    | Private                | Scout                 |
| 2    | Corporal                | Grunt                  |
| 3    | Sergeant                | Sergeant               |
| 4    | Master Sergeant         | Senior Sergeant        |
| 5    | Sergeant Major          | First Sergeant         |
| 6    | Knight                  | Stone Guard            |
| 7    | Knight-Lieutenant       | Blood Guard            |
| 8    | Knight-Captain          | Legionnaire            |
| 9    | Knight-Champion         | Centurion              |
| 10   | Lieutenant Commander    | Champion               |
| 11   | Commander               | Lieutenant General     |
| 12   | Marshal                 | General                |
| 13   | Field Marshal           | Warlord                |
| 14   | Grand Marshal           | High Warlord           |

Progress is per-character, persists across logins, and resets each season (a configurable
season number - bump it to start a new one).

## Commands

| Command                | Access | Description                                             |
|-------------------------|--------|----------------------------------------------------------|
| `.pvprank`               | Player | Shows your current rank, points, weekly progress and cap |
| `.pvprank top [n]`       | Player | Lists the top `n` ranked characters this season (default 10, max 25) |
| `.pvprank resetseason`   | GM     | Immediately archives and resets every character to the current season |

## Configuration

`conf/mod_pvp_ranks.conf.dist`:

| Key                          | Default | Description                                                        |
|-------------------------------|---------|----------------------------------------------------------------------|
| `PvpRanks.Enable`              | `1`     | Master on/off switch                                                 |
| `PvpRanks.PointsPerKill`       | `10`    | Points awarded per honorable PvP kill                                |
| `PvpRanks.WeeklyCap`           | `2000`  | Max points a character can earn per week                             |
| `PvpRanks.Season`              | `1`     | Current season number                                                |
| `PvpRanks.CountBots`           | `1`     | Whether a playerbot's kills award rank points                        |
| `PvpRanks.AnnounceRankUp`      | `1`     | Broadcast a server-wide message on rank-up                           |

## Installation

Clone into your AzerothCore `modules/` directory and rebuild the worldserver. The module
creates its own database tables automatically on first startup - no SQL files to apply.

## License

Released under the GNU GPL v2 (or later).
