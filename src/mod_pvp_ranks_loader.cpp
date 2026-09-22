/*
 * mod-pvp-ranks loader.
 *
 * The playerbots fork auto-globs every module's sources into one lib and looks
 * up a loader symbol derived from the folder name: for folder "mod-pvp-ranks"
 * that symbol is exactly "Addmod_pvp_ranksScripts". It must exist and call our
 * real registration function.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

void AddPvpRanksScripts();

void Addmod_pvp_ranksScripts()
{
    AddPvpRanksScripts();
}
