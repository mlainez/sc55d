/*
 * ROM set selection and loading.  This mirrors what upstream does inside
 * mcu.cpp's main(): same file names (they come from the core's own table),
 * same autodetect order, same model flags, same unscrambling.  It lives here
 * because upstream's copy is welded to its SDL frontend's main().
 */
#include "romset.h"

#include <cstdio>
#include <cstring>

#include "core.h"
#include "lcd.h"
#include "pcm.h"
#include "submcu.h"

namespace Romset {
namespace {

const int kRomSetFiles = 6;

const int kRom1Size = 0x8000;
const int kRom2Size = 0x80000;
const int kRomSmSize = 0x1000;

struct ModelName {
    const char *flag;
    int romset;
};

const ModelName kModelNames[] = {
    { "mk2",      ROM_SET_MK2 },
    { "st",       ROM_SET_ST },
    { "mk1",      ROM_SET_MK1 },
    { "cm300",    ROM_SET_CM300 },
    { "jv880",    ROM_SET_JV880 },
    { "scb55",    ROM_SET_SCB55 },
    { "rlp3237",  ROM_SET_RLP3237 },
    { "sc155",    ROM_SET_SC155 },
    { "sc155mk2", ROM_SET_SC155MK2 },
};

std::string Join(const std::string &dir, const char *file)
{
    return dir + "/" + file;
}

bool ReadExact(FILE *file, void *dst, size_t size, const std::string &path)
{
    if (fread(dst, 1, size, file) == size)
        return true;
    fprintf(stderr, "sc55d: %s: short read, expected %zu bytes\n", path.c_str(), size);
    return false;
}

bool ReadScrambled(FILE *file, uint8_t *dst, size_t size, const std::string &path)
{
    if (!ReadExact(file, tempbuf, size, path))
        return false;
    unscramble(tempbuf, dst, (int)size);
    return true;
}

void ApplyModelFlags(int romset)
{
    mcu_mk1 = false;
    mcu_cm300 = false;
    mcu_st = false;
    mcu_jv880 = false;
    mcu_scb55 = false;
    mcu_sc155 = false;

    switch (romset)
    {
        case ROM_SET_MK2:
        case ROM_SET_SC155MK2:
            if (romset == ROM_SET_SC155MK2)
                mcu_sc155 = true;
            break;
        case ROM_SET_ST:
            mcu_st = true;
            break;
        case ROM_SET_MK1:
        case ROM_SET_SC155:
            mcu_mk1 = true;
            mcu_st = false;
            if (romset == ROM_SET_SC155)
                mcu_sc155 = true;
            break;
        case ROM_SET_CM300:
            mcu_mk1 = true;
            mcu_cm300 = true;
            break;
        case ROM_SET_JV880:
            mcu_jv880 = true;
            rom2_mask /= 2; // rom is half the size
            lcd_width = 820;
            lcd_height = 100;
            lcd_col1 = 0x000000;
            lcd_col2 = 0x78b500;
            break;
        case ROM_SET_SCB55:
        case ROM_SET_RLP3237:
            mcu_scb55 = true;
            break;
        default:
            break;
    }
}

} // namespace

bool Parse(const std::string &name, int *romset)
{
    for (const ModelName &model : kModelNames)
    {
        if (name == model.flag)
        {
            *romset = model.romset;
            return true;
        }
    }
    return false;
}

int Autodetect(const std::string &dir)
{
    for (int i = 0; i < ROM_SET_COUNT; i++)
    {
        bool good = true;
        for (int j = 0; j < 5; j++)
        {
            if (roms[i][j][0] == '\0')
                continue;
            FILE *file = fopen(Join(dir, roms[i][j]).c_str(), "rb");
            if (!file)
            {
                good = false;
                break;
            }
            fclose(file);
        }
        if (good)
            return i;
    }
    return -1;
}

const char *Name(int romset)
{
    return rs_name[romset];
}

void PrintExpectedFiles(const std::string &dir, int romset)
{
    fprintf(stderr, "sc55d: %s ROM files expected in %s:\n", Name(romset), dir.c_str());
    for (int i = 0; i < kRomSetFiles; i++)
    {
        if (roms[romset][i][0] == '\0')
            continue;
        const bool optional = (romset == ROM_SET_JV880 && i >= 4);
        fprintf(stderr, "  %s%s\n", roms[romset][i], optional ? " (optional)" : "");
    }
}

bool Load(const std::string &dir, int romset)
{
    ApplyModelFlags(romset);

    FILE *files[kRomSetFiles] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    std::string paths[kRomSetFiles];
    bool missing = false;

    for (int i = 0; i < kRomSetFiles; i++)
    {
        if (roms[romset][i][0] == '\0')
            continue;
        paths[i] = Join(dir, roms[romset][i]);
        files[i] = fopen(paths[i].c_str(), "rb");
        const bool optional = mcu_jv880 && i >= 4;
        if (!files[i] && !optional)
        {
            fprintf(stderr, "sc55d: missing ROM file: %s\n", paths[i].c_str());
            missing = true;
        }
    }

    bool ok = !missing;

    if (ok)
        ok = ReadExact(files[0], rom1, kRom1Size, paths[0]);

    if (ok)
    {
        const size_t rom2_read = fread(rom2, 1, kRom2Size, files[1]);
        if (rom2_read == kRom2Size || rom2_read == kRom2Size / 2)
        {
            rom2_mask = (int)rom2_read - 1;
        }
        else
        {
            fprintf(stderr, "sc55d: %s: unexpected size %zu\n", paths[1].c_str(), rom2_read);
            ok = false;
        }
    }

    if (ok && mcu_mk1)
    {
        ok = ReadScrambled(files[2], waverom1, 0x100000, paths[2])
             && ReadScrambled(files[3], waverom2, 0x100000, paths[3])
             && ReadScrambled(files[4], waverom3, 0x100000, paths[4]);
    }
    else if (ok && mcu_jv880)
    {
        ok = ReadScrambled(files[2], waverom1, 0x200000, paths[2])
             && ReadScrambled(files[3], waverom2, 0x200000, paths[3]);

        if (ok && files[4] && fread(tempbuf, 1, 0x800000, files[4]))
            unscramble(tempbuf, waverom_exp, 0x800000);
        else if (ok)
            printf("sc55d: WaveRom EXP not found, skipping it.\n");

        if (ok && files[5] && fread(tempbuf, 1, 0x200000, files[5]))
            unscramble(tempbuf, waverom_card, 0x200000);
        else if (ok)
            printf("sc55d: WaveRom PCM card not found, skipping it.\n");
    }
    else if (ok)
    {
        ok = ReadScrambled(files[2], waverom1, 0x200000, paths[2]);

        if (ok && files[3])
            ok = ReadScrambled(files[3], mcu_scb55 ? waverom3 : waverom2, 0x100000, paths[3]);

        if (ok && files[4])
            ok = ReadExact(files[4], sm_rom, kRomSmSize, paths[4]);
    }

    for (int i = 0; i < kRomSetFiles; i++)
    {
        if (files[i])
            fclose(files[i]);
    }

    return ok;
}

} // namespace Romset
